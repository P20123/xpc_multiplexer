#pragma once
#include <alibc/containers/dynabuf.h>
#include <alibc/containers/array.h>
#include <alibc/containers/hashmap.h>
#include <alibc/containers/bitmap.h>

/**
 * Message buffer type
 * This is used to store a message and the size of the message, in bytes.
 * It forms a single entry in a msg_queue_t structure.
 * This may as well be array_t, but we aren't supporting the array interface,
 * so it gets a different name.
 */
typedef struct {
    int size;
    // this is the linking key for both writing an in-progress message
    // and clearing a buffer after it is finalized.
    int buf_id;
    // this is the offset for writing
    int wr_offset;
    dynabuf_t *buf;
} msg_buf_t;

/**
 * Message queue data type
 * The message queue is a collection of buffers of dynamic length.
 * Buffers marked as final are dequeue'd in unspecified order, and can be
 * cleared to allow them to be re-used without requiring a system call.
 */
typedef struct {
    array_t *cleared_buffers;
    hashmap_t *inflight_buffers;
    bitmap_t *final_buffer_marks;
    int current_min_id;
} msg_queue_t;

/**
 * Create a new message buffer.
 * @return a new message buffer, or NULL on failure.
 */
msg_buf_t *create_msg_buf();

/**
 * Destroy a message buffer.
 * @param self the message buffer to destroy. Associated data is lost.
 */
void msg_buf_free(msg_buf_t *self);

/**
 * Create a new message queue
 * @return a new message queue, or NULL on failure.
 */
msg_queue_t *create_msg_queue();


/**
 * Retrieve (and possibly allocate space for) a buffer to hold a new message
 * in the specified queue.
 * The value of id is interpreted as a signed integer.
 * If the value at id is less than zero, a new buffer is created,
 * and this function should only be called this way if a new message is being
 * added to the queue.  The new id of the buffer is stored in the returned
 * msg_buf_t, and should be used in subsequent calls to getbuf,
 * finalize, and clear in order to access the same buffer.
 * @param self message queue to use
 * @param id buffer id, or -1 to obtain an empty buffer.
 * @return a msg_buf_t instance, or NULL on failure.
 */
msg_buf_t *xpc_msg_getbuf(msg_queue_t *self, int id);

/**
 * Mark a message's buffer as final, indicating that it can be dequeued safely.
 * After calling this function, the buffer pointed to by which is considered
 * to be empty.  Its corresponding id specified by which is no longer valid.
 * @param self the message queue to use
 * @param which the id of a buffer to be finalized.
 * @return 0 on success, -1 if the id (which) is not valid.
 */
int xpc_msg_finalize(msg_queue_t *self, int which);

/**
 * Retrieve a message's buffer which is finalized.
 * If no messages are finalized, NULL will be returned.
 * @param self the message quee to use
 * @return an msg_buf_t whose contents are one complete message, or NULL if no
 * such buffers exist in the queue.
 */
msg_buf_t *xpc_msg_dequeue_final(msg_queue_t *self);

/**
 * Clear the contents of a message's buffer. This does not necessarily cause it
 * to be cleared in a particular way, but rather marks the buffer as being
 * available for use with another message without finalizing it.  This is useful
 * for failed or dropped messages.
 * @param self the message queue to use
 * @param which the id of the buffer to clear
 * @return 0 on success, -1 if the id (which) is not valid.
 */
int xpc_msg_clear(msg_queue_t *self, int which);

/**
 * Destroy an existing message queue, and all buffers associated with it.
 * @param self the message queue to destroy
 */
void xpc_msg_queue_destroy(msg_queue_t *self);
