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
  DEPENDS:=+libnl-tiny +libmosquitto +libubus +libubox +libblobmsg-json +kmod-mt7915e
endef

define Package/mt76-csi-daemon/description
  Reads MT7915 CSI data via the MTK nl80211 vendor interface and publishes
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
		$(PKG_BUILD_DIR)/config.c \
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
