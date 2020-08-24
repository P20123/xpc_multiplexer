#pragma once
/**
 * A module for writing applications using the epoll API.
 */

#include <stdbool.h>
#include <stdint.h>

#include <sys/epoll.h>

#include <alibc/containers/array.h>

/**
 * Function type for epoll_app callbacks.
 * @param context the context set in the call to create_epoll_app
 * @param fd integer file descriptor on which an event was triggered.
 */
typedef void (epoll_cb_t)(void *context, int fd);
/**
 * Application state.
 * The callbacks are called when the event corresponding to their name is
 * triggered on a file descriptor.  Any NULL callbacks will simply ignore that
 * event type, though, epoll will continue to fire that event if it is not
 * handled.
 */
typedef struct {
    int epoll_fd;
    bool run_mainloop;
    array_t *event_list;
    array_t *event_buffer;
    void *cb_ctx;
    epoll_cb_t *epollin_cb;
    epoll_cb_t *epollout_cb;
    epoll_cb_t *epollrdhup_cb;
    epoll_cb_t *epollpri_cb;
    epoll_cb_t *epollerr_cb;
    epoll_cb_t *epollhup_cb;
} epoll_app_t;

/**
 * Create a new epoll context in the kernel for this process, and initialize
 * the state required to handle it in this process.
 * @param close_on_exec 1 to set EPOLL_CLOEXEC, 0 otherwise
 * @param callback_ctx the context which will be passed to callback functions
 * when an event is triggered.
 * @return Initialized epoll_app_t, or NULL on failure.
 */
epoll_app_t *create_epoll_app(int close_on_exec, void *callback_ctx);

/**
 * Add a new file descriptor to the interest list of epoll for this process.
 * @param app previously initialized app context
 * @param fd the file descriptor (already open()'d)
 * @param flags epoll flags from epoll_ctl(2)
 * @return 0 on success, -1 if, after calling epoll_ctl, errno is
 *  EINVAL, EBADF, ELOOP, ENOMEM, ENOSPC, or EPERM
 *
 * If epoll_ctl returns EEXIST, epoll_app_mod_fd will be called instead.
 */
int epoll_app_add_fd(epoll_app_t *app, int fd, int flags);

/**
 * Remove a file descriptor from the interest list of this epoll context.
 * @param app the epoll_app controlling this fd
 * @param fd the file descriptor which should be removed.
 * @return number of remaining file descriptors in the interest list
 */
int epoll_app_del_fd(epoll_app_t *app, int fd);

/**
 * Modify the flags associated with a previously added file descriptor
 * @param app the epoll_app to use
 * @param fd the file descriptor whose flags are to be modified
 * @param flags the flags which will now be associated with the specified fd.
 * @return 0 on success, -1 if, after calling epoll_ctl, errno is
 *  EINVAL, EBADF, ELOOP, ENOMEM, ENOSPC, or EPERM
 */
int epoll_app_mod_fd(epoll_app_t *app, int fd, int flags);

/**
 * Retrieve the struct epoll_event associated with the given fd.
 * @param app the epoll_app to use
 * @param fd the file descriptor of interest
 * @return pointer to struct epoll_event, or NULL on failure.
 */
struct epoll_event *epoll_app_get_fd_events(epoll_app_t *app, int fd);

/**
 * Close all file descriptors associated with this application context.
 * @param app previously initialized epoll_app_t
 */
void epoll_app_close_all(epoll_app_t *app);

/**
 * Destroy the epoll_app, close all file descriptors, free associated memory.
 * @param app the epoll_app to destroy
 */
void destroy_epoll_app(epoll_app_t *app);

/**
 * Run the main loop.  This function will block until run_mainloop in the
 * pre-initialized application context is set to false, which can be done
 * by a signal handler, or by one of the handler functions when it is called.
 * @param app the application context to run.
 */

void epoll_app_mainloop(epoll_app_t *app);
