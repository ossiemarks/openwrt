#ifndef CSI_FRAME_H
#define CSI_FRAME_H

#include <stdint.h>

#define CSI_MAX_SUBCARRIERS 512

typedef struct {
    uint32_t ts;          /* firmware timestamp (ms) */
    uint8_t  ta[6];       /* transmitter address */
    int8_t   rssi;
    uint8_t  snr;
    uint8_t  data_bw;
    uint8_t  pri_ch_idx;
    uint8_t  rx_mode;
    uint16_t tx_idx;
    uint16_t rx_idx;
    uint32_t chain_info;
    uint32_t ext_info;
    uint16_t data_num;
    int16_t  data_i[CSI_MAX_SUBCARRIERS];
    int16_t  data_q[CSI_MAX_SUBCARRIERS];
} csi_frame_t;

#endif /* CSI_FRAME_H */
