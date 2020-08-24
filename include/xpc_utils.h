#pragma once
#include <stdbool.h>
#include <tinyxpc/tinyxpc.h>
#include <xpc_msg_queue.h>
#include <alibc/containers/dynabuf.h>
#include <alibc/containers/array.h>
#include <alibc/containers/hashmap.h>
/**
 * Keys and values for the switching table.
 * The supported mapping is (fd, to_channel) -> (fd, to_channel).
 */
typedef struct {
    int fd;
    int to_chn;
    // not supporting routing functions right now, but that could be useful
    // for doing things like ioctls on serial ports if necessary, and
    // interpreting custom xpc message types
} xpc_switch_tbl_entry_t;


/**
 * Information required to describe the state of reading from a single source.
 */
typedef struct {
    // otherwise pass buf_id to xpc_msg_getbuf
    bool msg_inflight;
    // temporary buffer for receiving a message when only the incoming fd
    // is known.
    txpc_hdr_t msg_hdr;
    // this is passed to xpc_msg_getbuf
    int buf_id;
    // this is the offset for reading (from an fd, into a buffer)
    int buf_offset;
} xpc_in_ctx_t;

/**
 * Information describing the state of output to a file descriptor.
 */
typedef struct {
    msg_queue_t *msg_queue;
    // the id that is currently being written.
    int current_buf_id;
} xpc_out_ctx_t;


typedef struct {
    uint32_t crc_polyn;
    bool big_endian;
    hashmap_t *in_contexts;
    hashmap_t *out_contexts;
    hashmap_t *switch_tbl;

    /**
     * These items are needed for controlling event-based IO.
     */
    // XXX flags in epoll_app need to be kept in the context somehow.
    // we should adjust this so that the functions ask the io event manager to
    // notify on read, write, or none events.
    void *io_event_context;
    int (*io_notify_read)(void *ctx, int fd, bool enable);
    int (*io_notify_write)(void *ctx, int fd, bool enable);
} xpc_router_t;


xpc_out_ctx_t *create_xpc_out_ctx(xpc_out_ctx_t *target);

void xpc_out_ctx_free(xpc_out_ctx_t *self);

/**
 * Create a new xpc router, initializing its data structures.
 * This does not automatically set any callbacks or routes.
 */
xpc_router_t *initialize_xpc_router();

/**
 * Free all structures associated with the given xpc router
 * @param ctx the router to destroy
 */
void xpc_router_destroy(xpc_router_t *ctx);
/**
 * Accumulate a message from a file descriptor, determine which output
 * descriptor it is going to, and read available data from the fd.
 * The data is read into a message buffer taken from the appropriate output fd's
 * message queue. Buffers associated with completed messages are automatically
 * cleared.
 * @param ctx the router context to use
 * @param fd the file descriptor to read from
 */
int xpc_accumulate_msg(xpc_router_t *ctx, int fd);

/**
 * Write as much of a message as possible to the specified fd.
 * Data is only written if it is available for the specified fd, no other
 * fds are tried.
 * @param ctx the router context to use
 * @param fd a file descriptor which is ready for writing.
 */
int xpc_write_msg(xpc_router_t *ctx, int fd);

/**
 * Set up the path for messages coming from a particular fd and channel
 */
int xpc_set_route(xpc_router_t *ctx, int ifd, int ofd, int ito, int oto);

/**
 * Remove the specified route, disabling messages going to that destination.
 */
int xpc_remove_route(xpc_router_t *ctx, int ifd, int ito);
