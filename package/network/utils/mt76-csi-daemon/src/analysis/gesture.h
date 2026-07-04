#ifndef CSI_GESTURE_H
#define CSI_GESTURE_H
#include "../csi_frame.h"
void gesture_init(void);
void gesture_process(const csi_frame_t *frame);
void gesture_deinit(void);
#endif
