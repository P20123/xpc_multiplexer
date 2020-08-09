#include <stdio.h>
#include <string.h>
#include <alibc/containers/array.h>
#include <alibc/containers/iterator.h>
#include <alibc/containers/array_iterator.h>
#include <xpc_msg_queue.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>

static int init(void **state) {
    *state = create_msg_queue();
    assert_non_null(*state);
    return 0;
}

static int finish(void **state) {
    xpc_msg_queue_destroy(*state);
    return 0;
}


static void test_getbuf(void **state) {
    msg_queue_t *q = *state;
    // ask for a buffer that doesn't exist (empty)
    int id = 10;
    msg_buf_t *buf = xpc_msg_getbuf(q, id);
    assert_null(buf);
    // ask for a new buffer
    id = -1;
    buf = xpc_msg_getbuf(q, id);
    assert_non_null(buf);
    dynabuf_resize(buf->buf, 4);
    dynabuf_set(buf->buf, 0, 0xCA);
    dynabuf_set(buf->buf, 1, 0xFE);
    dynabuf_set(buf->buf, 2, 0xBA);
    dynabuf_set(buf->buf, 3, 0xBE);

    assert_int_not_equal(buf->buf_id, -1);
    // ask for the same buffer again
    msg_buf_t *buf2 = xpc_msg_getbuf(q, buf->buf_id);
    assert_non_null(buf2);
    assert_int_equal(*(char*)dynabuf_fetch(buf->buf, 0) & 0xff, 0xCA);
    assert_int_equal(*(char*)dynabuf_fetch(buf->buf, 1) & 0xff, 0xFE);
    assert_int_equal(*(char*)dynabuf_fetch(buf->buf, 2) & 0xff, 0xBA);
    assert_int_equal(*(char*)dynabuf_fetch(buf->buf, 3) & 0xff, 0xBE);
    // ask for a buffer that doesn't exist (non-empty)
    id = 10;
    buf = xpc_msg_getbuf(q, id);
    assert_null(buf);
}

static void test_finalize_dequeue(void **state) {
    msg_queue_t *q = *state;
    // create a bunch of empty buffers
    for(int i = 0; i < 10; i++) {
        int create_new_buffer = -1;
        xpc_msg_getbuf(q, create_new_buffer);
    }
    // finalize a few of them
    xpc_msg_finalize(q, 1);
    xpc_msg_finalize(q, 3);
    xpc_msg_finalize(q, 7);

    // XXX
    // we need a way to represent buffers that are finalized, but haven't been
    // cleared and thus cannot be reused.  We could simply un-set the finalized
    // status on each, and that would theoretically work, but it's ugly.
    // doing that for now...
    // dequeue three times
    msg_buf_t *buf1 = xpc_msg_dequeue_final(q);
    assert_non_null(buf1);
    msg_buf_t *buf2 = xpc_msg_dequeue_final(q);
    assert_non_null(buf2);
    assert_ptr_not_equal(buf1, buf2);
    msg_buf_t *buf3 = xpc_msg_dequeue_final(q);
    assert_non_null(buf3);
    assert_ptr_not_equal(buf2, buf3);
    assert_ptr_not_equal(buf1, buf3);

    // there are no more buffers to dequeue
    msg_buf_t *buf4 = xpc_msg_dequeue_final(q);
    assert_null(buf4);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(
            test_getbuf,
            init,
            finish
        ),
        cmocka_unit_test_setup_teardown(
            test_finalize_dequeue,
            init,
            finish
        ),
    };

    int r = cmocka_run_group_tests(tests, NULL, NULL);
    return r;
}
