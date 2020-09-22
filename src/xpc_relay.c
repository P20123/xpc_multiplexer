#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h> // temp, FIXME
#include <xpc_relay.h>

xpc_relay_state_t *xpc_relay_config(
    xpc_relay_state_t *target, void *io_ctx, void *msg_ctx,
    io_wrap_fn *write, io_wrap_fn *read, io_reset_fn *reset,
    dispatch_fn *msg_handle_cb
) {
    if(target == NULL) goto done;
    // global state
    target->use_le = true; // FIXME
    target->connection_ready = false;
    // contexts
    target->io_ctx = io_ctx;
    target->msg_ctx = msg_ctx;
    // function pointers
    target->write = write;
    target->read = read;
    target->io_reset = reset;
    target->dispatch_cb = msg_handle_cb;
    // write operation
    target->inflight_wr_op.op = TXPC_OP_NONE;
    target->inflight_wr_op.total_bytes = 0;
    target->inflight_wr_op.bytes_complete = 0;
    target->inflight_wr_op.msg_hdr = (txpc_hdr_t){0};
    target->inflight_wr_op.buf = NULL;
    // read operation
    target->inflight_rd_op.op = TXPC_OP_NONE;
    target->inflight_rd_op.total_bytes = 0;
    target->inflight_rd_op.bytes_complete = 0;
    target->inflight_rd_op.msg_hdr = (txpc_hdr_t){0};
    target->inflight_rd_op.buf = NULL;
done:
    return target;
}

xpc_status_t xpc_relay_send_reset(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
    if(self->inflight_wr_op.op != TXPC_OP_NONE ||
            // FIXME... what if we need to reset mid-message?
            self->inflight_rd_op.op != TXPC_OP_NONE) {
        status = TXPC_STATUS_INFLIGHT;
        goto done;
    }
        self->signals |= SIG_RST_SEND;
done:
    return status;
}

xpc_status_t xpc_relay_send_disconnect(xpc_relay_state_t *self) {
}

xpc_status_t xpc_send_msg(xpc_relay_state_t *self, uint8_t to, uint8_t from, char *data, size_t bytes) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
    if(self->inflight_wr_op.op != TXPC_OP_NONE) {
        status = TXPC_STATUS_INFLIGHT;
        goto done;
    }
    self->inflight_wr_op.msg_hdr = (txpc_hdr_t){
        .size = bytes, .to = to, .from = from, .type = 5
    };
    self->inflight_wr_op.buf = data;
    self->inflight_wr_op.bytes_complete = 0;
    self->inflight_wr_op.total_bytes = sizeof(txpc_hdr_t) + bytes;
    self->inflight_wr_op.op = TXPC_OP_MSG;
done:
    return status;
}

// changes:
//  - no link with flow control, this is problematic 

xpc_status_t xpc_wr_op_continue(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL || self->read == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }

    int starting_state = -1;
    bool do_payload_write = false;
    bool prev_payload_write = false;
    int bytes = 0;
    do {
        bytes = 0;
        starting_state = self->inflight_wr_op.op;

        switch(self->inflight_wr_op.op) {
            case TXPC_OP_NONE:
                // check wr signals here
                if(self->signals & SIG_RST_SEND || self->signals & SIG_RST_RECVD) {
                    self->inflight_wr_op.op = TXPC_OP_RESET;
                    self->inflight_wr_op.bytes_complete = 0;
                    self->inflight_wr_op.total_bytes = sizeof(txpc_hdr_t);
                    self->inflight_wr_op.msg_hdr = (txpc_hdr_t){
                        .type = 1, .size = 0, .to = 0, .from = 0
                    };
                }
                // ... other cases here

            break;

            case TXPC_OP_RESET:
                if(self->inflight_wr_op.bytes_complete == sizeof(txpc_hdr_t)) {
                    if(self->signals & SIG_RST_RECVD) {
                        // if we did not initiate, we just sent the reply
                        self->signals &= ~SIG_RST_RECVD;
                        self->inflight_wr_op.op = TXPC_OP_NONE;
                        self->inflight_wr_op.bytes_complete = 0;
                        self->inflight_wr_op.total_bytes = 0;
                        self->io_reset(self->io_ctx, 0, -1);
                        self->io_reset(self->io_ctx, 1, -1);
                    }
                    else if(!(self->signals & SIG_RST_SEND)){
                        // we did initiate, rx sm will de-assert send signal
                        // and perform io reset
                        self->inflight_wr_op.op = TXPC_OP_NONE;
                        self->inflight_wr_op.bytes_complete = 0;
                        self->inflight_wr_op.total_bytes = 0;
                    }
                    // otherwise, we are waiting for the reply.
                }
            break;

            case TXPC_OP_MSG:
                prev_payload_write = do_payload_write;
                do_payload_write = true;
                if(self->inflight_wr_op.bytes_complete
                        == self->inflight_wr_op.msg_hdr.size + sizeof(txpc_hdr_t)) {
                    // if the currently inflight message has finished
                    self->io_reset(self->io_ctx, 0, -1);
                    // set state to none
                    self->inflight_wr_op.op = TXPC_OP_NONE;
                    prev_payload_write = do_payload_write;
                    do_payload_write = false;
                    goto done;
                }
            break;
        }

        if(self->inflight_wr_op.bytes_complete < sizeof(txpc_hdr_t) && self->inflight_wr_op.total_bytes > 0) {
            printf("doing header write\n");
            char *buf = (char*)&self->inflight_wr_op.msg_hdr;
            bytes = self->write(self->io_ctx, &buf, self->inflight_wr_op.bytes_complete, sizeof(txpc_hdr_t));
        }
        else if(do_payload_write) {
            printf("doing payload write\n");
            bytes = self->write(
                self->io_ctx,
                &self->inflight_wr_op.buf,
                self->inflight_wr_op.bytes_complete - sizeof(txpc_hdr_t),
                self->inflight_wr_op.total_bytes - self->inflight_wr_op.bytes_complete
            );
        }
        self->inflight_wr_op.bytes_complete += bytes;
    } while(self->inflight_wr_op.op != starting_state || prev_payload_write != do_payload_write);
done:
    return status;
}

xpc_status_t xpc_rd_op_continue(xpc_relay_state_t *self) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL || self->read == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }

    int starting_state = -1;
    bool do_payload_read = false;
    int bytes = 0;

    do {
        bytes = 0;
        starting_state = self->inflight_rd_op.op;
        // read in new bytes
        if(self->inflight_rd_op.bytes_complete < sizeof(txpc_hdr_t)) {
            printf("doing header read\n");                
            char *buf = (char*)&self->inflight_rd_op.msg_hdr;
            bytes = self->read(
                self->io_ctx,
                &buf,
                self->inflight_rd_op.bytes_complete,
                sizeof(txpc_hdr_t)
            );
        }
        else if(do_payload_read){
            printf("doing payload read\n");
            bytes = self->read(
                self->io_ctx,
                &self->inflight_rd_op.buf,
                self->inflight_rd_op.bytes_complete - sizeof(txpc_hdr_t),
                self->inflight_rd_op.total_bytes - self->inflight_rd_op.bytes_complete
            );
        }
        self->inflight_rd_op.bytes_complete += bytes;
        // inflight message read complete
        // do state update
        switch(self->inflight_rd_op.op) {
            case TXPC_OP_NONE:
                // if a message came in (and isn't part of an inflight message
                // because we're in the none state), figure out what state
                // we should go to.
                if(self->inflight_rd_op.bytes_complete >= sizeof(txpc_hdr_t)) {
                    switch(self->inflight_rd_op.msg_hdr.type) {
                        // reset
                        case 1:
                            // if we initiated, just de-assert the send signal
                            // on recv
                            if(self->signals & SIG_RST_SEND) {
                                self->signals &= ~SIG_RST_SEND;
                                self->io_reset(self->io_ctx, 0, -1);
                                self->io_reset(self->io_ctx, 1, -1);
                                self->inflight_rd_op.bytes_complete = 0;
                                self->inflight_rd_op.total_bytes = 5;
                            }
                            else {
                                // we got a reset, have to wait for completion
                                self->signals |= SIG_RST_RECVD;
                                self->inflight_rd_op.op = TXPC_OP_WAIT_RESET;
                            }
                        break;

                        // msg
                        case 5:
                            self->inflight_rd_op.op = TXPC_OP_WAIT_MSG;
                            self->inflight_rd_op.total_bytes = self->inflight_rd_op.msg_hdr.size + sizeof(txpc_hdr_t);
                            do_payload_read = true;
                        break;
                    }
                }

            break;

            case TXPC_OP_WAIT_RESET:
                if(self->inflight_rd_op.bytes_complete >= sizeof(txpc_hdr_t) &&
                        self->inflight_rd_op.msg_hdr.type == 1 &&
                        self->inflight_rd_op.msg_hdr.to == 0 &&
                        self->inflight_rd_op.msg_hdr.from == 0 &&
                        self->inflight_rd_op.msg_hdr.size == 0) {
                    // reset message received
                    // if we initiated, we just received the reply - go back to norm
                    if(self->signals & SIG_RST_SEND) {
                        self->signals &= ~(SIG_RST_SEND | SIG_RST_RECVD);
                        self->io_reset(self->io_ctx, 0, -1);
                        self->io_reset(self->io_ctx, 1, -1);
                    }
                    else {
                        // we did not initiate, stay here until SIG_RST_RECVD
                        // is de-asserted by tx sm
                        if(self->signals & SIG_RST_RECVD) {
                            self->inflight_rd_op.op = TXPC_OP_WAIT_RESET;
                        }
                        else {
                            self->inflight_rd_op.op = TXPC_OP_NONE;
                            self->inflight_rd_op.bytes_complete = 0;
                            self->inflight_rd_op.total_bytes = 0;
                            self->inflight_rd_op.buf = NULL;
                        }
                    }
                }
                else {
                    // incorrect sequence received, reset read ctx, try again.
                    self->io_reset(self->io_ctx, 1, 5);
                    self->inflight_rd_op.bytes_complete = 0;
                }
            break;

            case TXPC_OP_WAIT_MSG:
                printf("in msg rd state\n");
                if(self->inflight_rd_op.bytes_complete >= self->inflight_rd_op.total_bytes) {
                    printf("got a complete message\n");
                    // msg complete
                    do_payload_read = false;
                    self->inflight_rd_op.op = TXPC_OP_WAIT_DISPATCH;
                }
                else {
                    do_payload_read = true;
                }

            break;

            case TXPC_OP_WAIT_DISPATCH:
                if(self->dispatch_cb(self->msg_ctx, &self->inflight_rd_op.msg_hdr, self->inflight_rd_op.buf)) {
                    self->inflight_rd_op.op = TXPC_OP_NONE;
                    self->inflight_rd_op.total_bytes = 0;
                    self->inflight_rd_op.bytes_complete = 0;
                    self->io_reset(self->io_ctx, 1, -1);
                    goto done;
                }
            break;

        }

    // loop while state changed
    } while(self->inflight_rd_op.op != starting_state);

done:
    return status;
}
/*
 *
 *xpc_status_t xpc_rd_op_continue(xpc_relay_state_t *self) {
 *    int status = TXPC_STATUS_DONE;
 *    if(self == NULL || self->read == NULL) {
 *        status = TXPC_STATUS_BAD_STATE;
 *        goto done;
 *    }
 *    // read a new message header
 *    int bytes = 0;
 *    if(self->inflight_rd_op.bytes_complete < sizeof(txpc_hdr_t)) {
 *        printf("doing header read\n");
 *        io_buf_t buf;
 *        buf.read_buf = (char**)&self->inflight_rd_op.msg_hdr;
 *        bytes = self->read(
 *            self->io_ctx,
 *            buf,
 *            sizeof(txpc_hdr_t)
 *        );
 *        self->inflight_rd_op.bytes_complete += bytes;
 *    }
 *    if(self->inflight_rd_op.bytes_complete >= sizeof(txpc_hdr_t) &&
 *            self->inflight_rd_op.msg_hdr.type > 4) {
 *        printf("doing payload read\n");
 *        bytes = self->read(
 *            self->io_ctx,
 *            self->inflight_rd_op.buf,
 *            -1
 *        );
 *        self->inflight_rd_op.bytes_complete += bytes;
 *    }
 *    printf("bytes recvd: %i\n", self->inflight_rd_op.bytes_complete);
 *    // have to check none state early to allow write-signal precedence
 *    if(self->inflight_rd_op.op == TXPC_OP_NONE) {
 *        // handle signals from tx state machine
 *        if(self->signals & SIG_RST_SEND) {
 *            printf("starting recv reset sequence\n");
 *            self->inflight_rd_op.op = TXPC_OP_WAIT_RESET;
 *        }
 *        else if(self->signals & SIG_DISC_SEND) {
 *            // some kind of de-init should go here
 *            goto done;
 *        }
 *        else if(self->signals & SIG_CRC_SEND) {
 *            self->inflight_rd_op.op = TXPC_OP_WAIT_CRC;
 *        }
 *        else if(self->signals & SIG_ENDIANNESS_SEND) {
 *            self->inflight_rd_op.op = TXPC_OP_WAIT_ENDIANNESS;
 *        }
 *        else {
 *            self->inflight_rd_op.op = TXPC_OP_NONE;
 *            self->inflight_rd_op.total_bytes = 0;
 *        }
 *    }
 *    // switch on state after handling wr signals
 *    switch(self->inflight_rd_op.op) {
 *        case TXPC_OP_NONE:
 *            // handle incoming messages
 *            if(self->inflight_rd_op.bytes_complete >= sizeof(txpc_hdr_t)) {
 *                printf("got a message header\n");
 *                switch(self->inflight_rd_op.msg_hdr.type) {
 *                    // FIXME make these into constants in the txpc spec
 *                    case 1:
 *                        printf("reset msg recvd\n");
 *                        // reset message
 *                        // we got a reset, need to reply
 *                        self->inflight_rd_op.bytes_complete = 0;
 *                        self->inflight_rd_op.total_bytes = 0;
 *                        self->inflight_rd_op.op = TXPC_OP_NONE;
 *                        // raise reset signal
 *                        self->signals |= SIG_RST_RECVD;
 *                    break;
 *
 *                    case 2:
 *                        // set endianness
 *                        self->inflight_rd_op.op = TXPC_OP_WAIT_ENDIANNESS;
 *                        // raise endianness change signal
 *                        self->signals |= SIG_ENDIANNESS_RECVD;
 *                    break;
 *
 *                    case 3:
 *                        // set crc bits
 *                        self->inflight_rd_op.op = TXPC_OP_WAIT_CRC;
 *                        // raise endianness change signal
 *                        self->signals |= SIG_CRC_RECVD;
 *                    break;
 *
 *                    case 5:
 *                        // TODO we can get here, but now need to handle the message after it has come in... it could be complete already!
 *                        printf("msg recvd\n");
 *                        self->inflight_rd_op.op = TXPC_OP_MSG;
 *                        self->inflight_rd_op.total_bytes = sizeof(txpc_hdr_t) + self->inflight_rd_op.msg_hdr.size;
 *                    break;
 *
 *                }
 *            }
 *        break;
 *
 *        case TXPC_OP_WAIT_RESET:
 *            // reset properly received, de-assert SIG_RESET_SEND
 *            if(self->inflight_rd_op.bytes_complete >= sizeof(txpc_hdr_t)) {
 *                printf("finishing reset recv sequence\n");
 *                self->signals &= ~(SIG_RST_SEND | SIG_RST_RECVD);
 *                self->inflight_rd_op.op = TXPC_OP_NONE;
 *                self->inflight_rd_op.bytes_complete = 0;
 *                self->inflight_rd_op.total_bytes = 0;
 *                self->io_reset(self->io_ctx, 0, -1);
 *                self->io_reset(self->io_ctx, 1, -1);
 *            }
 *
 *        break;
 *
 *        case TXPC_OP_WAIT_MSG:
 *            if(self->inflight_rd_op.bytes_complete == self->inflight_rd_op.total_bytes) {
 *                bool dispatch_done = self->dispatch_cb(
 *                    self->msg_ctx,
 *                    &self->inflight_rd_op.msg_hdr,
 *                    *self->inflight_rd_op.buf.read_buf
 *                );
 *                if(dispatch_done) {
 *                    self->inflight_rd_op.op = TXPC_OP_NONE;
 *                    self->inflight_rd_op.bytes_complete = 0;
 *                    self->inflight_rd_op.total_bytes = 0;
 *                    self->io_reset(self->io_ctx, 1, -1);
 *                }
 *                else {
 *                    // uhhhh
 *                }
 *
 *            }
 *
 *        break;
 *
 *        case TXPC_OP_WAIT_CRC:
 *
 *        break;
 *
 *        case TXPC_OP_WAIT_ENDIANNESS:
 *
 *        break;
 *
 *        case TXPC_OP_WAIT_DISPATCH:
 *
 *        break;
 *
 *        default:
 *            status = TXPC_STATUS_BAD_STATE;
 *            goto done;
 *        break;
 *    }
 *done:
 *    return status;
 *}
 */
 
/*
 *xpc_status_t xpc_wr_op_continue(xpc_relay_state_t *self) {
 *    int status = TXPC_STATUS_DONE;
 *    if(self == NULL || self->write == NULL) {
 *        status = TXPC_STATUS_BAD_STATE;
 *        goto done;
 *    }
 *
 *    if(self->inflight_wr_op.op == TXPC_OP_NONE) {
 *        self->inflight_wr_op.bytes_complete = 0;
 *        // goto send reset state if we received a reset or if we are sending
 *        // a reset message.
 *        if(self->signals & SIG_RST_RECVD || self->signals & SIG_RST_SEND) {
 *            self->inflight_wr_op.op = TXPC_OP_RESET;
 *            self->inflight_wr_op.bytes_complete = 0;
 *            self->inflight_wr_op.msg_hdr = (txpc_hdr_t){
 *                .size = 0, .type = 1, .to = 0, .from = 0
 *            };
 *            self->inflight_wr_op.buf = NULL;
 *            self->inflight_wr_op.total_bytes = sizeof(txpc_hdr_t);
 *            printf("starting reset write sequence\n");
 *        }
 *        else if(self->signals & SIG_DISC_RECVD) {
 *            // some de-init should probably go here.
 *            goto done;
 *        }
 *        else {
 *            // none of the other signals require action on the
 *            // write state machine, exit successfully here.
 *            goto done;
 *        }
 *    }
 *
 *    // actually perform write operations here
 *    int bytes = 0;
 *    // write the message header first, if it has not been sent yet.
 *    if(self->inflight_wr_op.bytes_complete < sizeof(txpc_hdr_t)) {
 *        printf("doing header write\n");
 *        char *buf = (char*)&self->inflight_wr_op.msg_hdr;
 *        bytes = self->write(self->io_ctx, &buf, sizeof(txpc_hdr_t));
 *        self->inflight_wr_op.bytes_complete += bytes;
 *    }
 *    if(self->inflight_wr_op.bytes_complete < self->inflight_wr_op.total_bytes &&
 *            self->inflight_wr_op.bytes_complete >= sizeof(txpc_hdr_t)) {
 *        printf("doing payload write\n");
 *        bytes = self->write(
 *            self->io_ctx, &self->inflight_wr_op.buf, self->inflight_wr_op.total_bytes - self->inflight_wr_op.bytes_complete
 *        );
 *        self->inflight_wr_op.bytes_complete += bytes;
 *    }
 *    printf("bytes written: %i\n", bytes);
 *    // check rd and wr signals only in none state
 *    // do msg wr if pending/inflight
 *    // revert to none if msg wr complete
 *    // always return to idle after compl
 *    // determine state transitions, if any.
 *    switch(self->inflight_wr_op.op) {
 *        case TXPC_OP_NONE:
 *            // this state is escaped when a signal comes in from the read
 *            // state machine, or when a message send function is called.
 *            // ensure byte counter is reset before any new operations start.
 *        break;
 *
 *        case TXPC_OP_RESET:
 *            if(self->inflight_wr_op.bytes_complete >= sizeof(txpc_hdr_t)) {
 *                if(self->signals & SIG_RST_RECVD) {
 *                    printf("finished reset reply\n");
 *                    // deassert reset received signal, allow rx state to
 *                    // return to normal.
 *                    self->signals &= ~SIG_RST_RECVD;
 *                    self->io_reset(self->io_ctx, 0, -1);
 *                    self->io_reset(self->io_ctx, 1, -1);
 *                }
 *                // reset io context
 *                self->inflight_wr_op.op = TXPC_OP_NONE;
 *                self->signals &= ~SIG_RST_SEND;
 *                self->inflight_wr_op.bytes_complete = 0;
 *                self->inflight_wr_op.total_bytes = 0;
 *            }
 *        break;
 *
 *        case TXPC_OP_MSG:
 *            printf("in msg wr state\n");
 *            if(self->inflight_wr_op.bytes_complete
 *                    == self->inflight_wr_op.msg_hdr.size + sizeof(txpc_hdr_t)) {
 *                printf("msg wr done\n");
 *                // if the currently inflight message has finished
 *                self->io_reset(self->io_ctx, 0, -1);
 *                // set state to none
 *                self->inflight_wr_op.op = TXPC_OP_NONE;
 *                goto done;
 *             }
 *
 *        break;
 *
 *        case TXPC_OP_STOP:
 *            // send disconnect message
 *            // set state to none
 *        break;
 *
 *        case TXPC_OP_SET_CRC:
 *            // send crc bits message
 *            // wait for SIG_CRC_RECVD assertion
 *            if(self->signals & SIG_CRC_RECVD) {
 *                // set state to none
 *                self->inflight_wr_op.op = TXPC_OP_NONE;
 *                self->signals &= ~SIG_CRC_RECVD;
 *            }
 *        break;
 *
 *        case TXPC_OP_SET_ENDIANNESS:
 *            // send endianness message
 *            // wait for SIG_ENDIANNESS_RECVD assertion
 *            // set state to none
 *        break;
 *
 *        default:
 *            status = TXPC_STATUS_BAD_STATE;
 *            goto done;
 *        break;
 *    }
 *done:
 *    return status;
 *}
 */
