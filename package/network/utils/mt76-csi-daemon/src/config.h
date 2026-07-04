#ifndef CSI_CONFIG_H
#define CSI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define CSI_MAX_ZONES     8
#define CSI_ZONE_NAME_LEN 32
#define CSI_IFACE_LEN     16

typedef struct {
    char   iface[CSI_IFACE_LEN];
    int    bandwidth;           /* 20, 40, or 80 MHz */

    bool   udp_enabled;
    char   udp_host[64];
    int    udp_port;

    bool   mqtt_enabled;
    char   mqtt_broker[64];
    int    mqtt_port;
    char   mqtt_topic_raw[64];
    char   mqtt_topic_event[64];

    bool   rest_enabled;
    int    rest_port;

    bool   ubus_enabled;
    char   ubus_object[32];

    int    zone_count;
    char   zone_names[CSI_MAX_ZONES][CSI_ZONE_NAME_LEN];
} csi_config_t;

int  config_load(const char *path, csi_config_t *cfg);
void config_defaults(csi_config_t *cfg);

#endif /* CSI_CONFIG_H */
