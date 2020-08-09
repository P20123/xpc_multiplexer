#include <stdlib.h>
#include <alibc/containers/dynabuf.h>
#include <alibc/containers/array.h>
#include <alibc/containers/hashmap.h>
#include <alibc/containers/bitmap.h>
#include <alibc/containers/iterator.h>
#include <alibc/containers/array_iterator.h>
#include <alibc/containers/hashmap_iterator.h>
#include <alibc/containers/hash_functions.h>
#include <alibc/containers/comparators.h>
#include <xpc_msg_queue.h>

msg_buf_t *create_msg_buf() {
    msg_buf_t *r = malloc(sizeof(msg_buf_t));
    if(r == NULL) {
        goto done;
    }
    r->buf = create_dynabuf(1, sizeof(char));
    if(r->buf == NULL) {
        free(r);
        r = NULL;
        goto done;
    }
    r->buf_id = 0;
    r->wr_offset = 0;
done:
    return r;
}


void msg_buf_free(msg_buf_t *self) {
    if(self != NULL) {
        dynabuf_free(self->buf);
        free(self);
    }
}

msg_queue_t *create_msg_queue() {
    msg_queue_t *r = malloc(sizeof(msg_queue_t));
    if(r == NULL) {
        goto done;
    }

    r->cleared_buffers = create_array(1, sizeof(msg_buf_t*));
    if(r->cleared_buffers == NULL) {
        free(r);
        goto done;
    }

    r->inflight_buffers = create_hashmap(
        1, sizeof(int), sizeof(msg_buf_t*),
        alc_default_hash_i32, alc_default_cmp_i32, NULL
    );
    if(r->inflight_buffers == NULL) {
        array_free(r->cleared_buffers);
        free(r);
        r = NULL;
        goto done;
    }
    r->final_buffer_marks = create_bitmap(1);
    if(r->final_buffer_marks == NULL) {
        hashmap_free(r->inflight_buffers);
        array_free(r->cleared_buffers);
        free(r);
        r = NULL;
        goto done;
    }
    r->current_min_id = 0;
done:
    return r;
}


msg_buf_t *xpc_msg_getbuf(msg_queue_t *self, int id) {
    msg_buf_t *r = NULL;
    msg_buf_t **tmp = NULL;
    // caller is requesting a new buffer be created.
    if(id < 0) {
        // use an already-malloc'd buffer if possible.
        if(array_size(self->cleared_buffers) > 0) {
            tmp = array_remove(self->cleared_buffers, 0);
            r = (tmp == NULL) ? NULL:*tmp;
        }
        // otherwise, make a new one and hold onto it.
        else {
            r = create_msg_buf();
            if(r == NULL) {
                goto done;
            }
        }

        hashmap_set(self->inflight_buffers, self->current_min_id, r);
        bitmap_resize(
            self->final_buffer_marks, hashmap_size(self->inflight_buffers)
        );
        r->buf_id = self->current_min_id;
        // increase the min_id until it is no longer found in the hashmap.
        // it will get reset lower in calls to dequeue(), so this loop is
        // bounded by limits other than available memory.
        while(hashmap_fetch(self->inflight_buffers, ++self->current_min_id));
    }
    else {
        tmp = hashmap_fetch(self->inflight_buffers, id);
        r = (tmp == NULL) ? NULL:*tmp;
    }
done:
    return r;
}

int xpc_msg_finalize(msg_queue_t *self, int which) {
    int r = 0;
    if(hashmap_fetch(self->inflight_buffers, which) == NULL) {
        r = -1;
        goto done;
    }
    bitmap_add(self->final_buffer_marks, which);
done:
    return r;
}

msg_buf_t *xpc_msg_dequeue_final(msg_queue_t *self) {
    msg_buf_t *r = NULL;
    iter_context *it = create_hashmap_keys_iterator(self->inflight_buffers);
    while(iter_status(it) != ALC_ITER_STOP) {
        int *pnext = iter_next(it);
        if(pnext == NULL) break;
        int next = *pnext;
        if(bitmap_contains(self->final_buffer_marks, next)) {
            r = *hashmap_remove(self->inflight_buffers, next);
            // marked as inflight nonfinal, even though it is, to prevent
            // re-dequeueing this message. call clear() to allow this buffer
            // to be re-used.
            bitmap_remove(self->final_buffer_marks, next);
            break;
        }
    }
    iter_free(it);

    return r;
}

int xpc_msg_clear(msg_queue_t *self, int which) {
    int r = -1;
    msg_buf_t *buf = hashmap_remove(self->inflight_buffers, which);
    if(buf != NULL) {
        array_append(self->cleared_buffers, buf);
        buf->size = 0;
        buf->buf_id = 0;
        buf->wr_offset = 0;
        bitmap_remove(self->final_buffer_marks, which);
        // bring down the min_id to this index if it is lower than the
        // current minimum - otherwise the linear search in getbuf()
        // will find it.
        if(self->current_min_id > which) {
            self->current_min_id = which;
        }
        r = 0;
    }
    return r;
}

void xpc_msg_queue_destroy(msg_queue_t *self) {
    if(self != NULL) {
        iter_context *it = create_array_iterator(self->cleared_buffers);
        msg_buf_t **next;
        while((next = iter_next(it)) != NULL) {
            msg_buf_free(*next);
        }
        iter_free(it);

        it = create_hashmap_values_iterator(self->inflight_buffers);
        next = iter_next(it);
        while(iter_status(it) != ALC_ITER_STOP) {
            msg_buf_free(*next);
            next = iter_next(it);
        }
        iter_free(it);

        array_free(self->cleared_buffers);
        hashmap_free(self->inflight_buffers);
        bitmap_free(self->final_buffer_marks);
        free(self);
    }
}
