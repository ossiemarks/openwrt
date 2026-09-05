#ifndef CSI_NETLINK_H
#define CSI_NETLINK_H

#include <stdbool.h>
#include "queue.h"

#include <stddef.h>
#include <stdint.h>

/* Pick the AP-mode interface with the most associated stations. Writes the
 * name into buf; returns its station count, or -1 if no AP interface exists. */
int csi_nl_find_ap_iface(char *buf, size_t len);

/* Associated stations on iface. Fills up to max MACs; returns the total
 * number associated (may exceed max), or -1 on error. */
int csi_nl_list_stations(const char *iface, uint8_t (*macs)[6], int max);

/* Add (add=true) or remove a station from the firmware CSI MAC filter.
 * interval is the per-station report interval in us (0 = every frame). */
int csi_nl_sta_filter(const char *iface, const uint8_t *mac, bool add,
		      unsigned interval);

/* mode 2 / type 3: frame-type filter. Without this the firmware accepts
 * every other CSI command but never reports a frame. v1/v2 as in
 * MtkCSIdump (0, 34); other values also produce reports. */
int csi_nl_set_frame_type(const char *iface, uint8_t v1, uint8_t v2);

/* mode 1 / mode 0: start / stop CSI capture on iface. */
int csi_nl_enable(const char *iface);
int csi_nl_disable(const char *iface);

/* Blocking reader loop: polls the vendor dump interface and pushes
 * frames into q until *running goes false. */
int csi_nl_reader_run(const char *iface, csi_queue_t *q, volatile bool *running);

#endif
