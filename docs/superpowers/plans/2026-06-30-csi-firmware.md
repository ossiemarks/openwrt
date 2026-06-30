# CSI Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add MT7915 CSI extraction to OpenWrt for GL-MT3000, exposing raw CSI frames and processed presence/position/gesture/vitals events via UDP, MQTT, ubus, and REST.

**Architecture:** Kernel patches hook into the mt7915e RX path and push CSI frames via netlink. A userspace daemon reads the netlink socket, fans raw frames to UDP, and runs analysis modules publishing events to MQTT, ubus, and a REST server. A thin LuCI app surfaces live status.

**Tech Stack:** C (daemon + kernel patches), OpenWrt package build system, libnl-tiny (netlink), libmosquitto (MQTT), uhttpd/libubox (REST), libubus (ubus), LuCI (Lua/JS)

## Global Constraints

- Target device: GL.iNet GL-MT3000 (MediaTek MT7981B, MT7915e WiFi)
- OpenWrt branch: `feature/csi-mt7915` on `https://github.com/ossiemarks/openwrt`
- Kernel: 5.4.x (mediatek/filogic target)
- WiFi driver: mt76 @ commit `2dd6e4c8892f59b7943ee163afd6ced881bfb31b`
- All daemon code: pure C99, no C++ 
- Config file: `/etc/mt76-csi.conf` (INI format)
- REST port: 8080
- UDP default port: 5500
- MQTT default topic prefix: `csi/`
- No authorship comments in source files

---

## File Map

```
package/kernel/mt76/patches/csi/
  001-mt7915-csi-header.patch          ← adds mt7915/csi.h
  002-mt7915-csi-engine.patch          ← adds mt7915/csi.c
  003-mt7915-csi-mac-hook.patch        ← patches mt7915/mac.c RX path
  004-mt7915-csi-init.patch            ← patches mt7915/init.c
  005-mt7915-csi-vendor-cmd.patch      ← patches mt7915/main.c vendor nl80211
  006-mt7915-csi-makefile.patch        ← patches mt7915/Makefile

package/network/utils/mt76-csi-daemon/
  Makefile                             ← OpenWrt package definition
  src/
    main.c                             ← startup, config parse, thread launch
    config.h                           ← config struct shared by all modules
    netlink.c / netlink.h              ← MT7915 netlink reader → ring buffer
    queue.c / queue.h                  ← lock-free ring buffer (1024 frames)
    udp.c / udp.h                      ← raw CSI UDP forwarder
    mqtt.c / mqtt.h                    ← MQTT publisher via libmosquitto
    ubus.c / ubus.h                    ← ubus object + methods
    rest.c / rest.h                    ← HTTP REST via libubox/uhttpd
    analysis/
      presence.c / presence.h          ← amplitude variance → presence score
      position.c / position.h          ← phase diff → zone assignment
      gesture.c / gesture.h            ← temporal pattern → gesture label
      vitals.c / vitals.h              ← FFT → respiration + heart rate
  files/
    mt76-csi.conf                      ← default configuration
    mt76-csi.init                      ← OpenWrt procd init script

package/feeds/luci/applications/luci-app-csi/
  Makefile
  htdocs/luci-static/resources/view/csi/
    status.js                          ← LuCI view: presence/vitals live status
  root/usr/share/luci/menu.d/
    csi.json                           ← LuCI menu entry
  root/usr/share/rpcd/acl.d/
    csi.json                           ← rpcd ACL
```

---

## Phase 1 — Kernel CSI Patches

### Task 1: Build environment verification

**Files:**
- Read: `target/linux/mediatek/image/filogic.mk`
- Read: `package/kernel/mt76/Makefile`

**Interfaces:**
- Produces: confirmed working `make` invocation for GL-MT3000 target

- [ ] **Step 1: Install feeds**

```bash
cd /Users/osmanmarks/code/openwrt
./scripts/feeds update -a
./scripts/feeds install -a
```
Expected: feeds installed, `mosquitto` and `luci` packages now available.

- [ ] **Step 2: Configure for GL-MT3000**

```bash
make defconfig
cat > .config <<'EOF'
CONFIG_TARGET_mediatek=y
CONFIG_TARGET_mediatek_filogic=y
CONFIG_TARGET_mediatek_filogic_DEVICE_glinet_gl-mt3000=y
CONFIG_PACKAGE_kmod-mt7915e=y
CONFIG_PACKAGE_kmod-mt7915-firmware=y
CONFIG_PACKAGE_kmod-mt76-connac=y
CONFIG_PACKAGE_libnl-tiny=y
CONFIG_PACKAGE_libmosquitto=y
CONFIG_PACKAGE_libubus=y
CONFIG_PACKAGE_libubox=y
CONFIG_PACKAGE_uhttpd=y
CONFIG_PACKAGE_iw=y
EOF
make defconfig
```
Expected: `.config` updated with GL-MT3000 device and required packages.

- [ ] **Step 3: Verify mt76 downloads and builds**

```bash
make package/kernel/mt76/download V=s 2>&1 | tail -5
make package/kernel/mt76/compile V=s 2>&1 | tail -20
```
Expected: `kmod-mt7915e` appears in `bin/targets/mediatek/filogic/packages/`.

- [ ] **Step 4: Commit baseline config**

```bash
git add .config
git commit -m "build: add GL-MT3000 CSI-ready defconfig"
```

---

### Task 2: MT7915 CSI kernel patch — header

**Files:**
- Create: `package/kernel/mt76/patches/csi/001-mt7915-csi-header.patch`

**Interfaces:**
- Produces: `struct mt7915_csi_data`, `struct mt7915_csi`, `mt7915_csi_init()`, `mt7915_csi_deinit()`, `mt7915_csi_process_rx()` declarations available to later patches

- [ ] **Step 1: Create patches directory**

```bash
mkdir -p /Users/osmanmarks/code/openwrt/package/kernel/mt76/patches/csi
```

- [ ] **Step 2: Write the header patch**

Create `package/kernel/mt76/patches/csi/001-mt7915-csi-header.patch`:

```diff
--- /dev/null
+++ b/mt7915/csi.h
@@ -0,0 +1,62 @@
+/* SPDX-License-Identifier: ISC */
+#ifndef __MT7915_CSI_H
+#define __MT7915_CSI_H
+
+#define CSI_MAX_COUNT   256
+#define CSI_RING_SIZE   64
+
+struct mt7915_csi_data {
+	u8  ch_bw;
+	u16 rssi;
+	u8  snr;
+	u8  band;
+	u32 ts;
+	u16 data_num;
+	s16 data_i[CSI_MAX_COUNT];
+	s16 data_q[CSI_MAX_COUNT];
+	u8  ant_idx;
+};
+
+struct mt7915_csi {
+	struct list_head node;
+	struct mt7915_csi_data data;
+};
+
+struct mt7915_csi_ctrl {
+	bool enable;
+	u8 band;
+	spinlock_t lock;
+	struct list_head list;
+	u32 count;
+	wait_queue_head_t waitq;
+	struct net_device *ndev;
+};
+
+struct mt7915_dev;
+
+void mt7915_csi_init(struct mt7915_dev *dev, u8 band);
+void mt7915_csi_deinit(struct mt7915_dev *dev, u8 band);
+void mt7915_csi_process_rx(struct mt7915_dev *dev, struct sk_buff *skb,
+			    struct mt7915_csi_data *cdata);
+int  mt7915_csi_send_frame(struct mt7915_dev *dev,
+			    struct mt7915_csi_data *data);
+
+#endif /* __MT7915_CSI_H */
```

- [ ] **Step 3: Verify patch applies cleanly**

```bash
cd /Users/osmanmarks/code/openwrt
make package/kernel/mt76/download V=s 2>&1 | grep -E "ERROR|error" | head -5
# Patch test (dry run):
patch --dry-run -p1 -d build_dir/target-aarch64_cortex-a53_musl/linux-mediatek_filogic/mt76-*/mt7915/ \
  < package/kernel/mt76/patches/csi/001-mt7915-csi-header.patch 2>&1 | head -10
```
Expected: `dry run succeeded` or `patching file mt7915/csi.h`.

- [ ] **Step 4: Commit**

```bash
git add package/kernel/mt76/patches/csi/001-mt7915-csi-header.patch
git commit -m "kernel/mt76: add MT7915 CSI header patch"
```

---

### Task 3: MT7915 CSI kernel patch — engine

**Files:**
- Create: `package/kernel/mt76/patches/csi/002-mt7915-csi-engine.patch`

**Interfaces:**
- Consumes: `struct mt7915_csi_ctrl` from Task 2's `csi.h`
- Produces: `mt7915_csi_init()`, `mt7915_csi_deinit()`, `mt7915_csi_send_frame()` implemented; netlink socket pushes `mt7915_csi_data` to userspace

- [ ] **Step 1: Write the engine patch**

Create `package/kernel/mt76/patches/csi/002-mt7915-csi-engine.patch`:

```diff
--- /dev/null
+++ b/mt7915/csi.c
@@ -0,0 +1,120 @@
+// SPDX-License-Identifier: ISC
+#include <linux/kernel.h>
+#include <linux/module.h>
+#include <linux/netlink.h>
+#include <linux/skbuff.h>
+#include <net/genetlink.h>
+#include "mt7915.h"
+#include "csi.h"
+
+#define MT7915_CSI_MCAST_GRP_NAME "mt7915_csi"
+
+static struct genl_family mt7915_csi_genl_family;
+
+static const struct genl_multicast_group mt7915_csi_mcgrps[] = {
+	{ .name = MT7915_CSI_MCAST_GRP_NAME },
+};
+
+enum mt7915_csi_attrs {
+	MT7915_CSI_ATTR_UNSPEC,
+	MT7915_CSI_ATTR_DATA,
+	__MT7915_CSI_ATTR_MAX,
+};
+
+static struct genl_family mt7915_csi_genl_family = {
+	.name    = "mt7915_csi",
+	.version = 1,
+	.maxattr = __MT7915_CSI_ATTR_MAX - 1,
+	.mcgrps  = mt7915_csi_mcgrps,
+	.n_mcgrps = ARRAY_SIZE(mt7915_csi_mcgrps),
+	.module  = THIS_MODULE,
+};
+
+void mt7915_csi_init(struct mt7915_dev *dev, u8 band)
+{
+	struct mt7915_csi_ctrl *csi = &dev->csi[band];
+
+	spin_lock_init(&csi->lock);
+	INIT_LIST_HEAD(&csi->list);
+	init_waitqueue_head(&csi->waitq);
+	csi->band = band;
+	csi->enable = false;
+	csi->count = 0;
+}
+
+void mt7915_csi_deinit(struct mt7915_dev *dev, u8 band)
+{
+	struct mt7915_csi_ctrl *csi = &dev->csi[band];
+	struct mt7915_csi *c, *tmp;
+
+	csi->enable = false;
+	spin_lock_bh(&csi->lock);
+	list_for_each_entry_safe(c, tmp, &csi->list, node) {
+		list_del(&c->node);
+		kfree(c);
+	}
+	spin_unlock_bh(&csi->lock);
+}
+
+int mt7915_csi_send_frame(struct mt7915_dev *dev,
+			   struct mt7915_csi_data *data)
+{
+	struct sk_buff *msg;
+	void *hdr;
+	int err;
+
+	msg = genlmsg_new(sizeof(*data) + 64, GFP_ATOMIC);
+	if (!msg)
+		return -ENOMEM;
+
+	hdr = genlmsg_put(msg, 0, 0, &mt7915_csi_genl_family, 0, 0);
+	if (!hdr) {
+		err = -EMSGSIZE;
+		goto err_free;
+	}
+
+	if (nla_put(msg, MT7915_CSI_ATTR_DATA, sizeof(*data), data)) {
+		err = -EMSGSIZE;
+		goto err_cancel;
+	}
+
+	genlmsg_end(msg, hdr);
+	return genlmsg_multicast(&mt7915_csi_genl_family, msg, 0, 0, GFP_ATOMIC);
+
+err_cancel:
+	genlmsg_cancel(msg, hdr);
+err_free:
+	nlmsg_free(msg);
+	return err;
+}
+
+static int __init mt7915_csi_genl_init(void)
+{
+	return genl_register_family(&mt7915_csi_genl_family);
+}
+
+static void __exit mt7915_csi_genl_exit(void)
+{
+	genl_unregister_family(&mt7915_csi_genl_family);
+}
+
+module_init(mt7915_csi_genl_init);
+module_exit(mt7915_csi_genl_exit);
```

- [ ] **Step 2: Commit**

```bash
git add package/kernel/mt76/patches/csi/002-mt7915-csi-engine.patch
git commit -m "kernel/mt76: add MT7915 CSI engine (genl multicast)"
```

---

### Task 4: MT7915 CSI kernel patch — RX hook, init, Makefile

**Files:**
- Create: `package/kernel/mt76/patches/csi/003-mt7915-csi-mac-hook.patch`
- Create: `package/kernel/mt76/patches/csi/004-mt7915-csi-init.patch`
- Create: `package/kernel/mt76/patches/csi/005-mt7915-csi-makefile.patch`

**Interfaces:**
- Consumes: `mt7915_csi_send_frame()` from Task 3; `struct mt7915_dev` from mt7915.h
- Produces: CSI data flowing from RX path → genl multicast when `csi[band].enable = true`

- [ ] **Step 1: Write the mac.c RX hook patch**

Create `package/kernel/mt76/patches/csi/003-mt7915-csi-mac-hook.patch`:

```diff
--- a/mt7915/mac.c
+++ b/mt7915/mac.c
@@ -10,6 +10,7 @@
 #include "mac.h"
 #include "mcu.h"
 #include "../trace.h"
+#include "csi.h"

 #define to_rssi(field, rxv)	((FIELD_GET(field, rxv) - 220) / 2)

@@ -82,6 +83,27 @@ static void mt7915_mac_sta_poll(struct mt7915_dev *dev)
 	}
 }

+static void mt7915_csi_extract(struct mt7915_dev *dev,
+				struct mt76_rx_status *status,
+				struct sk_buff *skb)
+{
+	struct mt7915_csi_data cdata = {};
+	u8 band = status->ext_phy ? 1 : 0;
+
+	if (!dev->csi[band].enable)
+		return;
+
+	cdata.ch_bw   = status->bw;
+	cdata.rssi    = status->signal;
+	cdata.snr     = status->chain_signal[0];
+	cdata.band    = band;
+	cdata.ts      = jiffies_to_usecs(jiffies);
+	cdata.data_num = 0; /* filled by firmware CSI report path */
+	cdata.ant_idx  = 0;
+
+	mt7915_csi_send_frame(dev, &cdata);
+}
+
 void mt7915_mac_rx_done(struct mt7915_dev *dev, int token)
 {
 	struct mt76_dev *mdev = &dev->mt76;
@@ -120,6 +142,7 @@ void mt7915_mac_rx_done(struct mt7915_dev *dev, int token)
 		if (!skb)
 			continue;

+		mt7915_csi_extract(dev, &mdev->rx_skb[q].status, skb);
 		mt76_rx(&dev->mt76, q, skb);
 	}
 }
```

- [ ] **Step 2: Write the init.c patch**

Create `package/kernel/mt76/patches/csi/004-mt7915-csi-init.patch`:

```diff
--- a/mt7915/init.c
+++ b/mt7915/init.c
@@ -8,6 +8,7 @@
 #include "mt7915.h"
 #include "mac.h"
 #include "eeprom.h"
+#include "csi.h"

 static const struct ieee80211_iface_limit if_limits[] = {

@@ -580,6 +581,9 @@ int mt7915_register_device(struct mt7915_dev *dev)
 	if (ret)
 		return ret;

+	mt7915_csi_init(dev, 0);
+	mt7915_csi_init(dev, 1);
+
 	ret = mt76_register_device(&dev->mt76, true, mt76_rates,
 				   ARRAY_SIZE(mt76_rates));
 	if (ret)
@@ -600,6 +604,9 @@ void mt7915_unregister_device(struct mt7915_dev *dev)
 {
 	mt7915_unregister_ext_phy(dev);
 	mt76_unregister_device(&dev->mt76);
+
+	mt7915_csi_deinit(dev, 0);
+	mt7915_csi_deinit(dev, 1);
+
 	mt7915_mcu_exit(dev);
 	mt7915_tx_token_put(dev);
 	mt7915_dma_cleanup(dev);
```

- [ ] **Step 3: Write the Makefile patch**

Create `package/kernel/mt76/patches/csi/005-mt7915-csi-makefile.patch`:

```diff
--- a/mt7915/Makefile
+++ b/mt7915/Makefile
@@ -3,6 +3,7 @@ mt7915e-y := \
 	init.o \
 	dma.o \
 	eeprom.o \
+	csi.o \
 	mac.o \
 	mcu.o \
 	main.o \
```

- [ ] **Step 4: Add `csi_ctrl` array to mt7915_dev struct patch**

Create `package/kernel/mt76/patches/csi/006-mt7915-csi-dev-struct.patch`:

```diff
--- a/mt7915/mt7915.h
+++ b/mt7915/mt7915.h
@@ -10,6 +10,7 @@
 #include "../mt76_connac.h"
 #include "regs.h"
 #include "debugfs.h"
+#include "csi.h"

 #define MT7915_MAX_INTERFACES		19
 #define MT7915_MAX_WMM_SETS		4
@@ -230,6 +231,8 @@ struct mt7915_dev {
 	struct mt7915_dbg_cr_table dbg;
 #endif
 	struct mt7915_twt_entry twt_table[MT7915_MAX_STA_TWT_AGRT];
+
+	struct mt7915_csi_ctrl csi[2]; /* band 0=2.4GHz, 1=5GHz */
 };
```

- [ ] **Step 5: Build the patched module**

```bash
cd /Users/osmanmarks/code/openwrt
make package/kernel/mt76/compile V=s 2>&1 | grep -E "ERROR|error:|csi" | head -20
```
Expected: `csi.o` appears in build output, no errors.

- [ ] **Step 6: Commit**

```bash
git add package/kernel/mt76/patches/csi/
git commit -m "kernel/mt76: add MT7915 CSI RX hook, init, and dev struct patches"
```

---

## Phase 2 — mt76-csi-daemon

### Task 5: OpenWrt package scaffold

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/Makefile`
- Create: `package/network/utils/mt76-csi-daemon/src/config.h`

**Interfaces:**
- Produces: `struct csi_config` shared by all daemon modules; package builds with `make package/mt76-csi-daemon/compile`

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p /Users/osmanmarks/code/openwrt/package/network/utils/mt76-csi-daemon/src/analysis
mkdir -p /Users/osmanmarks/code/openwrt/package/network/utils/mt76-csi-daemon/files
```

- [ ] **Step 2: Write the OpenWrt package Makefile**

Create `package/network/utils/mt76-csi-daemon/Makefile`:

```makefile
include $(TOPDIR)/rules.mk

PKG_NAME:=mt76-csi-daemon
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/mt76-csi-daemon
  SECTION:=net
  CATEGORY:=Network
  TITLE:=MT7915 CSI daemon (presence, gesture, vitals)
  DEPENDS:=+libnl-tiny +libmosquitto +libubus +libubox +kmod-mt7915e
endef

define Package/mt76-csi-daemon/description
  Reads MT7915 CSI data via generic netlink and publishes
  raw frames over UDP plus processed events via MQTT, ubus, and REST.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

TARGET_CFLAGS += \
	-I$(STAGING_DIR)/usr/include/libnl-tiny \
	-I$(STAGING_DIR)/usr/include

TARGET_LDFLAGS += -lnl-tiny -lmosquitto -lubus -lubox -lblobmsg_json -pthread -lm

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) \
		$(PKG_BUILD_DIR)/main.c \
		$(PKG_BUILD_DIR)/netlink.c \
		$(PKG_BUILD_DIR)/queue.c \
		$(PKG_BUILD_DIR)/udp.c \
		$(PKG_BUILD_DIR)/mqtt.c \
		$(PKG_BUILD_DIR)/ubus.c \
		$(PKG_BUILD_DIR)/rest.c \
		$(PKG_BUILD_DIR)/analysis/presence.c \
		$(PKG_BUILD_DIR)/analysis/position.c \
		$(PKG_BUILD_DIR)/analysis/gesture.c \
		$(PKG_BUILD_DIR)/analysis/vitals.c \
		$(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/mt76-csi-daemon
endef

define Package/mt76-csi-daemon/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mt76-csi-daemon $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc
	$(INSTALL_DATA) ./files/mt76-csi.conf $(1)/etc/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/mt76-csi.init $(1)/etc/init.d/mt76-csi
endef

$(eval $(call BuildPackage,mt76-csi-daemon))
```

- [ ] **Step 3: Write the shared config header**

Create `package/network/utils/mt76-csi-daemon/src/config.h`:

```c
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
```

- [ ] **Step 4: Write the default config file**

Create `package/network/utils/mt76-csi-daemon/files/mt76-csi.conf`:

```ini
[csi]
interface = wlan0
bandwidth = 80

[udp]
enabled = true
host    = 192.168.1.100
port    = 5500

[mqtt]
enabled     = true
broker      = 127.0.0.1
port        = 1883
topic_raw   = csi/raw
topic_event = csi/event

[rest]
enabled = true
port    = 8080

[ubus]
enabled = true
object  = csi

[zones]
zone_0 = Living Room
zone_1 = Bedroom
zone_2 = Kitchen
```

- [ ] **Step 5: Write the procd init script**

Create `package/network/utils/mt76-csi-daemon/files/mt76-csi.init`:

```sh
#!/bin/sh /etc/rc.common

USE_PROCD=1
START=95
STOP=10

start_service() {
    procd_open_instance
    procd_set_param command /usr/sbin/mt76-csi-daemon -c /etc/mt76-csi.conf
    procd_set_param respawn 3600 5 5
    procd_set_param stderr 1
    procd_close_instance
}
```

- [ ] **Step 6: Verify package structure builds (stub test)**

```bash
cd /Users/osmanmarks/code/openwrt
# Add stub main.c so package compiles for scaffold test
cat > package/network/utils/mt76-csi-daemon/src/main.c <<'EOF'
#include <stdio.h>
int main(void) { printf("mt76-csi-daemon stub\n"); return 0; }
EOF
# Stub remaining files
for f in netlink queue udp mqtt ubus rest; do
  touch package/network/utils/mt76-csi-daemon/src/${f}.c
  touch package/network/utils/mt76-csi-daemon/src/${f}.h
done
for f in presence position gesture vitals; do
  touch package/network/utils/mt76-csi-daemon/src/analysis/${f}.c
  touch package/network/utils/mt76-csi-daemon/src/analysis/${f}.h
done
```

Expected: stub files created; package Makefile is syntactically valid.

- [ ] **Step 7: Commit scaffold**

```bash
git add package/network/utils/mt76-csi-daemon/
git commit -m "feat: add mt76-csi-daemon package scaffold"
```

---

### Task 6: Config parser + ring buffer + netlink reader

**Files:**
- Modify: `package/network/utils/mt76-csi-daemon/src/main.c`
- Create: `package/network/utils/mt76-csi-daemon/src/netlink.c`
- Create: `package/network/utils/mt76-csi-daemon/src/netlink.h`
- Create: `package/network/utils/mt76-csi-daemon/src/queue.c`
- Create: `package/network/utils/mt76-csi-daemon/src/queue.h`

**Interfaces:**
- Produces:
  - `queue_t *queue_create(int size)` — allocate ring buffer
  - `int queue_push(queue_t *q, csi_frame_t *f)` — non-blocking push, returns -1 if full
  - `int queue_pop(queue_t *q, csi_frame_t *f)` — blocking pop with 100ms timeout
  - `void netlink_start(csi_config_t *cfg, queue_t *q)` — start reader thread
  - `csi_frame_t` struct with fields matching `mt7915_csi_data`

- [ ] **Step 1: Write queue.h**

Create `package/network/utils/mt76-csi-daemon/src/queue.h`:

```c
#ifndef CSI_QUEUE_H
#define CSI_QUEUE_H

#include <stdint.h>
#include <pthread.h>

#define CSI_MAX_SUBCARRIERS 256

typedef struct {
    uint8_t  ch_bw;
    uint16_t rssi;
    uint8_t  snr;
    uint8_t  band;
    uint32_t ts_us;
    uint16_t data_num;
    int16_t  data_i[CSI_MAX_SUBCARRIERS];
    int16_t  data_q[CSI_MAX_SUBCARRIERS];
    uint8_t  ant_idx;
} csi_frame_t;

typedef struct {
    csi_frame_t  *buf;
    int           size;
    int           head;
    int           tail;
    int           count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} queue_t;

queue_t *queue_create(int size);
void     queue_destroy(queue_t *q);
int      queue_push(queue_t *q, const csi_frame_t *frame);
int      queue_pop(queue_t *q, csi_frame_t *frame, int timeout_ms);

#endif /* CSI_QUEUE_H */
```

- [ ] **Step 2: Write queue.c**

Create `package/network/utils/mt76-csi-daemon/src/queue.c`:

```c
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "queue.h"

queue_t *queue_create(int size) {
    queue_t *q = calloc(1, sizeof(queue_t));
    if (!q) return NULL;
    q->buf = calloc(size, sizeof(csi_frame_t));
    if (!q->buf) { free(q); return NULL; }
    q->size = size;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void queue_destroy(queue_t *q) {
    if (!q) return;
    free(q->buf);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    free(q);
}

int queue_push(queue_t *q, const csi_frame_t *frame) {
    pthread_mutex_lock(&q->lock);
    if (q->count == q->size) {
        /* Drop oldest frame to make room */
        q->head = (q->head + 1) % q->size;
        q->count--;
    }
    q->buf[q->tail] = *frame;
    q->tail = (q->tail + 1) % q->size;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int queue_pop(queue_t *q, csi_frame_t *frame, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        int r = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
        if (r == ETIMEDOUT) { pthread_mutex_unlock(&q->lock); return -1; }
    }
    *frame = q->buf[q->head];
    q->head = (q->head + 1) % q->size;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}
```

- [ ] **Step 3: Write netlink.h**

Create `package/network/utils/mt76-csi-daemon/src/netlink.h`:

```c
#ifndef CSI_NETLINK_H
#define CSI_NETLINK_H

#include "config.h"
#include "queue.h"

void netlink_start(csi_config_t *cfg, queue_t *q);
void netlink_stop(void);

#endif /* CSI_NETLINK_H */
```

- [ ] **Step 4: Write netlink.c**

Create `package/network/utils/mt76-csi-daemon/src/netlink.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include "netlink.h"
#include "queue.h"

#define GENL_FAMILY_NAME   "mt7915_csi"
#define GENL_MCAST_GRP     "mt7915_csi"
#define CSI_ATTR_DATA      1

static volatile int nl_running = 1;
static queue_t     *nl_queue   = NULL;

static int csi_nl_handler(struct nl_msg *msg, void *arg) {
    struct nlmsghdr  *nlh  = nlmsg_hdr(msg);
    struct genlmsghdr *gnlh = nlmsg_data(nlh);
    struct nlattr    *attrs[CSI_ATTR_DATA + 1];
    csi_frame_t       frame = {0};

    nla_parse(attrs, CSI_ATTR_DATA,
              genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);

    if (!attrs[CSI_ATTR_DATA])
        return NL_SKIP;

    /* Direct memcpy from kernel struct — must match mt7915_csi_data layout */
    memcpy(&frame, nla_data(attrs[CSI_ATTR_DATA]),
           nla_len(attrs[CSI_ATTR_DATA]) < (int)sizeof(frame)
               ? nla_len(attrs[CSI_ATTR_DATA]) : sizeof(frame));

    if (nl_queue)
        queue_push(nl_queue, &frame);

    return NL_OK;
}

static void *netlink_thread(void *arg) {
    struct nl_sock *sock;
    int family_id, grp_id;

    sock = nl_socket_alloc();
    nl_socket_disable_seq_check(sock);
    nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, csi_nl_handler, NULL);

    if (genl_connect(sock)) {
        fprintf(stderr, "csi-daemon: genl_connect failed\n");
        nl_socket_free(sock);
        return NULL;
    }

    family_id = genl_ctrl_resolve(sock, GENL_FAMILY_NAME);
    if (family_id < 0) {
        fprintf(stderr, "csi-daemon: genl family '%s' not found — "
                        "is kmod-mt7915-csi loaded?\n", GENL_FAMILY_NAME);
        nl_socket_free(sock);
        return NULL;
    }

    grp_id = genl_ctrl_resolve_grp(sock, GENL_FAMILY_NAME, GENL_MCAST_GRP);
    if (grp_id < 0) {
        fprintf(stderr, "csi-daemon: multicast group not found\n");
        nl_socket_free(sock);
        return NULL;
    }

    nl_socket_add_membership(sock, grp_id);

    while (nl_running)
        nl_recvmsgs_default(sock);

    nl_socket_free(sock);
    return NULL;
}

static pthread_t nl_tid;

void netlink_start(csi_config_t *cfg, queue_t *q) {
    nl_queue  = q;
    nl_running = 1;
    pthread_create(&nl_tid, NULL, netlink_thread, cfg);
}

void netlink_stop(void) {
    nl_running = 0;
    pthread_join(nl_tid, NULL);
}
```

- [ ] **Step 5: Write the config parser in main.c**

Replace `package/network/utils/mt76-csi-daemon/src/main.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "config.h"
#include "queue.h"
#include "netlink.h"
#include "udp.h"
#include "mqtt.h"
#include "ubus.h"
#include "rest.h"
#include "analysis/presence.h"
#include "analysis/position.h"
#include "analysis/gesture.h"
#include "analysis/vitals.h"

static volatile int running = 1;

static void on_signal(int sig) { (void)sig; running = 0; }

void config_defaults(csi_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->iface,           "wlan0",       CSI_IFACE_LEN - 1);
    cfg->bandwidth = 80;
    cfg->udp_enabled = true;
    strncpy(cfg->udp_host,        "127.0.0.1",   63);
    cfg->udp_port    = 5500;
    cfg->mqtt_enabled = true;
    strncpy(cfg->mqtt_broker,     "127.0.0.1",   63);
    cfg->mqtt_port   = 1883;
    strncpy(cfg->mqtt_topic_raw,  "csi/raw",     63);
    strncpy(cfg->mqtt_topic_event,"csi/event",   63);
    cfg->rest_enabled = true;
    cfg->rest_port    = 8080;
    cfg->ubus_enabled = true;
    strncpy(cfg->ubus_object,     "csi",         31);
}

int config_load(const char *path, csi_config_t *cfg) {
    config_defaults(cfg);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256], section[64] = "", key[64], val[128];
    int  zi = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "[%63[^]]]", section) == 1) continue;
        if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) != 2) continue;

        if (!strcmp(section, "csi")) {
            if (!strcmp(key, "interface")) strncpy(cfg->iface, val, CSI_IFACE_LEN-1);
            if (!strcmp(key, "bandwidth")) cfg->bandwidth = atoi(val);
        } else if (!strcmp(section, "udp")) {
            if (!strcmp(key, "enabled")) cfg->udp_enabled = !strcmp(val, "true");
            if (!strcmp(key, "host"))    strncpy(cfg->udp_host, val, 63);
            if (!strcmp(key, "port"))    cfg->udp_port = atoi(val);
        } else if (!strcmp(section, "mqtt")) {
            if (!strcmp(key, "enabled"))     cfg->mqtt_enabled = !strcmp(val, "true");
            if (!strcmp(key, "broker"))      strncpy(cfg->mqtt_broker, val, 63);
            if (!strcmp(key, "port"))        cfg->mqtt_port = atoi(val);
            if (!strcmp(key, "topic_raw"))   strncpy(cfg->mqtt_topic_raw, val, 63);
            if (!strcmp(key, "topic_event")) strncpy(cfg->mqtt_topic_event, val, 63);
        } else if (!strcmp(section, "rest")) {
            if (!strcmp(key, "enabled")) cfg->rest_enabled = !strcmp(val, "true");
            if (!strcmp(key, "port"))    cfg->rest_port = atoi(val);
        } else if (!strcmp(section, "ubus")) {
            if (!strcmp(key, "enabled")) cfg->ubus_enabled = !strcmp(val, "true");
            if (!strcmp(key, "object"))  strncpy(cfg->ubus_object, val, 31);
        } else if (!strcmp(section, "zones") && zi < CSI_MAX_ZONES) {
            if (!strncmp(key, "zone_", 5)) {
                strncpy(cfg->zone_names[zi++], val, CSI_ZONE_NAME_LEN-1);
                cfg->zone_count = zi;
            }
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *cfg_path = "/etc/mt76-csi.conf";
    csi_config_t cfg;

    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "-c")) cfg_path = argv[i+1];

    if (config_load(cfg_path, &cfg) < 0)
        fprintf(stderr, "csi-daemon: using defaults (config not found: %s)\n", cfg_path);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    queue_t *q = queue_create(1024);

    if (cfg.udp_enabled)  udp_start(&cfg, q);
    if (cfg.mqtt_enabled) mqtt_start(&cfg, q);
    if (cfg.ubus_enabled) ubus_start(&cfg);
    if (cfg.rest_enabled) rest_start(&cfg);

    presence_init();
    position_init(&cfg);
    gesture_init();
    vitals_init();

    netlink_start(&cfg, q);

    while (running) sleep(1);

    netlink_stop();
    vitals_deinit();
    gesture_deinit();
    position_deinit();
    presence_deinit();
    if (cfg.rest_enabled) rest_stop();
    if (cfg.ubus_enabled) ubus_stop();
    if (cfg.mqtt_enabled) mqtt_stop();
    if (cfg.udp_enabled)  udp_stop();
    queue_destroy(q);
    return 0;
}
```

- [ ] **Step 6: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/
git commit -m "feat(csi-daemon): config parser, ring buffer, netlink reader"
```

---

### Task 7: UDP forwarder

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/src/udp.c`
- Create: `package/network/utils/mt76-csi-daemon/src/udp.h`

**Interfaces:**
- Consumes: `queue_pop(q, &frame, 100)` from Task 6; `csi_config_t.udp_host`, `.udp_port`
- Produces: raw `csi_frame_t` bytes sent as UDP datagrams; `udp_start()`, `udp_stop()`

- [ ] **Step 1: Write udp.h**

Create `package/network/utils/mt76-csi-daemon/src/udp.h`:

```c
#ifndef CSI_UDP_H
#define CSI_UDP_H
#include "config.h"
#include "queue.h"
void udp_start(csi_config_t *cfg, queue_t *q);
void udp_stop(void);
#endif
```

- [ ] **Step 2: Write udp.c**

Create `package/network/utils/mt76-csi-daemon/src/udp.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "udp.h"

static volatile int    udp_running = 1;
static pthread_t       udp_tid;
static queue_t        *udp_q;
static csi_config_t   *udp_cfg;

static void *udp_thread(void *arg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(udp_cfg->udp_port),
    };
    inet_pton(AF_INET, udp_cfg->udp_host, &dst.sin_addr);

    csi_frame_t frame;
    while (udp_running) {
        if (queue_pop(udp_q, &frame, 100) == 0)
            sendto(fd, &frame, sizeof(frame), 0,
                   (struct sockaddr *)&dst, sizeof(dst));
    }
    close(fd);
    return NULL;
}

void udp_start(csi_config_t *cfg, queue_t *q) {
    udp_cfg = cfg;
    udp_q   = q;
    udp_running = 1;
    pthread_create(&udp_tid, NULL, udp_thread, NULL);
}

void udp_stop(void) {
    udp_running = 0;
    pthread_join(udp_tid, NULL);
}
```

- [ ] **Step 3: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/udp.c \
        package/network/utils/mt76-csi-daemon/src/udp.h
git commit -m "feat(csi-daemon): UDP raw frame forwarder"
```

---

### Task 8: MQTT publisher

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/src/mqtt.c`
- Create: `package/network/utils/mt76-csi-daemon/src/mqtt.h`

**Interfaces:**
- Consumes: `csi_config_t` mqtt fields; `queue_pop()`
- Produces: `mqtt_publish_event(topic, json_str)` callable by analysis modules; `mqtt_start()`, `mqtt_stop()`

- [ ] **Step 1: Write mqtt.h**

Create `package/network/utils/mt76-csi-daemon/src/mqtt.h`:

```c
#ifndef CSI_MQTT_H
#define CSI_MQTT_H
#include "config.h"
#include "queue.h"
void mqtt_start(csi_config_t *cfg, queue_t *q);
void mqtt_stop(void);
void mqtt_publish_event(const char *subtopic, const char *json);
#endif
```

- [ ] **Step 2: Write mqtt.c**

Create `package/network/utils/mt76-csi-daemon/src/mqtt.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <mosquitto.h>
#include "mqtt.h"

static volatile int     mqtt_running = 1;
static pthread_t        mqtt_tid;
static struct mosquitto *mosq         = NULL;
static csi_config_t    *mqtt_cfg;
static queue_t         *mqtt_q;

static void *mqtt_thread(void *arg) {
    mosquitto_lib_init();
    mosq = mosquitto_new("mt76-csi-daemon", true, NULL);
    if (!mosq) {
        fprintf(stderr, "csi-daemon: mosquitto_new failed\n");
        return NULL;
    }

    if (mosquitto_connect(mosq, mqtt_cfg->mqtt_broker,
                          mqtt_cfg->mqtt_port, 60)) {
        fprintf(stderr, "csi-daemon: MQTT connect failed to %s:%d\n",
                mqtt_cfg->mqtt_broker, mqtt_cfg->mqtt_port);
        mosquitto_destroy(mosq);
        mosq = NULL;
        return NULL;
    }

    csi_frame_t frame;
    while (mqtt_running) {
        if (queue_pop(mqtt_q, &frame, 100) == 0) {
            mosquitto_publish(mosq, NULL, mqtt_cfg->mqtt_topic_raw,
                              sizeof(frame), &frame, 0, false);
        }
        mosquitto_loop(mosq, 0, 1);
    }

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    mosq = NULL;
    return NULL;
}

void mqtt_start(csi_config_t *cfg, queue_t *q) {
    mqtt_cfg = cfg;
    mqtt_q   = q;
    mqtt_running = 1;
    pthread_create(&mqtt_tid, NULL, mqtt_thread, NULL);
}

void mqtt_stop(void) {
    mqtt_running = 0;
    pthread_join(mqtt_tid, NULL);
}

void mqtt_publish_event(const char *subtopic, const char *json) {
    if (!mosq || !json) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s",
             mqtt_cfg->mqtt_topic_event, subtopic);
    mosquitto_publish(mosq, NULL, topic,
                      (int)strlen(json), json, 0, false);
}
```

- [ ] **Step 3: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/mqtt.c \
        package/network/utils/mt76-csi-daemon/src/mqtt.h
git commit -m "feat(csi-daemon): MQTT publisher with event subtopic support"
```

---

### Task 9: ubus interface + REST server

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/src/ubus.c`
- Create: `package/network/utils/mt76-csi-daemon/src/ubus.h`
- Create: `package/network/utils/mt76-csi-daemon/src/rest.c`
- Create: `package/network/utils/mt76-csi-daemon/src/rest.h`

**Interfaces:**
- Consumes: `csi_state_t` global populated by analysis modules (Task 10-13)
- Produces:
  - ubus object `csi` with methods: `status`, `presence`, `zones`, `vitals`, `vitals_focus`
  - REST endpoints: `GET /api/csi/status`, `/presence`, `/zones`, `/vitals`; `POST /api/csi/vitals/focus`

- [ ] **Step 1: Write shared state header**

Create `package/network/utils/mt76-csi-daemon/src/state.h`:

```c
#ifndef CSI_STATE_H
#define CSI_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#define CSI_MAX_ZONES 8

typedef struct {
    pthread_mutex_t lock;

    /* Presence */
    bool     present;
    float    presence_confidence;
    int      person_count;

    /* Zones */
    int      zone_count;
    char     zone_names[CSI_MAX_ZONES][32];
    bool     zone_occupied[CSI_MAX_ZONES];
    float    zone_confidence[CSI_MAX_ZONES];

    /* Gesture */
    char     last_gesture[32];
    float    gesture_confidence;
    uint32_t gesture_ts;

    /* Vitals */
    int      vitals_focused_zone;   /* -1 = none */
    bool     vitals_feasible;
    char     vitals_infeasible_reason[32];
    float    respiration_bpm;
    float    heart_rate_bpm;
    float    vitals_confidence;

    /* Stats */
    uint64_t frame_count;
    uint32_t fps;
} csi_state_t;

extern csi_state_t g_state;

void state_init(int zone_count, char zone_names[][32]);

#endif /* CSI_STATE_H */
```

- [ ] **Step 2: Write state.c**

Create `package/network/utils/mt76-csi-daemon/src/state.c`:

```c
#include <string.h>
#include "state.h"

csi_state_t g_state;

void state_init(int zone_count, char zone_names[][32]) {
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);
    g_state.zone_count = zone_count;
    g_state.vitals_focused_zone = -1;
    for (int i = 0; i < zone_count && i < CSI_MAX_ZONES; i++)
        strncpy(g_state.zone_names[i], zone_names[i], 31);
}
```

- [ ] **Step 3: Write ubus.h**

Create `package/network/utils/mt76-csi-daemon/src/ubus.h`:

```c
#ifndef CSI_UBUS_H
#define CSI_UBUS_H
#include "config.h"
void ubus_start(csi_config_t *cfg);
void ubus_stop(void);
#endif
```

- [ ] **Step 4: Write ubus.c**

Create `package/network/utils/mt76-csi-daemon/src/ubus.c`:

```c
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <libubus.h>
#include <libubox/blobmsg_json.h>
#include "ubus.h"
#include "state.h"

static struct ubus_context *ctx   = NULL;
static struct ubus_object   obj;
static volatile int         ub_running = 1;
static pthread_t            ub_tid;
static csi_config_t        *ub_cfg;

static int ubus_status(struct ubus_context *c, struct ubus_object *o,
                       struct ubus_request_data *req, const char *method,
                       struct blob_attr *msg) {
    struct blob_buf b = {};
    blob_buf_init(&b, 0);
    pthread_mutex_lock(&g_state.lock);
    blobmsg_add_u64(&b, "frames", g_state.frame_count);
    blobmsg_add_u32(&b, "fps",    g_state.fps);
    pthread_mutex_unlock(&g_state.lock);
    ubus_send_reply(c, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int ubus_presence(struct ubus_context *c, struct ubus_object *o,
                         struct ubus_request_data *req, const char *method,
                         struct blob_attr *msg) {
    struct blob_buf b = {};
    blob_buf_init(&b, 0);
    pthread_mutex_lock(&g_state.lock);
    blobmsg_add_u8(&b,  "present",    g_state.present);
    blobmsg_add_u32(&b, "count",      g_state.person_count);
    blobmsg_add_u32(&b, "confidence", (uint32_t)(g_state.presence_confidence * 100));
    pthread_mutex_unlock(&g_state.lock);
    ubus_send_reply(c, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int ubus_vitals(struct ubus_context *c, struct ubus_object *o,
                       struct ubus_request_data *req, const char *method,
                       struct blob_attr *msg) {
    struct blob_buf b = {};
    blob_buf_init(&b, 0);
    pthread_mutex_lock(&g_state.lock);
    blobmsg_add_u8(&b,  "feasible",        g_state.vitals_feasible);
    if (g_state.vitals_feasible) {
        blobmsg_add_u32(&b, "respiration_bpm", (uint32_t)g_state.respiration_bpm);
        blobmsg_add_u32(&b, "heart_rate_bpm",  (uint32_t)g_state.heart_rate_bpm);
        blobmsg_add_u32(&b, "confidence",      (uint32_t)(g_state.vitals_confidence * 100));
    } else {
        blobmsg_add_string(&b, "reason", g_state.vitals_infeasible_reason);
    }
    pthread_mutex_unlock(&g_state.lock);
    ubus_send_reply(c, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static const struct ubus_method csi_methods[] = {
    UBUS_METHOD_NOARG("status",   ubus_status),
    UBUS_METHOD_NOARG("presence", ubus_presence),
    UBUS_METHOD_NOARG("vitals",   ubus_vitals),
};

static struct ubus_object_type csi_type =
    UBUS_OBJECT_TYPE("csi", csi_methods);

static void *ubus_thread(void *arg) {
    ctx = ubus_connect(NULL);
    if (!ctx) { fprintf(stderr, "csi-daemon: ubus connect failed\n"); return NULL; }

    memset(&obj, 0, sizeof(obj));
    obj.name = ub_cfg->ubus_object;
    obj.type = &csi_type;
    obj.methods    = csi_methods;
    obj.n_methods  = ARRAY_SIZE(csi_methods);
    ubus_add_object(ctx, &obj);

    while (ub_running)
        ubus_handle_event(ctx);

    ubus_free(ctx);
    return NULL;
}

void ubus_start(csi_config_t *cfg) {
    ub_cfg = cfg;
    ub_running = 1;
    pthread_create(&ub_tid, NULL, ubus_thread, NULL);
}

void ubus_stop(void) {
    ub_running = 0;
    pthread_join(ub_tid, NULL);
}
```

- [ ] **Step 5: Write rest.h**

Create `package/network/utils/mt76-csi-daemon/src/rest.h`:

```c
#ifndef CSI_REST_H
#define CSI_REST_H
#include "config.h"
void rest_start(csi_config_t *cfg);
void rest_stop(void);
#endif
```

- [ ] **Step 6: Write rest.c**

Create `package/network/utils/mt76-csi-daemon/src/rest.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "rest.h"
#include "state.h"

static volatile int  rest_running = 1;
static pthread_t     rest_tid;
static csi_config_t *rest_cfg;

static void send_json(int fd, int code, const char *body) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", code, strlen(body));
    send(fd, hdr, strlen(hdr), 0);
    send(fd, body, strlen(body), 0);
}

static void handle_request(int fd) {
    char req[512] = {};
    recv(fd, req, sizeof(req) - 1, 0);

    char method[8], path[128];
    sscanf(req, "%7s %127s", method, path);

    char buf[512];
    pthread_mutex_lock(&g_state.lock);

    if (!strcmp(path, "/api/csi/status")) {
        snprintf(buf, sizeof(buf),
            "{\"frames\":%llu,\"fps\":%u}",
            (unsigned long long)g_state.frame_count, g_state.fps);

    } else if (!strcmp(path, "/api/csi/presence")) {
        snprintf(buf, sizeof(buf),
            "{\"present\":%s,\"count\":%d,\"confidence\":%.2f}",
            g_state.present ? "true" : "false",
            g_state.person_count,
            g_state.presence_confidence);

    } else if (!strcmp(path, "/api/csi/vitals")) {
        if (g_state.vitals_feasible)
            snprintf(buf, sizeof(buf),
                "{\"feasible\":true,\"respiration_bpm\":%.1f,"
                "\"heart_rate_bpm\":%.1f,\"confidence\":%.2f}",
                g_state.respiration_bpm,
                g_state.heart_rate_bpm,
                g_state.vitals_confidence);
        else
            snprintf(buf, sizeof(buf),
                "{\"feasible\":false,\"reason\":\"%s\"}",
                g_state.vitals_infeasible_reason);

    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/csi/vitals/focus")) {
        /* Extract zone from body: {"zone":"B"} */
        char *zp = strstr(req, "\"zone\":");
        int zone = -1;
        if (zp) {
            char zname[4]; sscanf(zp + 7, " \"%3[^\"]\"", zname);
            zone = zname[0] - 'A';
        }
        g_state.vitals_focused_zone = zone;
        snprintf(buf, sizeof(buf), "{\"focused_zone\":%d}", zone);

    } else {
        pthread_mutex_unlock(&g_state.lock);
        send_json(fd, 404, "{\"error\":\"not found\"}");
        return;
    }

    pthread_mutex_unlock(&g_state.lock);
    send_json(fd, 200, buf);
}

static void *rest_thread(void *arg) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(rest_cfg->rest_port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 8);

    while (rest_running) {
        int fd = accept(srv, NULL, NULL);
        if (fd >= 0) {
            handle_request(fd);
            close(fd);
        }
    }
    close(srv);
    return NULL;
}

void rest_start(csi_config_t *cfg) {
    rest_cfg = cfg;
    rest_running = 1;
    pthread_create(&rest_tid, NULL, rest_thread, NULL);
}

void rest_stop(void) {
    rest_running = 0;
    pthread_join(rest_tid, NULL);
}
```

- [ ] **Step 7: Add state.c/.h to build Makefile**

Edit `package/network/utils/mt76-csi-daemon/Makefile` — find the `Build/Compile` block and add `state.c`:

```makefile
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) \
		$(PKG_BUILD_DIR)/main.c \
		$(PKG_BUILD_DIR)/netlink.c \
		$(PKG_BUILD_DIR)/queue.c \
		$(PKG_BUILD_DIR)/udp.c \
		$(PKG_BUILD_DIR)/mqtt.c \
		$(PKG_BUILD_DIR)/ubus.c \
		$(PKG_BUILD_DIR)/rest.c \
		$(PKG_BUILD_DIR)/state.c \
		$(PKG_BUILD_DIR)/analysis/presence.c \
		$(PKG_BUILD_DIR)/analysis/position.c \
		$(PKG_BUILD_DIR)/analysis/gesture.c \
		$(PKG_BUILD_DIR)/analysis/vitals.c \
		$(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/mt76-csi-daemon
endef
```

- [ ] **Step 8: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/
git commit -m "feat(csi-daemon): ubus object, REST server, shared state"
```

---

## Phase 3 — Analysis Modules

### Task 10: Presence detection

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/presence.c`
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/presence.h`

**Interfaces:**
- Consumes: `csi_frame_t` from queue; `g_state` from state.h; `mqtt_publish_event()`
- Produces: `presence_process(csi_frame_t *f)` — updates `g_state.present`, `.presence_confidence`, `.person_count`

- [ ] **Step 1: Write presence.h**

Create `package/network/utils/mt76-csi-daemon/src/analysis/presence.h`:

```c
#ifndef CSI_PRESENCE_H
#define CSI_PRESENCE_H
#include "../queue.h"
void presence_init(void);
void presence_process(const csi_frame_t *frame);
void presence_deinit(void);
#endif
```

- [ ] **Step 2: Write presence.c**

Create `package/network/utils/mt76-csi-daemon/src/analysis/presence.c`:

```c
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "presence.h"
#include "../state.h"
#include "../mqtt.h"

/* Sliding window: track amplitude variance over last N frames */
#define WINDOW_SIZE   100
#define PRESENT_THRESH  0.15f
#define ABSENT_THRESH   0.05f
#define HYSTERESIS_FRAMES 30

static float  window[WINDOW_SIZE];
static int    wpos = 0;
static int    wfull = 0;
static int    hysteresis = 0;
static bool   last_published = false;

static float amplitude(const csi_frame_t *f) {
    float sum = 0.0f;
    int n = f->data_num > 0 ? f->data_num : 64;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++)
        sum += sqrtf((float)f->data_i[i] * f->data_i[i] +
                     (float)f->data_q[i] * f->data_q[i]);
    return sum / n;
}

void presence_init(void) {
    memset(window, 0, sizeof(window));
    wpos = wfull = hysteresis = 0;
    last_published = false;
}

void presence_process(const csi_frame_t *frame) {
    window[wpos] = amplitude(frame);
    wpos = (wpos + 1) % WINDOW_SIZE;
    if (!wfull && wpos == 0) wfull = 1;

    int n = wfull ? WINDOW_SIZE : wpos;
    if (n < 10) return;

    /* Compute variance of amplitudes */
    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += window[i];
    mean /= n;
    float var = 0.0f;
    for (int i = 0; i < n; i++) {
        float d = window[i] - mean;
        var += d * d;
    }
    var /= n;
    float norm_var = var / (mean * mean + 1e-6f);

    bool present;
    float confidence;
    if (norm_var > PRESENT_THRESH) {
        present = true;
        confidence = fminf(1.0f, norm_var / (PRESENT_THRESH * 3));
    } else if (norm_var < ABSENT_THRESH) {
        present = false;
        confidence = fminf(1.0f, (ABSENT_THRESH - norm_var) / ABSENT_THRESH);
    } else {
        /* Hysteresis zone — keep last state */
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.present             = present;
    g_state.presence_confidence = confidence;
    g_state.person_count        = present ? 1 : 0;
    pthread_mutex_unlock(&g_state.lock);

    if (present != last_published) {
        char json[128];
        snprintf(json, sizeof(json),
            "{\"present\":%s,\"confidence\":%.2f,\"count\":%d}",
            present ? "true" : "false", confidence, present ? 1 : 0);
        mqtt_publish_event("presence", json);
        last_published = present;
    }
}

void presence_deinit(void) {}
```

- [ ] **Step 3: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/analysis/presence.c \
        package/network/utils/mt76-csi-daemon/src/analysis/presence.h
git commit -m "feat(csi-daemon): presence detection via amplitude variance"
```

---

### Task 11: Zone positioning

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/position.c`
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/position.h`

**Interfaces:**
- Consumes: `csi_frame_t`; `csi_config_t.zone_count`, `.zone_names`; `g_state`
- Produces: `position_process(f)` — updates `g_state.zone_occupied[]`, `.zone_confidence[]`

- [ ] **Step 1: Write position.h**

Create `package/network/utils/mt76-csi-daemon/src/analysis/position.h`:

```c
#ifndef CSI_POSITION_H
#define CSI_POSITION_H
#include "../queue.h"
#include "../config.h"
void position_init(csi_config_t *cfg);
void position_process(const csi_frame_t *frame);
void position_deinit(void);
#endif
```

- [ ] **Step 2: Write position.c**

Create `package/network/utils/mt76-csi-daemon/src/analysis/position.c`:

```c
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "position.h"
#include "../state.h"
#include "../mqtt.h"

/*
 * Zone fingerprinting approach: each zone has a learned CSI amplitude
 * profile (fingerprint). Incoming frames are matched to the nearest zone.
 * On startup we use RSSI-based heuristics until enough frames accumulate
 * for fingerprint learning.
 *
 * With 2 antennas and up to 8 zones this is a nearest-centroid classifier
 * over the mean per-subcarrier amplitude vector.
 */

#define FP_SUBCARRIERS  64
#define FP_LEARN_FRAMES 200

static int     num_zones = 0;
static float   fingerprints[CSI_MAX_ZONES][FP_SUBCARRIERS];
static float   fp_accum[CSI_MAX_ZONES][FP_SUBCARRIERS];
static int     fp_count[CSI_MAX_ZONES];
static bool    fp_ready[CSI_MAX_ZONES];
static int     active_zone = -1;

static void extract_features(const csi_frame_t *f, float *feat) {
    int n = f->data_num > FP_SUBCARRIERS ? FP_SUBCARRIERS : f->data_num;
    if (n <= 0) n = FP_SUBCARRIERS;
    for (int i = 0; i < FP_SUBCARRIERS; i++) {
        if (i < n) {
            feat[i] = sqrtf((float)f->data_i[i] * f->data_i[i] +
                            (float)f->data_q[i] * f->data_q[i]);
        } else {
            feat[i] = 0.0f;
        }
    }
}

static float cosine_sim(const float *a, const float *b, int n) {
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = sqrtf(na) * sqrtf(nb);
    return denom < 1e-6f ? 0.0f : dot / denom;
}

void position_init(csi_config_t *cfg) {
    num_zones = cfg->zone_count;
    memset(fingerprints, 0, sizeof(fingerprints));
    memset(fp_accum,     0, sizeof(fp_accum));
    memset(fp_count,     0, sizeof(fp_count));
    memset(fp_ready,     0, sizeof(fp_ready));
    active_zone = -1;
}

void position_process(const csi_frame_t *frame) {
    if (num_zones <= 0) return;

    float feat[FP_SUBCARRIERS];
    extract_features(frame, feat);

    /* Accumulate into currently active zone (set externally via focus API) */
    int learn_zone = g_state.vitals_focused_zone;
    if (learn_zone >= 0 && learn_zone < num_zones &&
        fp_count[learn_zone] < FP_LEARN_FRAMES) {
        for (int i = 0; i < FP_SUBCARRIERS; i++)
            fp_accum[learn_zone][i] += feat[i];
        fp_count[learn_zone]++;
        if (fp_count[learn_zone] == FP_LEARN_FRAMES) {
            for (int i = 0; i < FP_SUBCARRIERS; i++)
                fingerprints[learn_zone][i] =
                    fp_accum[learn_zone][i] / FP_LEARN_FRAMES;
            fp_ready[learn_zone] = true;
        }
    }

    /* Classify against ready fingerprints */
    int   best_zone = -1;
    float best_sim  = -1.0f;
    for (int z = 0; z < num_zones; z++) {
        if (!fp_ready[z]) continue;
        float sim = cosine_sim(feat, fingerprints[z], FP_SUBCARRIERS);
        if (sim > best_sim) { best_sim = sim; best_zone = z; }
    }

    if (best_zone < 0) return;

    pthread_mutex_lock(&g_state.lock);
    for (int z = 0; z < num_zones; z++) {
        g_state.zone_occupied[z]   = (z == best_zone);
        g_state.zone_confidence[z] = (z == best_zone) ? best_sim : 0.0f;
    }
    pthread_mutex_unlock(&g_state.lock);

    if (best_zone != active_zone) {
        active_zone = best_zone;
        char json[256];
        snprintf(json, sizeof(json),
            "{\"zone\":%d,\"zone_name\":\"%s\",\"confidence\":%.2f}",
            best_zone,
            g_state.zone_names[best_zone],
            best_sim);
        mqtt_publish_event("position", json);
    }
}

void position_deinit(void) {}
```

- [ ] **Step 3: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/analysis/position.c \
        package/network/utils/mt76-csi-daemon/src/analysis/position.h
git commit -m "feat(csi-daemon): zone fingerprint classifier for positioning"
```

---

### Task 12: Gesture recognition

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/gesture.c`
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/gesture.h`

**Interfaces:**
- Consumes: `csi_frame_t`; `g_state.present` (only classify when presence confirmed)
- Produces: `gesture_process(f)` — updates `g_state.last_gesture`, `.gesture_confidence`, `.gesture_ts`

- [ ] **Step 1: Write gesture.h**

Create `package/network/utils/mt76-csi-daemon/src/analysis/gesture.h`:

```c
#ifndef CSI_GESTURE_H
#define CSI_GESTURE_H
#include "../queue.h"
void gesture_init(void);
void gesture_process(const csi_frame_t *frame);
void gesture_deinit(void);
#endif
```

- [ ] **Step 2: Write gesture.c**

Create `package/network/utils/mt76-csi-daemon/src/analysis/gesture.c`:

```c
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "gesture.h"
#include "../state.h"
#include "../mqtt.h"

/*
 * Gesture detection via temporal amplitude derivative.
 * Classifies: push, pull, wave, tap using amplitude velocity and
 * direction patterns over a 1-second sliding window (50 frames at ~50fps).
 *
 * Gestures:
 *   push  — amplitude rises quickly then holds (person moves toward AP)
 *   pull  — amplitude falls quickly then holds (person moves away)
 *   wave  — rapid oscillation (3+ zero-crossings in 0.5s)
 *   tap   — brief spike (< 100ms) followed by return to baseline
 */

#define GESTURE_WINDOW 50
#define MIN_GESTURE_INTERVAL_MS 800

static float  amp_history[GESTURE_WINDOW];
static int    hpos = 0;
static int    hfull = 0;
static uint64_t last_gesture_ms = 0;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static float mean_amplitude(const csi_frame_t *f) {
    float s = 0.0f;
    int n = f->data_num > 0 ? f->data_num : 64;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++)
        s += sqrtf((float)f->data_i[i]*f->data_i[i] +
                   (float)f->data_q[i]*f->data_q[i]);
    return s / n;
}

static const char *classify(const float *h, int n) {
    if (n < GESTURE_WINDOW) return NULL;

    float mean = 0, mn = h[0], mx = h[0];
    for (int i = 0; i < n; i++) {
        mean += h[i];
        if (h[i] < mn) mn = h[i];
        if (h[i] > mx) mx = h[i];
    }
    mean /= n;
    float range = mx - mn;
    if (range < mean * 0.1f) return NULL;  /* no significant motion */

    /* Count zero-crossings of derivative around mean */
    int crossings = 0;
    for (int i = 1; i < n; i++)
        if ((h[i] - mean) * (h[i-1] - mean) < 0)
            crossings++;

    /* Trend: positive = push, negative = pull */
    float trend = h[n-1] - h[0];

    if (crossings >= 6) return "wave";

    /* Tap: spike in first/middle third, returns to baseline */
    float first_third_max = 0, last_third_mean = 0;
    for (int i = 0; i < n/3; i++)
        if (h[i] > first_third_max) first_third_max = h[i];
    for (int i = 2*n/3; i < n; i++)
        last_third_mean += h[i];
    last_third_mean /= (n - 2*n/3);
    if (first_third_max > mean * 1.4f &&
        last_third_mean < mean * 1.1f) return "tap";

    if (trend > range * 0.4f) return "push";
    if (trend < -range * 0.4f) return "pull";
    return NULL;
}

void gesture_init(void) {
    memset(amp_history, 0, sizeof(amp_history));
    hpos = hfull = 0;
    last_gesture_ms = 0;
}

void gesture_process(const csi_frame_t *frame) {
    if (!g_state.present) return;

    amp_history[hpos] = mean_amplitude(frame);
    hpos = (hpos + 1) % GESTURE_WINDOW;
    if (!hfull && hpos == 0) hfull = 1;
    if (!hfull) return;

    uint64_t now = now_ms();
    if (now - last_gesture_ms < MIN_GESTURE_INTERVAL_MS) return;

    /* Linearise ring buffer for classifier */
    float linear[GESTURE_WINDOW];
    for (int i = 0; i < GESTURE_WINDOW; i++)
        linear[i] = amp_history[(hpos + i) % GESTURE_WINDOW];

    const char *g = classify(linear, GESTURE_WINDOW);
    if (!g) return;

    last_gesture_ms = now;

    pthread_mutex_lock(&g_state.lock);
    strncpy(g_state.last_gesture, g, 31);
    g_state.gesture_confidence = 0.78f;
    g_state.gesture_ts         = (uint32_t)(now / 1000);
    pthread_mutex_unlock(&g_state.lock);

    char json[128];
    snprintf(json, sizeof(json),
        "{\"gesture\":\"%s\",\"confidence\":0.78,\"ts\":%u}",
        g, (uint32_t)(now / 1000));
    mqtt_publish_event("gesture", json);
}

void gesture_deinit(void) {}
```

- [ ] **Step 3: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/analysis/gesture.c \
        package/network/utils/mt76-csi-daemon/src/analysis/gesture.h
git commit -m "feat(csi-daemon): gesture classifier (push/pull/wave/tap)"
```

---

### Task 13: Vital signs

**Files:**
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/vitals.c`
- Create: `package/network/utils/mt76-csi-daemon/src/analysis/vitals.h`

**Interfaces:**
- Consumes: `csi_frame_t`; `g_state.vitals_focused_zone`; `g_state.present`; `g_state.zone_occupied[]`
- Produces: `vitals_process(f)` — updates `g_state.vitals_feasible`, `.respiration_bpm`, `.heart_rate_bpm`, `.vitals_infeasible_reason`

- [ ] **Step 1: Write vitals.h**

Create `package/network/utils/mt76-csi-daemon/src/analysis/vitals.h`:

```c
#ifndef CSI_VITALS_H
#define CSI_VITALS_H
#include "../queue.h"
void vitals_init(void);
void vitals_process(const csi_frame_t *frame);
void vitals_deinit(void);
#endif
```

- [ ] **Step 2: Write vitals.c**

Create `package/network/utils/mt76-csi-daemon/src/analysis/vitals.c`:

```c
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "vitals.h"
#include "../state.h"
#include "../mqtt.h"

/*
 * Vital signs via FFT on CSI amplitude time series.
 * Assumes ~20 frames/sec from netlink (rate depends on traffic density).
 *
 * Collection window: 30 seconds = 600 samples at 20fps.
 * FFT bin resolution: 20/600 = 0.033 Hz per bin.
 *
 * Respiration: 0.1 – 0.5 Hz  → bins 3..15
 * Heart rate:  0.8 – 2.0 Hz  → bins 24..60
 *
 * Feasibility gates:
 *   - vitals_focused_zone must be set (>= 0)
 *   - focused zone must be occupied (position module confirms)
 *   - presence must be true
 *   - recent amplitude variance must be low (person is still)
 */

#define VITALS_SAMPLES  600   /* 30s at 20fps */
#define SAMPLE_RATE_HZ  20.0f

static float  amp_buf[VITALS_SAMPLES];
static int    buf_pos   = 0;
static int    buf_full  = 0;
static int    frame_cnt = 0;

/* Minimal DFT for target frequency bands — avoids libfftw dependency */
static float dft_magnitude(const float *x, int n, float freq_hz) {
    float re = 0, im = 0;
    float w = 2.0f * (float)M_PI * freq_hz / SAMPLE_RATE_HZ;
    for (int k = 0; k < n; k++) {
        re += x[k] * cosf(w * k);
        im -= x[k] * sinf(w * k);
    }
    return sqrtf(re*re + im*im) / n;
}

static float peak_freq(const float *x, int n, float f_lo, float f_hi) {
    float best_mag = -1, best_f = 0;
    float step = 0.02f;  /* 0.02 Hz resolution */
    for (float f = f_lo; f <= f_hi; f += step) {
        float m = dft_magnitude(x, n, f);
        if (m > best_mag) { best_mag = m; best_f = f; }
    }
    return best_f;
}

static float amplitude(const csi_frame_t *f) {
    float s = 0.0f;
    int n = f->data_num > 0 ? f->data_num : 64;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++)
        s += sqrtf((float)f->data_i[i]*f->data_i[i] +
                   (float)f->data_q[i]*f->data_q[i]);
    return s / n;
}

static bool check_feasibility(void) {
    pthread_mutex_lock(&g_state.lock);
    int fz = g_state.vitals_focused_zone;

    if (fz < 0) {
        strncpy(g_state.vitals_infeasible_reason, "no_zone_focused", 31);
        g_state.vitals_feasible = false;
        pthread_mutex_unlock(&g_state.lock);
        return false;
    }
    if (!g_state.present) {
        strncpy(g_state.vitals_infeasible_reason, "no_presence", 31);
        g_state.vitals_feasible = false;
        pthread_mutex_unlock(&g_state.lock);
        return false;
    }
    if (fz < CSI_MAX_ZONES && !g_state.zone_occupied[fz]) {
        strncpy(g_state.vitals_infeasible_reason, "zone_empty", 31);
        g_state.vitals_feasible = false;
        pthread_mutex_unlock(&g_state.lock);
        return false;
    }
    pthread_mutex_unlock(&g_state.lock);
    return true;
}

static bool motion_check(void) {
    /* High variance = person moving → vitals unreliable */
    int n = buf_full ? VITALS_SAMPLES : buf_pos;
    if (n < 20) return false;
    float mean = 0;
    for (int i = 0; i < n; i++) mean += amp_buf[i];
    mean /= n;
    float var = 0;
    for (int i = 0; i < n; i++) { float d = amp_buf[i]-mean; var += d*d; }
    var /= n;
    float norm = var / (mean*mean + 1e-6f);
    return norm > 0.30f;  /* >30% normalised variance = motion */
}

void vitals_init(void) {
    memset(amp_buf, 0, sizeof(amp_buf));
    buf_pos = buf_full = frame_cnt = 0;
}

void vitals_process(const csi_frame_t *frame) {
    /* Subsample: keep every 1st frame (already ~20fps from netlink) */
    amp_buf[buf_pos] = amplitude(frame);
    buf_pos = (buf_pos + 1) % VITALS_SAMPLES;
    if (!buf_full && buf_pos == 0) buf_full = 1;

    frame_cnt++;
    if (frame_cnt % (VITALS_SAMPLES / 2) != 0) return;  /* analyse every 15s */

    if (!check_feasibility()) return;

    if (motion_check()) {
        pthread_mutex_lock(&g_state.lock);
        strncpy(g_state.vitals_infeasible_reason, "motion_detected", 31);
        g_state.vitals_feasible = false;
        pthread_mutex_unlock(&g_state.lock);
        return;
    }

    int n = buf_full ? VITALS_SAMPLES : buf_pos;

    /* Linearise ring buffer */
    float linear[VITALS_SAMPLES];
    int start = buf_full ? buf_pos : 0;
    for (int i = 0; i < n; i++)
        linear[i] = amp_buf[(start + i) % VITALS_SAMPLES];

    float resp_hz = peak_freq(linear, n, 0.1f, 0.5f);
    float hr_hz   = peak_freq(linear, n, 0.8f, 2.0f);
    float resp_bpm = resp_hz * 60.0f;
    float hr_bpm   = hr_hz   * 60.0f;

    /* Sanity bounds */
    if (resp_bpm < 6.0f || resp_bpm > 30.0f || hr_bpm < 40.0f || hr_bpm > 120.0f) {
        pthread_mutex_lock(&g_state.lock);
        strncpy(g_state.vitals_infeasible_reason, "low_snr", 31);
        g_state.vitals_feasible = false;
        pthread_mutex_unlock(&g_state.lock);
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.vitals_feasible    = true;
    g_state.respiration_bpm    = resp_bpm;
    g_state.heart_rate_bpm     = hr_bpm;
    g_state.vitals_confidence  = 0.75f;
    g_state.vitals_infeasible_reason[0] = '\0';
    pthread_mutex_unlock(&g_state.lock);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"feasible\":true,\"respiration_bpm\":%.1f,"
        "\"heart_rate_bpm\":%.1f,\"confidence\":0.75}",
        resp_bpm, hr_bpm);
    mqtt_publish_event("vitals", json);
}

void vitals_deinit(void) {}
```

- [ ] **Step 3: Wire analysis into main dispatch loop**

Edit `package/network/utils/mt76-csi-daemon/src/main.c` — after `netlink_start()`, add a dispatcher thread that reads the queue and calls all four analysis functions:

```c
/* Add this struct and thread before main() */
typedef struct { queue_t *q; } dispatch_args_t;

static void *dispatch_thread(void *arg) {
    queue_t *q = ((dispatch_args_t *)arg)->q;
    csi_frame_t frame;
    while (running) {
        if (queue_pop(q, &frame, 100) == 0) {
            g_state.frame_count++;
            presence_process(&frame);
            position_process(&frame);
            gesture_process(&frame);
            vitals_process(&frame);
        }
    }
    return NULL;
}

/* In main(), after netlink_start(): */
pthread_t dispatch_tid;
dispatch_args_t dargs = { .q = q };
pthread_create(&dispatch_tid, NULL, dispatch_thread, &dargs);
/* Before queue_destroy(): */
pthread_join(dispatch_tid, NULL);
```

- [ ] **Step 4: Commit**

```bash
git add package/network/utils/mt76-csi-daemon/src/analysis/vitals.c \
        package/network/utils/mt76-csi-daemon/src/analysis/vitals.h \
        package/network/utils/mt76-csi-daemon/src/main.c
git commit -m "feat(csi-daemon): FFT vital signs (respiration + heart rate) + dispatch loop"
```

---

## Phase 4 — LuCI App

### Task 14: LuCI status page

**Files:**
- Create: `package/feeds/luci/applications/luci-app-csi/Makefile`
- Create: `package/feeds/luci/applications/luci-app-csi/htdocs/luci-static/resources/view/csi/status.js`
- Create: `package/feeds/luci/applications/luci-app-csi/root/usr/share/luci/menu.d/csi.json`

**Interfaces:**
- Consumes: REST `GET /api/csi/presence`, `/api/csi/vitals`, `/api/csi/zones` from Task 9
- Produces: LuCI page at `/cgi-bin/luci/admin/network/csi` showing live presence + vitals

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p /Users/osmanmarks/code/openwrt/package/feeds/luci/applications/luci-app-csi/htdocs/luci-static/resources/view/csi
mkdir -p /Users/osmanmarks/code/openwrt/package/feeds/luci/applications/luci-app-csi/root/usr/share/luci/menu.d
mkdir -p /Users/osmanmarks/code/openwrt/package/feeds/luci/applications/luci-app-csi/root/usr/share/rpcd/acl.d
```

- [ ] **Step 2: Write LuCI Makefile**

Create `package/feeds/luci/applications/luci-app-csi/Makefile`:

```makefile
include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI CSI Monitor
LUCI_DEPENDS:=+mt76-csi-daemon
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

$(eval $(call BuildPackage,luci-app-csi))
```

- [ ] **Step 3: Write the LuCI view**

Create `package/feeds/luci/applications/luci-app-csi/htdocs/luci-static/resources/view/csi/status.js`:

```javascript
'use strict';
'require view';
'require poll';

return view.extend({
    render: function() {
        var container = document.createElement('div');
        container.innerHTML = `
            <h2>CSI Monitor</h2>
            <div style="display:flex;gap:24px;flex-wrap:wrap">
                <div id="csi-presence" style="padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px">
                    <h3>Presence</h3>
                    <p id="presence-status">Loading...</p>
                </div>
                <div id="csi-zones" style="padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px">
                    <h3>Zones</h3>
                    <p id="zones-status">Loading...</p>
                </div>
                <div id="csi-vitals" style="padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px">
                    <h3>Vitals</h3>
                    <div>
                        Zone focus: <select id="zone-select">
                            <option value="-1">None</option>
                            <option value="0">Zone A</option>
                            <option value="1">Zone B</option>
                            <option value="2">Zone C</option>
                        </select>
                        <button onclick="csiSetFocus()">Set</button>
                    </div>
                    <p id="vitals-status">Loading...</p>
                </div>
            </div>`;

        poll.add(function() {
            fetch('/api/csi/presence')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('presence-status').textContent =
                        d.present
                        ? ('Present — ' + d.count + ' person(s), confidence: ' +
                           (d.confidence * 100).toFixed(0) + '%')
                        : 'Not present';
                });

            fetch('/api/csi/vitals')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('vitals-status').textContent =
                        d.feasible
                        ? ('Resp: ' + d.respiration_bpm.toFixed(1) + ' bpm  ' +
                           'HR: ' + d.heart_rate_bpm.toFixed(1) + ' bpm  ' +
                           'Confidence: ' + (d.confidence * 100).toFixed(0) + '%')
                        : ('Unavailable: ' + d.reason);
                });
        }, 3);

        window.csiSetFocus = function() {
            var z = document.getElementById('zone-select').value;
            var zones = ['A','B','C','D','E','F','G','H'];
            fetch('/api/csi/vitals/focus', {
                method: 'POST',
                headers: {'Content-Type':'application/json'},
                body: JSON.stringify({zone: zones[parseInt(z)] || 'A'})
            });
        };

        return container;
    },
    handleSave: null,
    handleSaveApply: null,
    handleReset: null
});
```

- [ ] **Step 4: Write menu entry**

Create `package/feeds/luci/applications/luci-app-csi/root/usr/share/luci/menu.d/csi.json`:

```json
{
    "admin/network/csi": {
        "title": "CSI Monitor",
        "order": 90,
        "action": {
            "type": "view",
            "path": "csi/status"
        }
    }
}
```

- [ ] **Step 5: Add to .config and build**

```bash
echo "CONFIG_PACKAGE_luci-app-csi=y" >> /Users/osmanmarks/code/openwrt/.config
make defconfig
make package/feeds/luci/applications/luci-app-csi/compile V=s 2>&1 | tail -10
```

Expected: `luci-app-csi_*.ipk` appears in `bin/`.

- [ ] **Step 6: Commit**

```bash
git add package/feeds/luci/
git commit -m "feat: add luci-app-csi status page with presence and vitals"
```

---

## Phase 5 — Integration & Build

### Task 15: Full build + flash

**Files:**
- Modify: `.config`

- [ ] **Step 1: Final .config additions**

```bash
cat >> /Users/osmanmarks/code/openwrt/.config <<'EOF'
CONFIG_PACKAGE_kmod-mt7915e=y
CONFIG_PACKAGE_kmod-mt7915-firmware=y
CONFIG_PACKAGE_mt76-csi-daemon=y
CONFIG_PACKAGE_luci-app-csi=y
CONFIG_PACKAGE_libmosquitto=y
CONFIG_PACKAGE_libnl-tiny=y
CONFIG_PACKAGE_libubus=y
CONFIG_PACKAGE_libubox=y
CONFIG_PACKAGE_iw=y
EOF
make defconfig
```

- [ ] **Step 2: Full build**

```bash
make -j$(nproc) 2>&1 | tee /tmp/openwrt-build.log
grep -E "^ERROR|Build complete" /tmp/openwrt-build.log | tail -5
```
Expected: `Build complete.` with image at `bin/targets/mediatek/filogic/`.

- [ ] **Step 3: Flash to GL-MT3000**

```bash
# Find image
ls bin/targets/mediatek/filogic/*gl-mt3000*sysupgrade*
# SCP to router (default IP)
scp bin/targets/mediatek/filogic/*gl-mt3000*sysupgrade* root@192.168.8.1:/tmp/
# Flash (router will reboot)
ssh root@192.168.8.1 "sysupgrade -n /tmp/*sysupgrade*"
```

- [ ] **Step 4: Verify on device**

```bash
# After reboot (~90 seconds):
ssh root@192.168.8.1 "lsmod | grep mt7915"
# Expected: mt7915e module loaded

ssh root@192.168.8.1 "ls /usr/sbin/mt76-csi-daemon"
# Expected: /usr/sbin/mt76-csi-daemon

ssh root@192.168.8.1 "/etc/init.d/mt76-csi start && sleep 3 && curl http://localhost:8080/api/csi/status"
# Expected: {"frames":...,"fps":...}
```

- [ ] **Step 5: Enable CSI on wlan0**

```bash
ssh root@192.168.8.1 "iw dev wlan0 vendor recvbin 0x000ce0 0x9 0101"
# Verify frames arriving:
ssh root@192.168.8.1 "curl http://localhost:8080/api/csi/presence"
```

- [ ] **Step 6: Push final branch and open PR**

```bash
git add .config
git commit -m "build: final .config for GL-MT3000 CSI firmware"
git push personal feature/csi-mt7915
gh pr create \
  --repo ossiemarks/openwrt \
  --base main \
  --head feature/csi-mt7915 \
  --title "feat: MT7915 CSI extraction for GL-MT3000" \
  --body "Adds CSI firmware support: kernel patches, mt76-csi-daemon (UDP/MQTT/ubus/REST), zone-based vitals, gesture recognition, and LuCI status page."
```
