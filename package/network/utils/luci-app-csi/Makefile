include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI CSI Monitor
LUCI_DEPENDS:=+mt76-csi-daemon
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature to be evaluated
$(eval $(call BuildPackage,luci-app-csi))
