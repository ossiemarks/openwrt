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
    /*
     * 24-byte header followed by data_num interleaved I/Q int16 pairs
     * (4 bytes per subcarrier). All multi-byte fields are host byte order
     * (little endian on the target). Byte offsets:
     *
     *   0..3   ts          uint32  firmware timestamp (ms)
     *   4..9   ta          uint8[6] transmitter address
     *   10     rssi        int8
     *   11     snr         uint8
     *   12     data_bw     uint8
     *   13     pri_ch_idx  uint8
     *   14     rx_mode     uint8
     *   15     pad         uint8   always 0
     *   16..17 data_num    uint16  subcarriers carried in this datagram
     *   18..19 tx_idx      uint16  tx antenna index
     *   20..21 rx_idx      uint16  rx antenna index
     *   22..23 chain_info  uint16  low 16 bits of chain_info;
     *                              bit 15 marks the last chain of a group
     *   24..   data_i[0], data_q[0], data_i[1], data_q[1], ... (int16 each)
     */
    if (!enabled || sock < 0)
        return;

    size_t payload = 24 + (size_t)f->data_num * 4;
    unsigned char buf[24 + CSI_MAX_SUBCARRIERS * 4];
    unsigned char *p = buf;
    uint16_t chain_info = (uint16_t)f->chain_info;

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
    memcpy(p, &chain_info, 2); p += 2;
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
