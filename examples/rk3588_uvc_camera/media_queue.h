/*
 * media_queue.h — Thread-safe encoded media frame queue
 *
 * A bounded ring of encoded media packets (H.264 access units for
 * video, Opus packets for audio) shared between a capture/encode
 * thread (producer) and the main event loop (consumer).
 *
 * Two instances live inside media_pipeline_t, one per kind. The
 * kind tag is carried here so diagnostics and stats can distinguish
 * video drops from audio drops without the consumer needing to
 * track which queue is which.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MEDIA_QUEUE_H_
#define MEDIA_QUEUE_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_KIND_VIDEO,
    MEDIA_KIND_AUDIO,
} media_kind_t;

typedef struct {
    uint8_t *data;
    size_t   len;
    uint32_t pts_ms;
    bool     is_keyframe; /**< Video only; always false for audio. */
} media_frame_t;

typedef struct {
    media_kind_t     kind;
    int              capacity;   /**< Slot count, fixed at init. */
    media_frame_t   *slots;      /**< Heap-allocated ring of @c capacity entries. */
    int              head;
    int              count;
    uint32_t         drop_count; /**< Frames overwritten due to overflow. */
    pthread_mutex_t  lock;
    int              wake_pipe[2]; /**< pipe[0]=read (select), pipe[1]=write. */
} media_queue_t;

/**
 * @brief Initialize a media queue with the given capacity.
 * @return 0 on success, -1 on failure (pipe/alloc error).
 */
int  media_queue_init(media_queue_t *q, media_kind_t kind, int capacity);

/** @brief Free all pending frames and release OS resources. */
void media_queue_destroy(media_queue_t *q);

/**
 * @brief Copy @p data into the queue (producer thread).
 *
 * If the queue is full, drops the oldest frame and increments
 * @c drop_count. Wakes the consumer via the wake pipe.
 */
void media_queue_push(media_queue_t *q, const uint8_t *data, size_t len,
                      uint32_t pts_ms, bool is_keyframe);

/**
 * @brief Pop the oldest frame (consumer thread). Ownership of
 *        @c out->data is transferred to the caller; free it with free().
 * @return 0 on success, -1 if empty.
 */
int  media_queue_pop(media_queue_t *q, media_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_QUEUE_H_ */
