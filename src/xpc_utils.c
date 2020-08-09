#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <xpc_utils.h>
#include <tinyxpc/tinyxpc.h>
#include <xpc_msg_queue.h>
#include <alibc/containers/dynabuf.h>
#include <alibc/containers/array.h>
#include <alibc/containers/hashmap.h>
#include <alibc/containers/bitmap.h>
#include <alibc/containers/hash_functions.h>
#include <alibc/containers/comparators.h>
#include <alibc/containers/iterator.h>
#include <alibc/containers/array_iterator.h>
#include <alibc/containers/hashmap_iterator.h>

static int8_t xpc_switch_cmp(void *a, void *b) {
    xpc_switch_tbl_entry_t c = *(xpc_switch_tbl_entry_t*)&a;
    xpc_switch_tbl_entry_t d = *(xpc_switch_tbl_entry_t*)&b;
    int8_t status = 0;
    // only useful for equality checking for now.
    if(c.fd < d.fd) status = -1;
    if(c.to_chn != d.to_chn) status = 1;
    return status;
}

static int xpc_switch_hash(void *a) {
    xpc_switch_tbl_entry_t t = *(xpc_switch_tbl_entry_t*)&a;
    return t.fd + t.to_chn;
}

xpc_out_ctx_t *create_xpc_out_ctx(xpc_out_ctx_t *target) {
    xpc_out_ctx_t *r = target;
    if(r == NULL) {
        goto done;
    }
    r->msg_queue = create_msg_queue();
    if(r->msg_queue == NULL) {
        free(r);
        r = NULL;
        goto done;
    }
    // no buffer in use.
    r->current_buf_id = -1;
done:
    return r;
}

void xpc_out_ctx_free(xpc_out_ctx_t *self) {
    if(self != NULL) {
        xpc_msg_queue_destroy(self->msg_queue);
    }
}

xpc_router_t *initialize_xpc_router() {
    xpc_router_t *r = malloc(sizeof(xpc_router_t));
    if(r == NULL) {
        goto done;
    }

    // initialize the fd buffers list
    r->in_contexts = create_hashmap(
        4, sizeof(int), sizeof(xpc_in_ctx_t),
        alc_default_hash_i32, alc_default_cmp_i32, NULL
    );
    if(r->in_contexts == NULL) {
        free(r);
        r = NULL;
        goto done;
    }

    r->out_contexts = create_hashmap(
        4, sizeof(int), sizeof(xpc_out_ctx_t),
        alc_default_hash_i32, alc_default_cmp_i32, NULL
    );
    if(r->out_contexts == NULL) {
        hashmap_free(r->in_contexts);
        free(r);
        r = NULL;
        goto done;
    }
    r->switch_tbl = create_hashmap(
        4, sizeof(xpc_switch_tbl_entry_t), sizeof(xpc_switch_tbl_entry_t),
        xpc_switch_hash, xpc_switch_cmp, NULL);
    if(r->switch_tbl == NULL) {
        hashmap_free(r->out_contexts);
        hashmap_free(r->in_contexts);
        free(r);
        r = NULL;
        goto done;
    }
done:
    return r;
}

void xpc_router_destroy(xpc_router_t *ctx) {
    if(ctx != NULL) {
        hashmap_free(ctx->in_contexts);
        iter_context *it = create_hashmap_values_iterator(ctx->out_contexts);
        xpc_out_ctx_t *next = (xpc_out_ctx_t *)iter_next(it);
        while(iter_status(it) != ALC_ITER_STOP) {
            xpc_out_ctx_free(next);
            next = (xpc_out_ctx_t *)iter_next(it);
        }
        hashmap_free(ctx->out_contexts);
        hashmap_free(ctx->switch_tbl);
        free(ctx);
    }
}


int xpc_accumulate_msg(xpc_router_t *ctx, int fd) {
    msg_buf_t *msg_buf = NULL;
    xpc_out_ctx_t *out_ctx = NULL;
    int bytes_read = 0;
    // get the context for this input fd
    xpc_in_ctx_t *in_ctx = hashmap_fetch(ctx->in_contexts, fd);
    if(hashmap_status(ctx->in_contexts) != ALC_HASHMAP_SUCCESS) {
        goto done;
    }
    // Switch table lookup cannot be performed without (fd, to).
    // If a message is not in-flight, the to-channel is not available.
    // Read into a temporary buffer first until the header is obtained,
    // then use the inflight message logic.
    if(!in_ctx->msg_inflight) {
        // need a new buffer from xpc_msg_getbuf
        in_ctx->buf_id = -1;
        in_ctx->buf_offset = 0;

        // obtain the header, blocking (necessary to be able to continue
        // processing data from this fd).
        while(bytes_read < sizeof(txpc_hdr_t)) {
            bytes_read += read(
                fd, &in_ctx->msg_hdr + bytes_read, sizeof(txpc_hdr_t)
            );
        }
        in_ctx->msg_inflight = true;
    }
    // A message is now inflight, so the stored header of this fd is valid.
    // Fetch the switch table entry for this fd.
    xpc_switch_tbl_entry_t key = {.fd = fd, .to_chn = in_ctx->msg_hdr.to};
    xpc_switch_tbl_entry_t *sw_ent = hashmap_fetch(ctx->switch_tbl, *(void**)&key);
    if(sw_ent == NULL) {
        // couldn't find that route, just drop the message.
        // this implies reading and ignoring the number of bytes in the size
        // header.
        goto done;
    }

    // Fetch the output queue associated with the fd this message is going to.
    out_ctx = hashmap_fetch(ctx->out_contexts, sw_ent->fd);
    if(out_ctx == NULL) {
        // no queue for this fd. here we make the assumption that any fd
        // in the routing table is already open, so a lack of an fd must be
        // an error in the caller.
        goto done;
    }
    // NOTE: possible efficiency improvement: store the buffers in a minheap,
    // and now that the message size is known (spec chg.), get one that is
    // optimally sized for this message
    msg_buf = xpc_msg_getbuf(out_ctx->msg_queue, in_ctx->buf_id);
    // couldn't obtain a buffer, it doesn't exist and no memory remains.
    if(msg_buf == NULL) {
        goto done;
    }
    in_ctx->msg_inflight = true;
    in_ctx->buf_id = msg_buf->buf_id;
    memcpy(msg_buf->buf->buf, &in_ctx->msg_hdr, sizeof(txpc_hdr_t));
    /*in_ctx->buf_offset += sizeof(txpc_hdr_t);*/

    
    // this makes sure there is space to read into, and also limits the size
    // of read so that we guarantee that a new function call to accumulate_msg
    // happens at the message boundary.
    if(msg_buf->buf->capacity < in_ctx->msg_hdr.size + sizeof(txpc_hdr_t)) {
        dynabuf_resize(msg_buf->buf, in_ctx->msg_hdr.size + sizeof(txpc_hdr_t));
    }
    // XXX the associated fd M U S T  be opened with O_NONBLOCK, or this will
    // cause a lot of deadlocks.
    int rd_bytes = read(
        fd,
        msg_buf->buf->buf + in_ctx->buf_offset,
        msg_buf->buf->capacity - in_ctx->buf_offset
    );
    if(rd_bytes == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            // no more data is available. do we do anything?
        }
    }
    else {
        // bytes_read was filled by grabbing the header if this is the first
        // call for this message.
        in_ctx->buf_offset += rd_bytes + bytes_read;
        // update the size of the actual contents of this message.
        msg_buf->size = in_ctx->buf_offset;
        bytes_read = in_ctx->buf_offset;
    }

    // strange design choice, but we handle negotiation messages here.
    // This is because it is known ahead of time that these messages will
    // never go anywhere except back to the sender, and many of them will
    // simply never elicit a response.
    if(in_ctx->msg_hdr.to == 0 && in_ctx->msg_hdr.from == 0) {
        // negotiation sub-protocol, we handle these
        switch(in_ctx->msg_hdr.type) {
            case TXPC_NEG_TYPE_CRC_CONFIG:
            break;
            case TXPC_NEG_TYPE_DISCONNECT:
            break;
            case TXPC_NEG_TYPE_ENDIANNESS:
            break;
            case TXPC_NEG_TYPE_REPORT_VERSION:
            break;
            // not supporting other neg types for now
        }
    }
    else {
        // tell the io event manager to watch the output fd again.
        if(ctx->io_add_fd_cb != NULL) {
            ctx->io_add_fd_cb(ctx->io_event_context, sw_ent->fd);
        }
        if(msg_buf->size == sizeof(txpc_hdr_t) + in_ctx->msg_hdr.size) {
            xpc_msg_finalize(out_ctx->msg_queue, in_ctx->buf_id);
            in_ctx->msg_inflight = false;
        }
    }

    // a crc can be done here as well, if the message is complete.
done:
    return bytes_read;
}

int xpc_write_msg(xpc_router_t *ctx, int fd) {
    int bytes_written = 0;
    msg_buf_t *msg_buf;
    // get the context for this output fd
    xpc_out_ctx_t *out_ctx = hashmap_fetch(ctx->out_contexts, fd);
    if(hashmap_status(ctx->out_contexts) != ALC_HASHMAP_SUCCESS) {
        goto done;
    }

    // get finalized message, ensure it's the inflight one if there is a buffer
    // being sent right now.
    if(out_ctx->current_buf_id == -1) {
        msg_buf = xpc_msg_dequeue_final(out_ctx->msg_queue);
        // there are no complete messages
        if(msg_buf != NULL) {
            out_ctx->current_buf_id = msg_buf->buf_id;
        }
    }
    else {
        msg_buf = xpc_msg_getbuf(out_ctx->msg_queue, out_ctx->current_buf_id);
    }

    if(msg_buf != NULL) {
        bytes_written = write(fd, msg_buf->buf->buf + msg_buf->wr_offset, msg_buf->size);
        msg_buf->size -= bytes_written;
        msg_buf->wr_offset += bytes_written;
        // no data to write, we can clear
        xpc_msg_clear(out_ctx->msg_queue, msg_buf->buf_id);
        out_ctx->current_buf_id = -1;
    }
    else {
        // no messages are available for this fd
        // tell the io event system to not continue raising write ready events.
        if(ctx->io_del_fd_cb != NULL) {
            ctx->io_del_fd_cb(ctx->io_event_context, fd);
        }
    }
done:
    return bytes_written;
}

int xpc_set_route(xpc_router_t *ctx, int ifd, int ofd, int ito, int oto) {
    int status = 0;
    xpc_switch_tbl_entry_t key = {.fd = ifd, .to_chn = ito};
    xpc_switch_tbl_entry_t val = {.fd = ofd, .to_chn = oto};
    // does this union save stack space? no idea!
    union {
        xpc_in_ctx_t in_ctx;
        xpc_out_ctx_t out_ctx;
    } ctxts = {0};
    // XXX this is because sizeof(xpc_switch_tbl_entry_t) = 8.
    // thus, the dynabuf copies by value, and we need to pass the struct,
    // not a pointer to it.  now THAT is a frustrating little gotcha.
    hashmap_set(ctx->switch_tbl, *(void**)&key, *(void**)&val);
    if((status = hashmap_status(ctx->switch_tbl)) != ALC_HASHMAP_SUCCESS) {
        goto done;
    }
    
    hashmap_set(ctx->in_contexts, ifd, &ctxts.in_ctx);
    if((status = hashmap_status(ctx->in_contexts)) != ALC_HASHMAP_SUCCESS) {
        goto done;
    }
    if(hashmap_fetch(ctx->out_contexts, ofd) == NULL) {
        create_xpc_out_ctx(&ctxts.out_ctx);
        hashmap_set(ctx->out_contexts, ofd, &ctxts.out_ctx);
        status = (hashmap_status(ctx->out_contexts) != ALC_HASHMAP_SUCCESS);
    }
done:
    return status;
}


int xpc_remove_route(xpc_router_t *ctx, int ifd, int ito) {
    xpc_switch_tbl_entry_t key = {.fd = ifd, .to_chn = ito};
    hashmap_remove(ctx->switch_tbl, *(void**)&key);
    hashmap_remove(ctx->in_contexts, *(void**)&key);
    // what about the output context?
    return hashmap_status(ctx->switch_tbl) != ALC_HASHMAP_SUCCESS;
}
