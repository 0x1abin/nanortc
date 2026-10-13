/* Fair handoff between a blocking I/O owner and application senders. */
#ifndef INTEROP_OWNER_H_
#define INTEROP_OWNER_H_

#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t handoff;
    atomic_int senders;
} interop_owner_t;

static inline void interop_owner_init(interop_owner_t *owner)
{
    pthread_mutex_init(&owner->mutex, NULL);
    pthread_cond_init(&owner->handoff, NULL);
    atomic_init(&owner->senders, 0);
}

static inline void interop_owner_step_lock(interop_owner_t *owner)
{
    pthread_mutex_lock(&owner->mutex);
    /* pthread_mutex does not guarantee fairness. Do not reacquire it for
     * another blocking poll while an application sender is already waiting. */
    while (atomic_load(&owner->senders))
        pthread_cond_wait(&owner->handoff, &owner->mutex);
}

static inline void interop_owner_step_unlock(interop_owner_t *owner)
{
    pthread_mutex_unlock(&owner->mutex);
}

static inline void interop_owner_send_lock(interop_owner_t *owner)
{
    atomic_fetch_add(&owner->senders, 1);
    pthread_mutex_lock(&owner->mutex);
    atomic_fetch_sub(&owner->senders, 1);
}

static inline void interop_owner_send_unlock(interop_owner_t *owner)
{
    pthread_cond_signal(&owner->handoff);
    pthread_mutex_unlock(&owner->mutex);
}

static inline void interop_owner_destroy(interop_owner_t *owner)
{
    pthread_cond_destroy(&owner->handoff);
    pthread_mutex_destroy(&owner->mutex);
}

#endif
