#include <stdlib.h>
#include <errno.h>
#include <sys/epoll.h>
#include <epoll_app.h>
#include <alibc/containers/array.h>
#include <alibc/containers/array_iterator.h>
#include <alibc/containers/iterator.h>

epoll_app_t *create_epoll_app(int close_on_exec, void *callback_ctx) {
    epoll_app_t *r = malloc(sizeof(epoll_app_t));
    if(r == NULL) {
        goto done;
    }

    // initialize epoll
    r->epoll_fd = epoll_create1(close_on_exec);
    if(r->epoll_fd == -1) {
        free(r);
        goto done;
    }

    // initialize the epoll events list
    r->event_list = create_array(4, sizeof(struct epoll_event));
    if(r->event_list == NULL) {
        free(r);
        r = NULL;
        goto done;
    }

    // initialize the epoll active events buffer
    r->event_buffer = create_array(4, sizeof(struct epoll_event));
    if(r->event_buffer == NULL) {
        array_free(r->event_list);
        free(r);
        r = NULL;
        goto done;
    }
    r->cb_ctx = callback_ctx;
    r->run_mainloop = true;
done:
    return r;
}

int epoll_app_add_fd(epoll_app_t *app, int fd, int flags) {
    int fd_index = array_size(app->event_list);
    struct epoll_event epoll_temp = {0};
    epoll_temp.events = flags;
    // NOTE: this can be used to store any pointer and will be available
    // from epoll_wait when it unblocks
    epoll_temp.data = (epoll_data_t)fd;

    // alc will copy the local structure
    array_append(app->event_list, &epoll_temp);
    // ensure there is space for epoll to have all fds active after epoll_wait()
    array_resize(app->event_buffer, array_size(app->event_list));

    // add event_list to epoll
    int r = epoll_ctl(
        app->epoll_fd, EPOLL_CTL_ADD, fd,
        // get the permanent address back from the array
        (struct epoll_event*)array_fetch(
            app->event_list, fd_index
        )
    );
    if(r == 0) goto done;
    switch(errno) {
        case EEXIST:
            // user probably meant to use EPOLL_CTL_MOD...
            // call mod with the same args
            return epoll_app_mod_fd(app, fd, flags);
        break;
        
        case EINVAL:
            // epfd is wrong, fd is wrong, or op + flags is wrong
        case EBADF:
            // epfd or fd is bad
        case ELOOP:
            // epoll monitoring another epoll.
        case ENOMEM:
            // could not add, no memory
        case ENOSPC:
            // no more watches available for this user
        case EPERM:
            // fd does not refer to an epoll instance, app context is broken.
            fd_index = -1;
        break;

    }
done:
    return fd_index == -1 ? -1:0;
}

int epoll_app_del_fd(epoll_app_t *app, int fd) {
    int fd_index = 0;
    bool found = false;
    struct epoll_event epoll_temp = {0};
    // search for the requested fd in the event list, and remove it.
    for(fd_index = 0; fd_index < array_size(app->event_list); fd_index++) {
        epoll_temp = *(struct epoll_event *)array_fetch(
            app->event_list, fd_index
        );
        if(epoll_temp.data.u32 == fd) {
            array_remove(app->event_list, fd_index);
            found = true;
            break;
        }
    }
    if(!found) {
        // couldn't find that event, don't bother with epoll
        goto done;
    }

    // remove the fd from epoll's interest list
    int r = epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    if(r == 0) goto done;
    switch(errno) {
        case EINVAL:
            // epfd is wrong, fd is wrong, or op + flags is wrong
        case EBADF:
            // epfd or fd is bad
        case EPERM:
            // fd does not refer to an epoll instance, app context is broken.
            fd_index = -1;
        break;

    }
done:
    return fd_index == -1 ? -1:0;
}

int epoll_app_mod_fd(epoll_app_t *app, int fd, int flags) {
    int status = 0;
    struct epoll_event *epoll_temp = epoll_app_get_fd_events(app, fd);
    if(epoll_temp == NULL) {
        // couldn't find that fd, caller likely meant to call add
        status = epoll_app_add_fd(app, fd, flags);
        goto done;
    }
    // ask epoll to modify the events monitored for this fd
    int r = epoll_ctl(
        app->epoll_fd, EPOLL_CTL_MOD, fd,
        epoll_temp
    );
    if(r == 0) goto done;
    switch(errno) {
        case EINVAL:
            // epfd is wrong, fd is wrong, or op + flags is wrong
        case EBADF:
            // epfd or fd is bad
        case ELOOP:
            // epoll monitoring another epoll.
        case ENOMEM:
            // could not add, no memory
        case ENOSPC:
            // no more watches available for this user
        case EPERM:
            // fd does not refer to an epoll instance, app context is broken.
            status = -1;
        break;

    }
done:
    return status;
}
struct epoll_event *epoll_app_get_fd_events(epoll_app_t *app, int fd) {
    struct epoll_event *r = NULL;
    iter_context *it = create_array_iterator(app->event_list);
    for(struct epoll_event *e = iter_next(it); e; e = iter_next(it)) {
        if(e->data.fd == fd) {
            r = e;
            break;
        }
    }
done:
    return r;
}

void epoll_app_close_all(epoll_app_t *app) {
    // normal cleanup
    iter_context *it = create_array_iterator(app->event_list);
    for(struct epoll_event *ev = iter_next(it); ev; ev = iter_next(it)) {
        // relevant fd was stored in data member
        // the valgrind error here is okay, it's because of the NULL
        int ep_status = epoll_ctl(
            app->epoll_fd, ev->data.fd, EPOLL_CTL_DEL, NULL
        );
        if(ep_status == 0) free(ev);
    }
    iter_free(it);
}

void destroy_epoll_app(epoll_app_t *app) {
    if(app != NULL) {
        epoll_app_close_all(app);
        close(app->epoll_fd);
        array_free(app->event_buffer);
        array_free(app->event_list);
        free(app);
    }
    return;
}

void epoll_app_mainloop(epoll_app_t *app) {
    while(app->run_mainloop) {
        int epoll_r = epoll_wait(
            app->epoll_fd,
            (struct epoll_event *)(app->event_buffer->data->buf),
            // use event list to get number of actual events in interest list
            array_size(app->event_list),
            -1 // block forever if no data is available
        );
        if(epoll_r == -1) {
            perror("epoll_wait");
            break;
        }
        // XXX this is not pretty, modifies the array size so normal operations
        // only act on what epoll actually put in the buffer.
        app->event_buffer->size = epoll_r;
        iter_context *it = create_array_iterator(app->event_buffer);
        for(struct epoll_event *ev = iter_next(it); ev; ev = iter_next(it)) {
            int curr_fd = (int)ev->data.u32;
            if(ev->events & EPOLLIN) {
                // read event
                if(app->epollin_cb != NULL) {
                    app->epollin_cb(app->cb_ctx, curr_fd);
                }
            }
            if(ev->events & EPOLLOUT) {
                // write event
                if(app->epollout_cb != NULL) {
                    app->epollout_cb(app->cb_ctx, curr_fd);
                }
            }
            if(ev->events & EPOLLRDHUP) {
                // read hangup / peer closed connection
                if(app->epollrdhup_cb != NULL) {
                    app->epollrdhup_cb(app->cb_ctx, curr_fd);
                }
            }
            if(ev->events & EPOLLPRI) {
                // exceptional condition
                if(app->epollpri_cb != NULL) {
                    app->epollpri_cb(app->cb_ctx, curr_fd);
                }
            }
            if(ev->events & EPOLLERR) {
                // write on read-closed fifo or other error
                if(app->epollerr_cb != NULL) {
                    app->epollerr_cb(app->cb_ctx, curr_fd);
                }
            }
            if(ev->events & EPOLLHUP) {
                // hangup
                if(app->epollhup_cb != NULL) {
                    app->epollhup_cb(app->cb_ctx, curr_fd);
                }
            }
        }
        iter_free(it);
    }
}
