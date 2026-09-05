#ifndef CSI_UBUS_H
#define CSI_UBUS_H
#include "config.h"
#include <stdbool.h>
/* Runs a ubus connection loop in its own thread until *running is false. */
int  ubus_start(const csi_config_t *cfg, volatile bool *running);
void ubus_stop(void);
#endif
