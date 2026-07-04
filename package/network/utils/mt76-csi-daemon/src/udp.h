#ifndef CSI_UDP_H
#define CSI_UDP_H
#include "csi_frame.h"
#include "config.h"
int  udp_init(const csi_config_t *cfg);
void udp_send(const csi_frame_t *f);
void udp_deinit(void);
#endif
