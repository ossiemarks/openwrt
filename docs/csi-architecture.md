# CSI (Channel State Information) Architecture — GL-MT3000

## System Architecture

```
┌─────────────────────────────────────────────────────┐
│                 GL-MT3000 (MT7981B)                  │
│                                                      │
│  ┌─────────────────────────────────────────────┐    │
│  │  KERNEL SPACE                               │    │
│  │                                             │    │
│  │  mt7915e driver (patched)                   │    │
│  │    └── CSI engine → netlink socket          │    │
│  └──────────────────┬──────────────────────────┘    │
│                     │ netlink (AF_NETLINK)           │
│  ┌──────────────────▼──────────────────────────┐    │
│  │  USERSPACE: mt76-csi-daemon                 │    │
│  │                                             │    │
│  │  ┌─────────┐  ┌───────┐  ┌───────────────┐ │    │
│  │  │ UDP     │  │ MQTT  │  │ ubus + REST   │ │    │
│  │  │ forward │  │ pub   │  │ API server    │ │    │
│  │  └────┬────┘  └───┬───┘  └──────┬────────┘ │    │
│  └───────┼───────────┼─────────────┼───────────┘    │
└──────────┼───────────┼─────────────┼────────────────┘
           │           │             │
           ▼           ▼             ▼
      Remote host   MQTT broker   LuCI/ubus/
      (PicoScenes,  (Home Asst.)  HTTP :8080
       MATLAB)
```

## Kernel Driver Patch Layer

**Files modified in mt76 source tree:**

```
mt76/
├── mt7915/
│   ├── csi.c          ← new: CSI capture engine
│   ├── csi.h          ← new: CSI data structures
│   ├── init.c         ← patched: register CSI on interface up
│   ├── mac.c          ← patched: hook RX path to extract CSI frames
│   ├── main.c         ← patched: add vendor nl80211 commands
│   └── Makefile       ← patched: add csi.o
```

## CSI Frame Data Structure

```c
struct mt7915_csi {
    u8  ch_bw;          // channel bandwidth (20/40/80 MHz)
    u16 nr;             // number of Rx antennas
    u16 nc;             // number of Tx antennas
    u16 rssi;           // RSSI
    u8  snr;            // SNR
    u32 ts;             // timestamp (µs)
    s16 data_i[256];    // I (real) per subcarrier
    s16 data_q[256];    // Q (imaginary) per subcarrier
};
```

## Activation Flow

1. Patch applied at build time via `patches/csi/001-mt7915-csi-engine.patch`
2. CSI enabled per-interface via `iw` vendor command:
   ```
   iw dev wlan0 vendor recvbin 0x000ce0 0x9 <cfg>
   ```
3. Kernel pushes CSI frames to netlink group `MT76_TM_ATTR_CSI_DATA`

## Deliverables

| Component | Location | Purpose |
|-----------|----------|---------|
| `kmod-mt7915-csi` | `package/kernel/mt76/patches/csi/` | Kernel patch enabling CSI netlink interface |
| `mt76-csi-daemon` | `package/network/utils/mt76-csi-daemon/` | Userspace daemon: netlink → UDP/MQTT/ubus/REST |
| `luci-app-csi` | `package/feeds/luci/applications/luci-app-csi/` | LuCI status page for live presence/motion state |

## Use Cases Enabled

- **Presence detection** — detect humans in a room without cameras
- **Indoor positioning** — track device locations using WiFi signal subcarriers
- **Gesture recognition** — hand/motion detection via subcarrier amplitude changes

## CSI Data Throughput

- Up to **256 subcarrier complex values** per frame (80 MHz bandwidth)
- Per-packet capture at full line rate
- I/Q data suitable for amplitude + phase analysis
