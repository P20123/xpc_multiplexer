#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <tinyxpc/tinyxpc.h>

/**
 * XPC Relay definitions
 *
 * The XPC Relay is the connection-state manager for a single point-to-point
 * TinyXPC session.  The use of function pointers for all io calls and handlers
 * allows it to be integrated into any event system and does not impose memory
 * system requirements on the connected environment.
 */

typedef union {
    char *write_buf;
    char **read_buf;
} io_buf_t;

/**
 * IO wrapping function type declaration.  The XPC Relay uses functions of this
 * type to abstract read and write operations to a single endpoint.
 * The pointer at buffer should be left unchanged in the write call, but should
 * be changed to the location of the new data in a read call.
 * @param io_ctx will be passed to the io wrapper when it is called. The value
 * will be that of the io_ctx in the XPC Relay when it was configured.
 * @param buffer mutable pointer to either data (write) or NULL (read).
 * @param bytes_max the size of data to write from buffer, or to read from the
 * IO stream.  On a read, this is the number of bytes that should be read from
 * the stream. The value of bytes_max will decrease by whatever value the
 * read function returns on each successive call until a full message is
 * present in the buffer. The relay will only read one message at a time, and as
 * such the IO functions must maintain their own state and memory.
 * @return the actual number of bytes read or written.
 */
typedef int (io_wrap_fn)(void *io_ctx, io_buf_t *buffer, size_t bytes_max);

/**
 *
 */
typedef bool (dispatch_fn)(void *msg_ctx, txpc_hdr_t *msg_hdr, int status);

enum {
    TXPC_OP_NONE,
    TXPC_OP_RESET,
    TXPC_OP_MSG,
    TXPC_OP_STOP,
    TXPC_OP_WAIT_RESET
};

enum {
    TXPC_STATUS_DONE,
    TXPC_STATUS_INFLIGHT,
    TXPC_STATUS_BAD_STATE
};

typedef struct {
    bool use_le;
    bool connection_ready;
    int crc_bits;
    void *io_ctx;
    void *msg_ctx;
    io_wrap_fn *write;
    io_wrap_fn *read;
    dispatch_fn *dispatch_cb;
    struct {
        int op;
        int bytes_complete;
        txpc_hdr_t msg_hdr;
        io_buf_t buf;
    } inflight_op;
} xpc_relay_state_t;

/**
 * Configure the XPC Relay for a new connection.
 *
 * @param target pointer to preallocated contiguous memory for state. The memory
 * at this pointer is always changed unless target is NULL.
 * @param io_ctx pointer to the context for the IO read/write callbacks.
 * @param msg_ctx pointer to the context for handling message events
 * (recvd, failed, disconnect)
 * @param write the IO wrapper for writing a byte stream to the endpoint
 * @param read the IO wrapper for reading a byte stream from an endpoint
 * @param msg_handle_cb message handler callback function, called on any
 * message event.
 *
 * @return target, or NULL on failure.
 */
xpc_relay_state_t *xpc_relay_config(
    xpc_relay_state_t *target, void *io_ctx, void *msg_ctx,
    io_wrap_fn *write, io_wrap_fn *read, dispatch_fn *msg_handle_cb
);

/**
 *
 */
int xpc_relay_send_reset(xpc_relay_state_t *self);

/**
 *
 */
int xpc_relay_send_disconnect(xpc_relay_state_t *self);

/**
 *
 */
int xpc_send_msg(xpc_relay_state_t *self);

/**
 * Continue whatever operation is currently inflight.
 * This function should be called in the main event loop of the enclosing
 * application.  It doubles as the read handler, and will call the message
 * handler callback when a complete message has been received.
 * Any inflight write operations are also updated in this function.
 * @param self XPC Relay context
 * @return 0 if all operations are complete, 1 otherwise.
 */
int xpc_op_continue(xpc_relay_state_t *self);

/**
 * FIXME continue should probably not "double" as the read function, just make
 * them separate.
 */
int xpc_read_bytes(xpc_relay_state_t *self);

// not supporting streams right now.  we are assuming in-order message
// processing for the standard messages, so any nacks are for the most recently
// sent message. (not that we can do anything about it.)
