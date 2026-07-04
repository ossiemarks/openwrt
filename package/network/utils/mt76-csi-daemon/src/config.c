#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "config.h"

void config_defaults(csi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->iface, sizeof(cfg->iface), "ra0");
    cfg->bandwidth = 80;

    cfg->udp_enabled = false;
    snprintf(cfg->udp_host, sizeof(cfg->udp_host), "127.0.0.1");
    cfg->udp_port = 5500;

    cfg->mqtt_enabled = false;
    snprintf(cfg->mqtt_broker, sizeof(cfg->mqtt_broker), "127.0.0.1");
    cfg->mqtt_port = 1883;
    snprintf(cfg->mqtt_topic_raw, sizeof(cfg->mqtt_topic_raw), "csi/raw");
    snprintf(cfg->mqtt_topic_event, sizeof(cfg->mqtt_topic_event), "csi/event");

    cfg->rest_enabled = true;
    cfg->rest_port = 8080;

    cfg->ubus_enabled = true;
    snprintf(cfg->ubus_object, sizeof(cfg->ubus_object), "csi");

    cfg->zone_count = 0;
}

static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;
    if (!*s)
        return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static bool parse_bool(const char *v)
{
    return !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1");
}

int config_load(const char *path, csi_config_t *cfg)
{
    char line[256], section[32] = "";
    FILE *f;

    config_defaults(cfg);

    f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line), *eq, *key, *val;

        if (!*s || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close) {
                *close = '\0';
                snprintf(section, sizeof(section), "%s", s + 1);
            }
            continue;
        }

        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);

        if (!strcmp(section, "csi")) {
            if (!strcmp(key, "interface"))
                snprintf(cfg->iface, sizeof(cfg->iface), "%s", val);
            else if (!strcmp(key, "bandwidth"))
                cfg->bandwidth = atoi(val);
        } else if (!strcmp(section, "udp")) {
            if (!strcmp(key, "enabled"))
                cfg->udp_enabled = parse_bool(val);
            else if (!strcmp(key, "host"))
                snprintf(cfg->udp_host, sizeof(cfg->udp_host), "%s", val);
            else if (!strcmp(key, "port"))
                cfg->udp_port = atoi(val);
        } else if (!strcmp(section, "mqtt")) {
            if (!strcmp(key, "enabled"))
                cfg->mqtt_enabled = parse_bool(val);
            else if (!strcmp(key, "broker"))
                snprintf(cfg->mqtt_broker, sizeof(cfg->mqtt_broker), "%s", val);
            else if (!strcmp(key, "port"))
                cfg->mqtt_port = atoi(val);
            else if (!strcmp(key, "topic_raw"))
                snprintf(cfg->mqtt_topic_raw, sizeof(cfg->mqtt_topic_raw), "%s", val);
            else if (!strcmp(key, "topic_event"))
                snprintf(cfg->mqtt_topic_event, sizeof(cfg->mqtt_topic_event), "%s", val);
        } else if (!strcmp(section, "rest")) {
            if (!strcmp(key, "enabled"))
                cfg->rest_enabled = parse_bool(val);
            else if (!strcmp(key, "port"))
                cfg->rest_port = atoi(val);
        } else if (!strcmp(section, "ubus")) {
            if (!strcmp(key, "enabled"))
                cfg->ubus_enabled = parse_bool(val);
            else if (!strcmp(key, "object"))
                snprintf(cfg->ubus_object, sizeof(cfg->ubus_object), "%s", val);
        } else if (!strcmp(section, "zones")) {
            if (!strncmp(key, "zone_", 5) && cfg->zone_count < CSI_MAX_ZONES) {
                snprintf(cfg->zone_names[cfg->zone_count],
                         CSI_ZONE_NAME_LEN, "%s", val);
                cfg->zone_count++;
            }
        }
    }

    fclose(f);
    return 0;
}
