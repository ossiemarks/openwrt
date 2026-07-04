#ifndef CSI_QUEUE_H
#define CSI_QUEUE_H

#include <stdbool.h>
#include "csi_frame.h"

#define CSI_QUEUE_SIZE 1024

typedef struct csi_queue csi_queue_t;

csi_queue_t *queue_create(void);
void queue_destroy(csi_queue_t *q);

/* producer: copies frame in; drops oldest when full */
void queue_push(csi_queue_t *q, const csi_frame_t *f);

/* consumer: blocks up to timeout_ms; returns true if a frame was popped */
bool queue_pop(csi_queue_t *q, csi_frame_t *out, int timeout_ms);

void queue_shutdown(csi_queue_t *q);

#endif /* CSI_QUEUE_H */
