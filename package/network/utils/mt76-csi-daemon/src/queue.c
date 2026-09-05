#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include "queue.h"

struct csi_queue {
    csi_frame_t frames[CSI_QUEUE_SIZE];
    unsigned head;   /* next write */
    unsigned tail;   /* next read */
    unsigned count;
    bool shutdown;
    pthread_mutex_t lock;
    pthread_cond_t nonempty;
};

csi_queue_t *queue_create(void)
{
    csi_queue_t *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->nonempty, NULL);
    return q;
}

void queue_destroy(csi_queue_t *q)
{
    if (!q)
        return;
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->nonempty);
    free(q);
}

void queue_push(csi_queue_t *q, const csi_frame_t *f)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == CSI_QUEUE_SIZE) {
        /* drop oldest */
        q->tail = (q->tail + 1) % CSI_QUEUE_SIZE;
        q->count--;
    }
    memcpy(&q->frames[q->head], f, sizeof(*f));
    q->head = (q->head + 1) % CSI_QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->nonempty);
    pthread_mutex_unlock(&q->lock);
}

bool queue_pop(csi_queue_t *q, csi_frame_t *out, int timeout_ms)
{
    struct timespec ts;
    bool ok = false;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&q->lock);
    while (!q->count && !q->shutdown) {
        if (pthread_cond_timedwait(&q->nonempty, &q->lock, &ts) == ETIMEDOUT)
            break;
    }
    if (q->count) {
        memcpy(out, &q->frames[q->tail], sizeof(*out));
        q->tail = (q->tail + 1) % CSI_QUEUE_SIZE;
        q->count--;
        ok = true;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

void queue_shutdown(csi_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->shutdown = true;
    pthread_cond_broadcast(&q->nonempty);
    pthread_mutex_unlock(&q->lock);
}
