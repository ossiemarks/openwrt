#ifndef CSI_CONFIG_H
#define CSI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define CSI_MAX_ZONES     8
#define CSI_ZONE_NAME_LEN 32
#define CSI_IFACE_LEN     16
#define CSI_MAX_STATIONS  16

typedef struct {
    char   iface[CSI_IFACE_LEN];
    bool   iface_auto;          /* pick the AP interface with most stations */
    int    bandwidth;           /* 20, 40, or 80 MHz */

    /* Firmware only reports CSI for stations in its MAC filter list.
     * stations_auto keeps the list in sync with the associated stations;
     * otherwise the fixed list below is installed once at startup. */
    bool     stations_auto;
    int      station_count;
    uint8_t  stations[CSI_MAX_STATIONS][6];
    unsigned sta_interval;      /* per-station report interval, us (0 = every frame) */

    /* Frame-type filter (CSI cfg item 3). Mandatory: the firmware reports
     * nothing until it is set. Defaults match MtkCSIdump. */
    uint8_t  frame_type_v1;
    uint8_t  frame_type_v2;

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
int  config_parse_mac(const char *s, uint8_t *mac);
/* Append a fixed station; switches stations_auto off. */
int  config_add_station(csi_config_t *cfg, const char *mac_str);

#endif /* CSI_CONFIG_H */
