#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "udp.h"

static int sock = -1;
static struct sockaddr_in dst;
static bool enabled;

int udp_init(const csi_config_t *cfg)
{
    enabled = cfg->udp_enabled;
    if (!enabled)
        return 0;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg->udp_port);
    if (inet_pton(AF_INET, cfg->udp_host, &dst.sin_addr) != 1) {
        close(sock);
        sock = -1;
        return -1;
    }
    return 0;
}

void udp_send(const csi_frame_t *f)
{
    /* header (fixed) + interleaved I/Q int16 for data_num subcarriers */
    if (!enabled || sock < 0)
        return;

    size_t payload = 24 + (size_t)f->data_num * 4;
    unsigned char buf[24 + CSI_MAX_SUBCARRIERS * 4];
    unsigned char *p = buf;

    memcpy(p, &f->ts, 4); p += 4;
    memcpy(p, f->ta, 6);  p += 6;
    *p++ = (unsigned char)f->rssi;
    *p++ = f->snr;
    *p++ = f->data_bw;
    *p++ = f->pri_ch_idx;
    *p++ = f->rx_mode;
    *p++ = 0; /* pad */
    memcpy(p, &f->data_num, 2); p += 2;
    memcpy(p, &f->tx_idx, 2); p += 2;
    memcpy(p, &f->rx_idx, 2); p += 2;
    for (int i = 0; i < f->data_num; i++) {
        memcpy(p, &f->data_i[i], 2); p += 2;
        memcpy(p, &f->data_q[i], 2); p += 2;
    }

    sendto(sock, buf, payload, 0, (struct sockaddr *)&dst, sizeof(dst));
}

void udp_deinit(void)
{
    if (sock >= 0)
        close(sock);
    sock = -1;
}
