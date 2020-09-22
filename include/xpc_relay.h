#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <tinyxpc/tinyxpc.h>
// NOTE:
// not supporting streams right now.  we are assuming in-order message
// processing for the standard messages, so any nacks are for the most recently
// sent message. (not that we can do anything about it.)

/**
 * XPC Relay definitions
 *
 * The XPC Relay is the connection-state manager for a single point-to-point
 * TinyXPC session.  The use of function pointers for all io calls and handlers
 * allows it to be integrated into any event system and does not impose memory
 * system requirements on the connected environment.
 */

/**
 * The XPC Relay is implemented as two parallel state machines which interact
 * with each other through limited signals.  In TinyXPC, messages are atomic, so
 * the state of either machine cannot be interrupted until the state is
 * TXPC_OP_NONE, meaning no message is currently inflight.
 */


/**
 * IO wrapping function type declaration.  The XPC Relay uses functions of this
 * type to abstract read and write operations to a single endpoint.
 *
 * Write calls: buffer points to a char * of length bytes_max
 * Read calls: if buffer points to non-null, read bytes_max bytes into the
 * region at *buffer, else save to a dynamic region, and write *buffer such that
 * it points to the first byte of data read.
 *
 * The purpose of having the offset is to allow the io structure to be opaque
 * to the relay. The relay may change this value at any time, and it may also
 * change the buffer location between subsequent calls in the same message.
 *
 * @param io_ctx will be passed to the io wrapper when it is called. The value
 * will be that of the io_ctx in the XPC Relay when it was configured.
 * @param buffer mutable pointer to either data (write) or NULL (read).
 * @param offset number of bytes into buffer to start io at.
 * @param bytes_max the size of data to write from buffer, or to read from the
 * IO stream.  On a read, this is the number of bytes that should be read from
 * the stream. The value of bytes_max will decrease by whatever value the
 * read function returns on each successive call until a full message is
 * present in the buffer. The relay will only read one message at a time, and as
 * such the IO functions must maintain their own state and memory.
 * @return the actual number of bytes read or written.
 */
typedef int (io_wrap_fn)(void *io_ctx, char **buffer, int offset, size_t bytes_max);

/**
 * Function type for reset callbacks.  When a full message has been received,
 * the xpc relay will inform the IO subsystem that it can discard that size in
 * its input buffer. The same can be done when the xpc relay is finished with
 * a transmission - this allows IO calls to be buffered in systems where that
 * is advantageous.
 * @param io_ctx io_ctx set at initialization of xpc_relay_config
 * @param which 0 if read, 1 if write
 * @param bytes the number of bytes which can be discarded. -1 if all bytes
 * must be discarded.
 */
typedef void (io_reset_fn)(void *io_ctx, int which, size_t bytes);

/**
 * Function type for incoming message handling.
 */
typedef bool (dispatch_fn)(void *msg_ctx, txpc_hdr_t *msg_hdr, char *payload);

typedef enum {
    TXPC_OP_NONE,
    TXPC_OP_RESET,
    TXPC_OP_MSG,
    TXPC_OP_STOP,
    TXPC_OP_SET_CRC,
    TXPC_OP_SET_ENDIANNESS,
    // these are aliased to minimize the state variable size, but are more
    // readable when looking through the read state machine.
    TXPC_OP_WAIT_RESET = TXPC_OP_RESET,
    TXPC_OP_WAIT_MSG = TXPC_OP_MSG,
    TXPC_OP_WAIT_CRC = TXPC_OP_SET_CRC,
    TXPC_OP_WAIT_ENDIANNESS = TXPC_OP_SET_ENDIANNESS,
    TXPC_OP_WAIT_DISPATCH
} xpc_sm_state_t;

typedef enum {
    TXPC_STATUS_DONE,
    TXPC_STATUS_INFLIGHT,
    TXPC_STATUS_BAD_STATE
} xpc_status_t;

typedef struct {
    // global state for the xpc connection
    bool use_le;
    bool connection_ready;
    int crc_bits;
    // contexts for the io subsystem and application message handler
    void *io_ctx;
    void *msg_ctx;
    // function pointers for io and application message handling
    io_wrap_fn *write;
    io_wrap_fn *read;
    io_reset_fn *io_reset;
    dispatch_fn *dispatch_cb;
    // TODO add a CRC function
    // TODO figure out crc callback points
    // These are signals between the two state machines.
    // the _SEND signals are asserted by the entry point functions, and not
    // by the write state machine.
    // FIXME how do we make sync the read and write sms so they don't do
    // an infinite reset loop?
    // Only RST, CRC, and ENDIANNESS are ack'd with a clone response.
    // thus, the entry point functions ASSERT those _SEND signals, and
    // xpc_op_read_continue DEASSERTS them when the ack is received.
    // all others, xpc_op_write_continue deasserts after message transmission
    // is complete.
    // We do NOT allow messages to be sent while waiting for an ack.
    enum int_sig_t {
        // a reset message was received from the endpoint, invalidate write
        // state.
        SIG_RST_RECVD = 1,
        // this endpoint is sending a reset message, invalidate read state.
        SIG_RST_SEND = (1 << 1),
        // a disconnect message was received from the endpoint.
        SIG_DISC_RECVD = (1 << 2),
        // this endpoint is sending a disconnect message, invalidate read state.
        SIG_DISC_SEND = (1 << 3),
        SIG_CRC_RECVD = (1 << 4),
        SIG_CRC_SEND = (1 << 5),
        SIG_ENDIANNESS_RECVD = (1 << 6),
        SIG_ENDIANNESS_SEND = (1 << 7)
    } signals;

    // internal state for both state machines is identical.
    struct xpc_sm_t {
        xpc_sm_state_t op;
        int total_bytes;
        int bytes_complete;
        txpc_hdr_t msg_hdr;
        char *buf;
    } inflight_wr_op, inflight_rd_op;
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
    io_wrap_fn *write, io_wrap_fn *read, io_reset_fn *reset,
    dispatch_fn *msg_handle_cb
);

/**
 *
 */
xpc_status_t xpc_relay_send_reset(xpc_relay_state_t *self);

/**
 *
 */
xpc_status_t xpc_relay_send_disconnect(xpc_relay_state_t *self);

/**
 *
 */
xpc_status_t xpc_send_msg(xpc_relay_state_t *self, uint8_t to, uint8_t from, char *data, size_t bytes);

/**
 * Attempt to continue the currently inflight write operation.  State changes
 * are also enacted by this function for any write-related state transitions.
 * This function should be called when the stream associated with this
 * xpc connection is ready for writing.
 * @param self XPC Relay context
 * @return 0 if all operations are complete (no messages inflight), 1 otherwise.
 */
xpc_status_t xpc_wr_op_continue(xpc_relay_state_t *self);

/**
 * Attempt to continue the currently inflight read operation.  State changes
 * are also enacted by this function for any read-related state transitions.
 * This function should be called when the stream associated with this
 * xpc connection is ready for reading.
 * @param self XPC Relay context
 * @return 0 if all operations are complete (no messages inflight), 1 otherwise.
 */
xpc_status_t xpc_rd_op_continue(xpc_relay_state_t *self);
