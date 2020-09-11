#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <errno.h>
#include <signal.h>
#include <tinyxpc/tinyxpc.h>
#include <alibc/containers/array.h>
#include <alibc/containers/array_iterator.h>
#include <alibc/containers/iterator.h>
#include <xpc_utils.h>
#include <epoll_app.h>

epoll_app_t *global_context;

static const int epoll_rd_flags = EPOLLIN | EPOLLHUP | EPOLLRDHUP;
static const int epoll_wr_flags = EPOLLOUT | EPOLLHUP;
static const int epoll_rdwr_flags = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP;

static int app_set_rd_notify(void *ctx, int fd, bool enable) {
    struct epoll_event *events = epoll_app_get_fd_events(ctx, fd);
    if(events == NULL) {
        epoll_app_add_fd(ctx, fd, epoll_rd_flags);
    }
    else {
        if(enable) {
            events->events |= (EPOLLIN);
        }
        else {
            events->events &= ~(EPOLLIN);
        }
        epoll_app_mod_fd(ctx, fd, events->events);
    }
    return 0;
}

static int app_set_wr_notify(void *ctx, int fd, bool enable) {
    struct epoll_event *events = epoll_app_get_fd_events(ctx, fd);
    if(events == NULL) {
        epoll_app_add_fd(ctx, fd, epoll_wr_flags);
    }
    else {
        if(enable) {
            events->events |= (EPOLLOUT);
        }
        else {
            events->events &= ~(EPOLLOUT);
        }
        epoll_app_mod_fd(ctx, fd, events->events);
    }
    return 0;
}



static void unix_signal_handler(int signum) {
    switch(signum) {
        case SIGINT:
            global_context->run_mainloop = false;
        break;

        default:
        fprintf(stderr, "got unhandled signal: %s\n", strsignal(signum));
        break;
    }
}


int configure_device(char *dev_path, int baud, int flags) {
    int fd = -1;
    fd = open(dev_path, O_RDWR | O_NOCTTY /*| O_NONBLOCK*/);
    if(fd == -1) {
        goto done;
    }

    // set flags
    fcntl(fd, F_SETFL, flags);

    // set the port as being exclusive
    ioctl(fd, TIOCEXCL, NULL);

    struct termios options;
    tcgetattr(fd, &options);
    cfmakeraw(&options);
    // FIXME there has to be a better way to set these baud rates.
    cfsetspeed(&options, B115200);
    tcsetattr(fd, TCSANOW, &options);

done:
    return fd;
}

int main(int argc, char **argv) {
    int status = 0;

    struct sigaction sa;
    // set up unix signals
    sa.sa_handler = unix_signal_handler;
    sigfillset(&sa.sa_mask);
    sigdelset(&sa.sa_mask, SIGINT);
    // restart "interruptable" functions after the handler returns
    sa.sa_flags = SA_RESTART;
    if(sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        status = -1;
        goto done;
    }

    // application state init
    epoll_app_t *app = create_epoll_app(0, NULL);
    if(app == NULL) {
        status = -2;
        goto done;
    }
    global_context = app;


    // tell epoll AND xpc about INPUTS, tell ONLY xpc about OUTPUTS.
    // xpc has the smarts to turn off write events when no data is available.
    int ser_fd = configure_device(argv[1], 921600, 0 /*FNDELAY*/);
    if(ser_fd == -1) {
        perror("open");
        status = -4;
        goto bad_device;
    }

    // TODO how do we handle fds that are in AND out?
    // we're going to need EVEN MORE STATE LOGIC
    // this was rdwr_flags
    /*epoll_app_add_fd(app, ser_fd, epoll_rd_flags);*/

    // make a fifo for the demux'd output
/*
 *    if(mkfifo("k64_stdout", 0660) < 0 && errno != EEXIST) {
 *        perror("mkfifo k64_stdout");
 *        status = -5;
 *        goto bad_device;
 *    }
 *
 */
    // we are only writing to the fifo
/*
 *    int k64out_fd = open("./k64_stdout", O_WRONLY);
 *    if(k64out_fd == -1) {
 *        perror("open k64_stdout");
 *        status = -5;
 *        goto bad_device;
 *    }
 *
 *    epoll_app_add_fd(app, k64out_fd, EPOLLOUT | EPOLLHUP);
 */
    /*epoll_app_add_fd(app, STDOUT_FILENO, epoll_wr_flags);*/

    // make a fifo for mux'd input
/*
 *    if(mkfifo("k64_stdin", 0660) < 0 && errno != EEXIST) {
 *        perror("mkfifo k64_stdin");
 *        status = -5;
 *        goto bad_device;
 *    }
 *    int k64in_fd = open("./k64_stdin", O_RDONLY | O_NONBLOCK);
 *    if(k64in_fd == -1) {
 *        perror("open k64_stdin");
 *        status = -5;
 *        goto bad_device;
 *    }
 *
 *    epoll_app_add_fd(app, k64in_fd, EPOLLIN|EPOLLHUP|EPOLLRDHUP);
 */

    // configure the xpc router
    xpc_router_t *xpc = initialize_xpc_router();
    if(xpc == NULL) {
        goto bad_device;
    }
    // allow xpc to talk to epoll_app
    xpc->io_event_context = app;
    /*
     *xpc->io_add_fd_cb = app_add_fd;
     *xpc->io_del_fd_cb = app_del_fd;
     */
    xpc->io_notify_read = app_set_rd_notify;
    xpc->io_notify_write = app_set_wr_notify;

    // use xpc to handle epoll_app
    app->cb_ctx = xpc;
    app->epollin_cb = xpc_accumulate_msg;
    app->epollout_cb = xpc_write_msg;
    xpc_set_route(xpc, ser_fd, STDOUT_FILENO, 1, 1); 
    xpc_set_route(xpc, ser_fd, ser_fd, 1, 1); 

    epoll_app_mainloop(global_context);

    xpc_router_destroy(xpc);
// early exit conditions
bad_device:
    destroy_epoll_app(app);
done:
    return status;
}
