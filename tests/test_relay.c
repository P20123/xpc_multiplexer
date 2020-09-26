#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <alibc/containers/array.h>
#include <alibc/containers/iterator.h>
#include <alibc/containers/array_iterator.h>
#include <xpc_relay.h>
#include <crc.h>
#include <setjmp.h>


typedef struct {
    int read_fd, write_fd;
    int read_offset, write_offset;
    char read_buf[255];
} test_io_ctx_t;

int test_read_wrapper(void *io_ctx, char **buffer, int offset, size_t bytes_max) {
    test_io_ctx_t *ctx = (test_io_ctx_t*)io_ctx;
    int bytes = 0;
    // read into the specified location if read_buf is set, otherwise store it
    // in our own context.
    if(*buffer == NULL) {
        bytes = read(ctx->read_fd, ctx->read_buf + offset, bytes_max);
        // tell the relay where we read into
        *buffer = (char*)ctx->read_buf;
    }
    else {
        bytes = read(ctx->read_fd, *buffer, bytes_max);
    }
    if(bytes > 0) {
        ctx->read_offset += bytes;
    }
    else {
        bytes = 0;
    }
    return bytes > 0 ? bytes:0;
}

int test_write_wrapper(void *io_ctx, char **buffer, int offset, size_t bytes_max) {
    test_io_ctx_t *ctx = (test_io_ctx_t*)io_ctx;
    int bytes = write(ctx->write_fd, *buffer + offset, bytes_max);
    if(bytes > 0) {
        ctx->write_offset += bytes;
    }
    else {
        // something went wrong. FIXME we have no way to say "write failed"...
        // should we pass this through to the relay?
        bytes = 0;
    }
    return bytes;
}

void test_reset_fn(void *io_ctx, int which, size_t bytes) {
    test_io_ctx_t *ctx = (test_io_ctx_t*)io_ctx;
    // we don't handle more than one message at a time in this impl,
    // so we can just reset to zero bytes offset.
    if(which) {
        printf("io read reset called\n");
        ctx->read_offset = 0;
    }
    else {
        printf("io write reset called\n");
        ctx->write_offset = 0;
    }
}

bool test_msg_dispatch_fn(void *msg_ctx, txpc_hdr_t *msg, char *payload) {
    printf("message dispatch called\n");
    payload[msg->size] = 0; // do not print crc at end of payload
    printf("[%i -> %i] %s", msg->from, msg->to, payload);
    return true;
}


void xpc_send_block(int fd, int type, int to, int from, char *payload, size_t bytes) {
    int bytes_written = 0;
    txpc_hdr_t hdr = {.size = bytes, .to = to, .from = from, .type = type};
    while(bytes_written < bytes + 5) {
        if(bytes_written < 5) {
            bytes_written += write(fd, &hdr, 5);
        }
        else {
            bytes_written += write(fd, payload, bytes);
        }
    }
}

int test_nocrc(void) {
    int fd_set1[2] = {0};
    int fd_set2[2] = {0};
    int r = pipe(fd_set1);
    if(r == -1) {
        goto done;
    }
    r = pipe(fd_set2);
    if(r == -1) {
        close(fd_set1[0]);
        close(fd_set1[1]);
        goto done;
    }

    test_io_ctx_t ctx1 = {0};
    test_io_ctx_t ctx2 = {0};
    xpc_relay_state_t uut1 = {0};
    xpc_relay_state_t uut2 = {0};

    xpc_relay_config(
        &uut1, &ctx1, NULL, NULL,
        test_write_wrapper, test_read_wrapper, test_reset_fn,
        test_msg_dispatch_fn, NULL
    );
    xpc_relay_config(
        &uut2, &ctx2, NULL, NULL,
        test_write_wrapper, test_read_wrapper, test_reset_fn,
        test_msg_dispatch_fn, NULL
    );

    ctx1.write_fd = fd_set1[1];
    ctx2.read_fd = fd_set1[0];

    ctx2.write_fd = fd_set2[1];
    ctx1.read_fd = fd_set2[0];

    // send reset
    xpc_relay_send_reset(&uut1);
    printf("UUT1\n");
    xpc_wr_op_continue(&uut1);
    /*xpc_send_block(ctx1.write_fd, 1, 0, 0, NULL, 0); // manual reset*/
    /*uut1.signals &= ~SIG_RST_SEND;*/


    // receive reset and reply
    printf("UUT2\n");
    xpc_rd_op_continue(&uut2);
    xpc_wr_op_continue(&uut2);
    /*xpc_send_block(ctx2.write_fd, 1, 0, 0, NULL, 0); // manual reset*/
    /*uut2.signals &= ~SIG_RST_RECVD;*/

    // receive reply
    printf("UUT1\n");
    xpc_rd_op_continue(&uut1);
    printf("--->reset test complete\n");

    // reset sequence complete, send a message!
    printf("UUT1\n");
    // an extra wr op is required, since we are in reset until rx sm gets reply
    // and the next write operation occurs.
    xpc_wr_op_continue(&uut1);
    xpc_send_msg(&uut1, 1, 1, "hello uut2!\n", 12);
    xpc_wr_op_continue(&uut1);
    printf("UUT2\n");
    xpc_rd_op_continue(&uut2);

    xpc_send_msg(&uut2, 1, 1, "hello uut1!\n", 12);
    xpc_wr_op_continue(&uut2);
    printf("UUT1\n");
    xpc_rd_op_continue(&uut1);

    close(fd_set1[0]);
    close(fd_set1[1]);
    close(fd_set2[0]);
    close(fd_set2[1]);
done:
    return r;
}

int test_dual_reset(void) {
    int fd_set1[2] = {0};
    int fd_set2[2] = {0};
    int r = pipe(fd_set1);
    if(r == -1) {
        goto done;
    }
    r = pipe(fd_set2);
    if(r == -1) {
        close(fd_set1[0]);
        close(fd_set1[1]);
        goto done;
    }

    test_io_ctx_t ctx1 = {0};
    test_io_ctx_t ctx2 = {0};
    xpc_relay_state_t uut1 = {0};
    xpc_relay_state_t uut2 = {0};

    xpc_relay_config(
        &uut1, &ctx1, NULL, NULL,
        test_write_wrapper, test_read_wrapper, test_reset_fn,
        test_msg_dispatch_fn, NULL
    );
    xpc_relay_config(
        &uut2, &ctx2, NULL, NULL,
        test_write_wrapper, test_read_wrapper, test_reset_fn,
        test_msg_dispatch_fn, NULL
    );

    ctx1.write_fd = fd_set1[1];
    ctx2.read_fd = fd_set1[0];

    ctx2.write_fd = fd_set2[1];
    ctx1.read_fd = fd_set2[0];

    // send reset
    printf("UUT1\n");
    xpc_relay_send_reset(&uut1);
    xpc_wr_op_continue(&uut1);

    printf("UUT2\n");
    xpc_relay_send_reset(&uut2);
    xpc_wr_op_continue(&uut2);
    /*xpc_send_block(ctx1.write_fd, 1, 0, 0, NULL, 0); // manual reset*/
    /*uut1.signals &= ~SIG_RST_SEND;*/


    // receive reset and reply
    printf("UUT1\n");
    xpc_rd_op_continue(&uut1);
    printf("UUT2\n");
    xpc_rd_op_continue(&uut2);
    /*xpc_send_block(ctx2.write_fd, 1, 0, 0, NULL, 0); // manual reset*/
    /*uut2.signals &= ~SIG_RST_RECVD;*/

    printf("--->reset test complete\n");

    // reset sequence complete, send a message!
    printf("UUT1\n");
    // both machines sent a reset, so both will have a one-event latency on
    // being ready to write again (must first ack RECVD signal from rx sm)
    xpc_wr_op_continue(&uut1);
    xpc_wr_op_continue(&uut2);
    xpc_send_msg(&uut1, 1, 1, "hello uut2!\n", 12);
    xpc_wr_op_continue(&uut1);
    printf("UUT2\n");
    xpc_rd_op_continue(&uut2);

    xpc_send_msg(&uut2, 1, 1, "hello uut1!\n", 12);
    xpc_wr_op_continue(&uut2);
    printf("UUT1\n");
    xpc_rd_op_continue(&uut1);

    close(fd_set1[0]);
    close(fd_set1[1]);
    close(fd_set2[0]);
    close(fd_set2[1]);
done:
    return r;
}


uint16_t crc_table[256];

typedef struct {
    crc_t crc;
} crc_ctx_t;


char *test_crc_fn(void *crc_ctx, char *buf, size_t bytes) {
    crc_ctx_t *ctx = (crc_ctx_t*)crc_ctx;
    ctx->crc = crc_init();
    ctx->crc = crc_update(ctx->crc, buf, bytes);
    ctx->crc = crc_finalize(ctx->crc);
    return (char*)&ctx->crc;
}

int test_withcrc(void) {
    int fd_set1[2] = {0};
    int fd_set2[2] = {0};
    int r = pipe(fd_set1);
    if(r == -1) {
        goto done;
    }
    r = pipe(fd_set2);
    if(r == -1) {
        close(fd_set1[0]);
        close(fd_set1[1]);
        goto done;
    }

    test_io_ctx_t ctx1 = {0};
    test_io_ctx_t ctx2 = {0};
    xpc_relay_state_t uut1 = {0};
    xpc_relay_state_t uut2 = {0};
    crc_ctx_t crc1, crc2;

    xpc_relay_config(
        &uut1, &ctx1, NULL, &crc1,
        test_write_wrapper, test_read_wrapper, test_reset_fn,
        test_msg_dispatch_fn, test_crc_fn
    );
    xpc_relay_config(
        &uut2, &ctx2, NULL, &crc2,
        test_write_wrapper, test_read_wrapper, test_reset_fn,
        test_msg_dispatch_fn, test_crc_fn
    );

    // force relays to use crc without requiring full config msg.
    uut1.conn_config.crc_bits = 32;
    uut2.conn_config.crc_bits = 32;


    ctx1.write_fd = fd_set1[1];
    ctx2.read_fd = fd_set1[0];

    ctx2.write_fd = fd_set2[1];
    ctx1.read_fd = fd_set2[0];

    // send reset
    xpc_relay_send_reset(&uut1);
    printf("UUT1\n");
    xpc_wr_op_continue(&uut1);
    /*xpc_send_block(ctx1.write_fd, 1, 0, 0, NULL, 0); // manual reset*/
    /*uut1.signals &= ~SIG_RST_SEND;*/


    // receive reset and reply
    printf("UUT2\n");
    xpc_rd_op_continue(&uut2);
    xpc_wr_op_continue(&uut2);
    /*xpc_send_block(ctx2.write_fd, 1, 0, 0, NULL, 0); // manual reset*/
    /*uut2.signals &= ~SIG_RST_RECVD;*/

    // receive reply
    printf("UUT1\n");
    xpc_rd_op_continue(&uut1);
    printf("--->reset test complete\n");

    // reset sequence complete, send a message!
    printf("UUT1\n");
    // an extra wr op is required, since we are in reset until rx sm gets reply
    // and the next write operation occurs.
    // (this will not happen in a non-test case)
    xpc_wr_op_continue(&uut1);
    xpc_send_msg(&uut1, 1, 1, "hello uut2!\n", 12);
    xpc_wr_op_continue(&uut1);
    printf("UUT2\n");
    xpc_rd_op_continue(&uut2);

    xpc_send_msg(&uut2, 1, 1, "hello uut1!\n", 12);
    xpc_wr_op_continue(&uut2);
    printf("UUT1\n");
    xpc_rd_op_continue(&uut1);

    close(fd_set1[0]);
    close(fd_set1[1]);
    close(fd_set2[0]);
    close(fd_set2[1]);
done:
    return r;
}

int main(void) {
    test_withcrc();
    return 0;
}
