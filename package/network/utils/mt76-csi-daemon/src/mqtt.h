#ifndef CSI_MQTT_H
#define CSI_MQTT_H
#include "csi_frame.h"
#include "config.h"
int  mqtt_start(const csi_config_t *cfg);
void mqtt_publish_raw(const csi_frame_t *f);
void mqtt_publish_event(const char *subtopic, const char *json);
void mqtt_stop(void);
#endif
