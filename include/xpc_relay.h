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
 * Function type for IO notification flow control.  The XPC Relay will call
 * these to inform the IO subsystem when to disable read/write notifications.
 * @param io_ctx io_ctx set at initialization of xpc_relay_config
 * @param which 0 if read, 1 if write
 * @param enable true if the relay should be notified of events, false otherwise
 */
typedef void (io_notify_config)(void *io_ctx, int which, bool enable);


/**
 * Function type for incoming message handling.
 */
typedef bool (dispatch_fn)(void *msg_ctx, txpc_hdr_t *msg_hdr, char *payload);


/**
 * Function type for computing a CRC.  The returned pointer should contain
 * a CRC of the number of bits specified by crc_bits.
 * @param crc_ctx context for the CRC subsystem, as passed in xpc_relay_config.
 * @param buf pointer to contiguous memory to compute the CRC of.
 * @param bytes number of bytes at buf to compute CRC of.
 * @return storage location of the computed CRC.
 */
typedef char *(crc_fn)(void *crc_ctx, char *buf, size_t bytes);

/**
 * Function to change the generator polynomial for the CRC subsystem.
 * @param crc_ctx context for the CRC subsystem, as passed in xpc_relay_config.
 * @param crc_bits number of bits in the CRC generator polynomial.
 * @param polyn pointer to contiguous memory containing the coefficients.
 */
typedef void (crc_polyn_config)(void *crc_ctx, int crc_bits, char *polyn);


typedef enum {
    TXPC_OP_NONE,
    TXPC_OP_RESET,
    TXPC_OP_MSG,
    TXPC_OP_CONFIG,
    TXPC_OP_ACK,
    // these are aliased to minimize the state variable size, but are more
    // readable when looking through the read state machine.
    TXPC_OP_WAIT_RESET = TXPC_OP_RESET,
    TXPC_OP_WAIT_MSG = TXPC_OP_MSG,
    TXPC_OP_WAIT_CONFIG = TXPC_OP_CONFIG,
    TXPC_OP_WAIT_ACK = TXPC_OP_ACK,
    TXPC_OP_WAIT_DISPATCH
} xpc_sm_state_t;

typedef enum {
    TXPC_STATUS_DONE,
    TXPC_STATUS_INFLIGHT,
    TXPC_STATUS_INHIBIT,
    TXPC_STATUS_BAD_STATE
} xpc_status_t;

typedef struct {
    unsigned char crc_bits;
    // there has to be a better way to express this. FIXME.
    union {
        unsigned char flags;
        enum {
            CONFIG_MASK_RESERVED = 0xfe,
            CONFIG_FLAGS_REQ_ACK = 0x01
        } flag_defs;
    };
} xpc_config_t;

typedef struct {
    // global state for the xpc connection
    xpc_config_t conn_config;
    // contexts for the io subsystem, message handler, and crc subsystem.
    void *io_ctx;
    void *msg_ctx;
    void *crc_ctx;
    // function pointers for io, message handling, and crc subsystem.
    io_wrap_fn *write;
    io_wrap_fn *read;
    io_reset_fn *io_reset;
    io_notify_config *io_notify;
    dispatch_fn *dispatch_cb;
    crc_fn *crc;
    crc_polyn_config *crc_config;
    // These are signals between the two state machines.
    // the _SEND signals are asserted by the entry point functions, and not
    // by the write state machine.  Signals requiring acknowledgement are
    // de-asserted from the read state machine.  The write state machine will
    // return to normal operation on the next call to xpc_wr_op_continue.
    // The same is true of the read signals.  The read state machine asserts
    // _RECVD, and the write state machine deasserts them when the reply has
    // been sent.
    enum int_sig_t {
        // a reset message was received from the endpoint, invalidate write
        // state.
        SIG_RST_RECVD = 1,
        // this endpoint is sending a reset message, invalidate read state.
        SIG_RST_SEND = (1 << 1),
        SIG_CONFIG_RECVD = (1 << 2),
        SIG_CONFIG_SEND = (1 << 3),
        SIG_XOFF_RECVD = (1 << 4),
        SIG_ACK_RECVD = (1 << 5),
        SIG_NACK_RECVD = (1 << 6)
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
 * @param crc_ctx pointer to the context for crc computations
 * @param write the IO wrapper for writing a byte stream to the endpoint
 * @param read the IO wrapper for reading a byte stream from an endpoint
 * @param reset function to force buffer clear on IO subsystem
 * @param io_notify function to enable/disable read/write notifications
 * @param msg_handle_cb message handler callback function, called on any
 * message event.
 * @param crc crc computation callback function, called when verifying the crc
 * on message read, and when computing the crc of a message payload to send.
 * @param crc_config crc configuration function to set the parameters for the
 * crc subsystem.
 *
 * @return target, or NULL on failure.
 */
xpc_relay_state_t *xpc_relay_config(
    xpc_relay_state_t *target, void *io_ctx, void *msg_ctx, void *crc_ctx,
    io_wrap_fn *write, io_wrap_fn *read, io_reset_fn *reset,
    io_notify_config *io_notify, dispatch_fn *msg_handle_cb,
    crc_fn *crc, crc_polyn_config *crc_config
);

/**
 * Reset the connection. This should be called immediately after
 * configuration, before any messages are sent to ensure that buffers are in
 * sync on both ends.  Additionally, it should be sent any time that IO calls
 * become desynchronized or bytes are lost.
 * @param self the relay which should issue a new reset.
 * @return TXPC_STATUS_DONE when ready to send, TXPC_STATUS_INFLIGHT if not.
 */
xpc_status_t xpc_relay_send_reset(xpc_relay_state_t *self);

/**
 * Set up the communication channel parameters.
 * The default is no crc, no acknowledge.
 * @param crc_bits the number of bits to use in CRC computations. 0 to disable.
 * @param crc_polyn pointer to the CRC polynomial.
 * @param msg_sync_ack whether or not to force synchronous acknowledgement on
 * messages (does not apply to configuration messages or streams)
 * @return TXPC_STATUS_DONE when ready to send, TXPC_STATUS_INFLIGHT if not.
 */
xpc_status_t xpc_relay_send_config(
    xpc_relay_state_t *self,
    int crc_bits, char *crc_polyn,
    bool msg_sync_ack
);

/**
 * Send a flow control message. Sending an XOFF message prevents messages after
 * receipt of this message from being sent by the remote, but does not cancel
 * any inflight messages. Sending an XON message allows flow to resume. Whether
 * inflight message are re-transmitted is not specified.
 * @param self the relay which should issue the message
 * @param xon 1 to enable flow, 0 to disable.
 */
xpc_status_t xpc_relay_set_flow(xpc_relay_state_t *self, bool xon);

/**
 * Send a new message with optional payload to the remote endpoint.
 * @param self the relay which should send the message
 * @param to the "to" message field value
 * @param from the "from" message field value
 * @param data buffer to contiguous memory containing message payload
 * @param bytes the number of bytes of payload, < 65536
 * @return TXPC_STATUS_DONE when message is being sent, TXPC_STATUS_INFLIGHT
 * if the relay is busy.
 */
xpc_status_t xpc_send_msg(xpc_relay_state_t *self, uint8_t to, uint8_t from, char *data, size_t bytes);

/**
 * Attempt to continue the currently inflight write operation.  State changes
 * are also enacted by this function for any write-related state transitions.
 * This function should be called when the stream associated with this
 * xpc connection is ready for writing.
 * @param self the relay whose IO channel is ready for writing
 * @return txpc_status_t
 */
xpc_status_t xpc_wr_op_continue(xpc_relay_state_t *self);

/**
 * Attempt to continue the currently inflight read operation.  State changes
 * are also enacted by this function for any read-related state transitions.
 * This function should be called when the stream associated with this
 * xpc connection is ready for reading.
 * @param self the relay whose IO channel is ready for reading
 * @return txpc_status_t
 */
xpc_status_t xpc_rd_op_continue(xpc_relay_state_t *self);
