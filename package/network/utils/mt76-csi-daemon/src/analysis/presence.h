#ifndef CSI_PRESENCE_H
#define CSI_PRESENCE_H
#include "../csi_frame.h"
void presence_init(void);
void presence_process(const csi_frame_t *frame);
void presence_deinit(void);
#endif
