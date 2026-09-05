#ifndef CSI_VITALS_H
#define CSI_VITALS_H
#include "../csi_frame.h"
void vitals_init(void);
void vitals_process(const csi_frame_t *frame);
void vitals_deinit(void);
#endif
