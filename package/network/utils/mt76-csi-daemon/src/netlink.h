#ifndef CSI_NETLINK_H
#define CSI_NETLINK_H

#include <stdbool.h>
#include "queue.h"

/* Enable CSI on iface. If sta_mac is non-NULL ("aa:bb:..." string),
 * install a per-station MAC filter first. interval is the fw report
 * interval in us (0 = every frame). */
int csi_nl_enable(const char *iface, const char *sta_mac, unsigned interval);
int csi_nl_disable(const char *iface);

/* Blocking reader loop: polls the vendor dump interface and pushes
 * frames into q until *running goes false. */
int csi_nl_reader_run(const char *iface, csi_queue_t *q, volatile bool *running);

#endif
