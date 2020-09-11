#include <stdbool.h>
#include <stddef.h>
#include <xpc_relay.h>

xpc_relay_state_t *xpc_relay_config(
    xpc_relay_state_t *target, void *io_ctx, void *msg_ctx,
    io_wrap_fn *write, io_wrap_fn *read, dispatch_fn *msg_handle_cb
) {
    if(target == NULL) goto done;
    target->io_ctx = io_ctx;
    target->msg_ctx = msg_ctx;
    target->write = write;
    target->read = read;
    target->dispatch_cb = msg_handle_cb;
    target->use_le = true; // FIXME
    target->connection_ready = false;
    target->inflight_op.op = TXPC_OP_NONE;
    target->inflight_op.bytes_complete = 0;
    target->inflight_op.msg_hdr = (txpc_hdr_t){0};
    target->inflight_op.buf = (io_buf_t){0};
done:
    return target;
}

int xpc_relay_send_reset(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL || self->write == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
    // do nothing if another operation is inflight
    if(!(self->inflight_op.op == TXPC_OP_NONE || self->inflight_op.op == TXPC_OP_RESET)) {
        status = TXPC_STATUS_INFLIGHT;
        goto done;
    }
    // connection reset message
    self->inflight_op.msg_hdr = (txpc_hdr_t){
        .size = 0,
        .type = 1,
        .to = 0,
        .from = 0
    };
    // set the current operation to be reset
    self->inflight_op.op = TXPC_OP_RESET;
    // FIXME change ptr type for write
    int bytes = self->write(
        self->io_ctx,
        &self->inflight_op.buf,
        sizeof(txpc_hdr_t)
    );
    if(self->inflight_op.bytes_complete == sizeof(txpc_hdr_t)) {
        // reset message has been sent
        self->inflight_op.op = TXPC_OP_NONE;
        self->inflight_op.bytes_complete = 0;
        self->inflight_op.msg_hdr = (txpc_hdr_t){0};
        status = TXPC_STATUS_DONE;
    }
    else {
        self->inflight_op.bytes_complete += bytes;
        status = TXPC_STATUS_INFLIGHT;
    }
done:
    return status;
}

int xpc_relay_send_disconnect(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL || self->write == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
    // do nothing if another operation is inflight
    if(!(self->inflight_op.op == TXPC_OP_NONE || self->inflight_op.op == TXPC_OP_RESET)) {
        status = TXPC_STATUS_INFLIGHT;
        goto done;
    }
done:
    return status;
}

int xpc_send_msg(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL || self->write == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
    // do nothing if another operation is inflight
    if(!(self->inflight_op.op == TXPC_OP_NONE || self->inflight_op.op == TXPC_OP_RESET)) {
        status = TXPC_STATUS_INFLIGHT;
        goto done;
    }
done:
    return status;
}

int xpc_op_continue(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL || self->write == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
done:
    return status;
}

int xpc_read_bytes(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL || self->write == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
done:
    return status;
}
