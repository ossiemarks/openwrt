#ifndef CSI_REST_H
#define CSI_REST_H
#include "config.h"
#include <stdbool.h>
int  rest_start(const csi_config_t *cfg, volatile bool *running);
void rest_stop(void);
#endif
