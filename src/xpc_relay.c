#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h> // temp, FIXME
#include <xpc_relay.h>
// notes:
//  - we can make resets/flow controls async by calling io_reset mid-msg, but
//  that would be living life dangerously.
//  - crc_bits >> 3 is wrong.  Consider the case of a 33 bit crc.
//  - crc_config should support seed settings, inversion, byte swapping...

xpc_relay_state_t *xpc_relay_config(
    xpc_relay_state_t *target, void *io_ctx, void *msg_ctx, void *crc_ctx,
    io_wrap_fn *write, io_wrap_fn *read, io_reset_fn *reset,
    io_notify_config *io_notify, dispatch_fn *msg_handle_cb,
    crc_fn *crc, crc_polyn_config *crc_config
) {
    if(target == NULL) goto done;
    // global state
    target->conn_config = (xpc_config_t){.crc_bits = 0, .flags = 0};
    // contexts
    target->io_ctx = io_ctx;
    target->msg_ctx = msg_ctx;
    target->crc_ctx = crc_ctx;
    // function pointers
    target->write = write;
    target->read = read;
    target->io_reset = reset;
    target->io_notify = io_notify;
    target->dispatch_cb = msg_handle_cb;
    target->crc = crc;
    target->crc_config = crc_config;
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
    if(self->inflight_wr_op.op != TXPC_OP_NONE) { // || self->inflight_rd_op.op != TXPC_OP_NONE) {
        status = TXPC_STATUS_INFLIGHT;
        goto done;
    }
    else if(self->signals & SIG_XOFF_RECVD) {
        status = TXPC_STATUS_INHIBIT;
        goto done;
    }
    self->signals |= SIG_RST_SEND;
    self->inflight_wr_op.op = TXPC_OP_RESET;
    self->inflight_wr_op.bytes_complete = 0;
    self->inflight_wr_op.total_bytes = sizeof(txpc_hdr_t);
    self->inflight_wr_op.msg_hdr = (txpc_hdr_t){
        .type = 1, .size = 0, .to = 0, .from = 0
    };
    self->io_notify(self->io_ctx, 1, true);
done:
    return status;
}

xpc_status_t xpc_relay_send_config(
        xpc_relay_state_t *self,
        int crc_bits, char *crc_polyn,
        bool msg_sync_ack) {
    int status = TXPC_STATUS_DONE;
    if(self == NULL) {
        status = TXPC_STATUS_BAD_STATE;
        goto done;
    }
    if(self->inflight_wr_op.op != TXPC_OP_NONE) {
        status = TXPC_STATUS_INFLIGHT;
        goto done;
    }
    else if(self->signals & SIG_XOFF_RECVD) {
        status = TXPC_STATUS_INHIBIT;
        goto done;
    }
    self->inflight_wr_op.msg_hdr = (txpc_hdr_t) {
        .type = 2, .size = 1 + 1 + (crc_bits >> 3), .to = 0, .from = 0
    };
    self->inflight_wr_op.buf = crc_polyn;
    self->inflight_wr_op.total_bytes = sizeof(txpc_hdr_t) + self->inflight_wr_op.msg_hdr.size;
    self->inflight_wr_op.bytes_complete = 0;
    self->inflight_wr_op.op = TXPC_OP_CONFIG;

    self->conn_config.crc_bits = crc_bits;
    self->conn_config.flags = msg_sync_ack;

    self->crc_config(self->crc_ctx, crc_bits, crc_polyn);
    /*self->signals |= SIG_CONFIG_SEND; // XXX what is this for?*/
    self->io_notify(self->io_ctx, 1, true);
done:
    return status;
    
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
    else if(self->signals & SIG_XOFF_RECVD) {
        status = TXPC_STATUS_INHIBIT;
        goto done;
    }
    self->inflight_wr_op.msg_hdr = (txpc_hdr_t){
        .size = bytes, .to = to, .from = from, .type = 5
    };
    self->inflight_wr_op.buf = data;
    self->inflight_wr_op.bytes_complete = 0;
    // the >> 3 divides by 8 to go from bits -> bytes, and 0 >> 3 = 0 -> no crc.
    self->inflight_wr_op.total_bytes =
        sizeof(txpc_hdr_t) + bytes + (self->conn_config.crc_bits >> 3);
    self->inflight_wr_op.op = TXPC_OP_MSG;
    self->io_notify(self->io_ctx, 1, true);
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
    bool do_crc_write = false;
    char *crc_location = NULL;
    char *payload_location = NULL;
    int write_size = 0;
    int write_offset = 0;
    int bytes = 0;
    do {
        bytes = 0;
        starting_state = self->inflight_wr_op.op;

        switch(self->inflight_wr_op.op) {
            case TXPC_OP_NONE:
                // check rd signals here
                if(self->signals & SIG_XOFF_RECVD) {
                    // inhibit all operations until this signal is de-asserted.
                    status = TXPC_STATUS_INHIBIT;
                    goto done; // short circuit to the end
                }
                if(self->signals & SIG_RST_RECVD) {
                    self->inflight_wr_op.op = TXPC_OP_RESET;
                    self->inflight_wr_op.bytes_complete = 0;
                    self->inflight_wr_op.total_bytes = sizeof(txpc_hdr_t);
                    self->inflight_wr_op.msg_hdr = (txpc_hdr_t){
                        .type = 1, .size = 0, .to = 0, .from = 0
                    };
                }
                // turn off write notifications if there is no msg to send
                self->io_notify(self->io_ctx, 1, false);
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
                write_offset = self->inflight_wr_op.bytes_complete - sizeof(txpc_hdr_t);
                prev_payload_write = do_payload_write;
                do_payload_write = true;
                payload_location = self->inflight_wr_op.buf;
                write_size = self->inflight_wr_op.total_bytes - self->inflight_wr_op.bytes_complete - (self->conn_config.crc_bits >> 3);
                if(self->inflight_wr_op.bytes_complete
                        == self->inflight_wr_op.total_bytes) {
                    // if the currently inflight message has finished
                    self->io_reset(self->io_ctx, 0, -1);
                    // set state to none
                    self->inflight_wr_op.op = TXPC_OP_NONE;
                    prev_payload_write = do_payload_write;
                    do_payload_write = false;
                    do_crc_write = false;
                    /*goto done;*/
                }
                // this case must come after the above case, otherwise crc write
                // will trigger even when there is no crc being used.
                else if(self->inflight_wr_op.bytes_complete
                        == self->inflight_wr_op.msg_hdr.size
                        + sizeof(txpc_hdr_t)) {
                    // hdr + payload sent, but not crc
                    write_offset = 0;
                    self->io_reset(self->io_ctx, 0, -1);
                    crc_location = self->crc(
                        self->crc_ctx,
                        self->inflight_wr_op.buf,
                        self->inflight_wr_op.msg_hdr.size
                    );
                    prev_payload_write = do_payload_write;
                    do_payload_write = false;
                    do_crc_write = true;
                }
            break;

            case TXPC_OP_CONFIG:
                printf("sending config msg\n");
                do_payload_write = true;
                if(self->inflight_wr_op.bytes_complete
                        == self->inflight_wr_op.total_bytes) {
                    // if the currently inflight message has finished
                    self->io_reset(self->io_ctx, 0, -1);
                    // set state to none
                    self->inflight_wr_op.op = TXPC_OP_NONE;
                    prev_payload_write = do_payload_write;
                    self->inflight_wr_op.total_bytes = 0;
                    self->inflight_wr_op.bytes_complete = 0;
                    do_payload_write = false;
                    do_crc_write = false;
                }
                else if(self->inflight_wr_op.bytes_complete == sizeof(txpc_hdr_t) + 2) {
                    // hdr + mode + crc_bits sent, send polyn now
                    payload_location = self->inflight_wr_op.buf;
                    write_size = self->conn_config.crc_bits >> 3;
                }
                else if(self->inflight_wr_op.bytes_complete == sizeof(txpc_hdr_t) + 1) {
                    // hdr + mode sent
                    payload_location = (char*)&self->conn_config.crc_bits;
                    write_size = 1;

                }
                else if(self->inflight_wr_op.bytes_complete == sizeof(txpc_hdr_t)) {
                    // hdr only
                    payload_location = (char*)&self->conn_config.flags;
                    write_size = 1;
                }
            break;

            case TXPC_OP_ACK:

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
                &payload_location,
                write_offset,
                write_size
            );
        }
        else if(do_crc_write) {
            printf("doing crc write\n");
            bytes = self->write(
                self->io_ctx,
                &crc_location,
                0,
                (self->conn_config.crc_bits >> 3)
            );
        }
        self->inflight_wr_op.bytes_complete += bytes;
    } while(self->inflight_wr_op.op != starting_state || bytes > 0);// || prev_payload_write != do_payload_write);
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
    bool prev_payload_read = false;
    bool do_crc_read = false;
    char *crc_location = NULL;
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
        else if(do_payload_read || do_crc_read){
            printf("doing payload/crc read\n");
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

                        // config
                        case 2:
                            self->inflight_rd_op.op = TXPC_OP_WAIT_CONFIG;
                            self->inflight_rd_op.total_bytes = self->inflight_rd_op.msg_hdr.size + sizeof(txpc_hdr_t);
                            prev_payload_read = do_payload_read;
                            do_payload_read = true;
                        break;

                        // msg
                        case 5:
                            self->inflight_rd_op.op = TXPC_OP_WAIT_MSG;
                            self->inflight_rd_op.total_bytes = self->inflight_rd_op.msg_hdr.size + sizeof(txpc_hdr_t) + (self->conn_config.crc_bits >> 3);
                            prev_payload_read = do_payload_read;
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
                    self->inflight_rd_op.total_bytes = 0;
                }
            break;

            case TXPC_OP_WAIT_MSG:
                printf("in msg rd state\n");
                if(self->inflight_rd_op.bytes_complete == self->inflight_rd_op.total_bytes) {
                    printf("got a complete message\n");
                    // msg complete
                    do_crc_read = false;
                    if(self->conn_config.crc_bits) {
                        crc_location = self->crc(
                            self->crc_ctx,
                            self->inflight_rd_op.buf,
                            self->inflight_rd_op.msg_hdr.size
                        );
                        // verify crc
                        if(!memcmp(
                                crc_location,
                                self->inflight_rd_op.buf
                                    + self->inflight_rd_op.msg_hdr.size,
                                self->conn_config.crc_bits >> 3)) {
                            
                            self->inflight_rd_op.op = TXPC_OP_WAIT_DISPATCH;
                            // TODO ack here in forced ack mode
                        }
                        else {
                            // TODO nack here in forced ack mode
                            self->io_reset(self->io_ctx, 1, -1);
                        }
                    }
                    else {
                        // crc disabled, go to dispatch state.
                        self->inflight_rd_op.op = TXPC_OP_WAIT_DISPATCH;
                    }
                }
                else if(self->inflight_rd_op.bytes_complete
                        >= self->inflight_rd_op.msg_hdr.size
                        + sizeof(txpc_hdr_t)) {
                    // hdr + payload recv'd, crc in transit
                    prev_payload_read = do_payload_read;
                    do_payload_read = false;
                    do_crc_read = true;
                }
                else {
                    // hdr recv'd, payload and possible crc in transit.
                    prev_payload_read = do_payload_read;
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

            case TXPC_OP_WAIT_CONFIG:
                printf("reading config msg\n");
                prev_payload_read = do_payload_read;
                do_payload_read = true;
                do_crc_read = false;
                if(self->inflight_rd_op.bytes_complete
                        == self->inflight_rd_op.total_bytes) {
                    printf("config msg done\n");
                    // if the currently inflight message has finished
                    self->inflight_rd_op.total_bytes = 0;
                    self->inflight_rd_op.bytes_complete = 0;
                    self->io_reset(self->io_ctx, 1, -1);
                    // set state to none
                    self->inflight_rd_op.op = TXPC_OP_NONE;
                    prev_payload_read = do_payload_read;
                    do_payload_read = false;
                    do_crc_read = false;
                    self->conn_config.flags = self->inflight_rd_op.buf[0];
                    self->conn_config.crc_bits = self->inflight_rd_op.buf[1];
                    self->crc_config(self->crc_ctx, self->conn_config.crc_bits, self->inflight_rd_op.buf + 2);
                    goto done;
                }
            break;

        }

    // loop while state changed
    } while(self->inflight_rd_op.op != starting_state);

done:
    return status;
}
