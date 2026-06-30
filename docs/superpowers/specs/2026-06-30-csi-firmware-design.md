# CSI Firmware Design — GL-MT3000 (MT7915)

**Date:** 2026-06-30  
**Device:** GL.iNet GL-MT3000 (MediaTek MT7981B SoC, MT7915e WiFi)  
**Branch:** feature/csi-mt7915  
**Repo:** https://github.com/ossiemarks/openwrt

---

## 1. Overview

Add Channel State Information (CSI) extraction to OpenWrt firmware for the GL-MT3000 router.
CSI data is extracted from the MT7915e WiFi chipset at the subcarrier level and used for:

- **Presence detection** — human presence without cameras
- **Indoor positioning** — zone-based location tracking
- **Gesture recognition** — motion/gesture classification
- **Vital signs monitoring** — respiration and heart rate (zone-focused, single person)

---

## 2. System Architecture

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

---

## 3. Deliverables

| Component | Location | Purpose |
|-----------|----------|---------|
| `kmod-mt7915-csi` | `package/kernel/mt76/patches/csi/` | Kernel patch enabling CSI netlink interface |
| `mt76-csi-daemon` | `package/network/utils/mt76-csi-daemon/` | Userspace daemon: netlink → UDP/MQTT/ubus/REST |
| `luci-app-csi` | `package/feeds/luci/applications/luci-app-csi/` | LuCI status page for live presence/motion/vitals |

---

## 4. Kernel Driver Patches

### 4.1 MT7915 CSI Engine

**Files added/modified in mt76 source tree:**

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

### 4.2 CSI Frame Data Structure

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

### 4.3 Activation

CSI enabled per-interface via iw vendor command:
```
iw dev wlan0 vendor recvbin 0x000ce0 0x9 <cfg>
```

Kernel pushes CSI frames to netlink group `MT76_TM_ATTR_CSI_DATA`.

### 4.4 Patch Location

```
package/kernel/mt76/patches/csi/
├── 001-mt7915-csi-header.patch
├── 002-mt7915-csi-engine.patch
├── 003-mt7915-csi-mac-hook.patch
├── 004-mt7915-csi-netlink.patch
└── 005-mt7915-csi-makefile.patch
```

---

## 5. Userspace Daemon (mt76-csi-daemon)

### 5.1 Internal Architecture

```
netlink reader thread
        │
        ▼
  ring buffer queue (lock-free, 1024 frames)
        │
   ┌────┴──────────────────────┐
   │                           │
   ▼                           ▼
output worker threads       analysis thread
   ├── UDP forwarder           ├── amplitude variance → presence score
   ├── MQTT publisher          ├── subcarrier phase diff → position/zone
   └── ubus/REST server        ├── temporal pattern → gesture classifier
                               └── FFT vital band → respiration + heart rate
                                       │
                                       ▼
                                events published to
                                all three outputs
```

### 5.2 Package Layout

```
package/network/utils/mt76-csi-daemon/
├── Makefile
├── src/
│   ├── main.c
│   ├── netlink.c         ← MT7915 netlink reader
│   ├── queue.c           ← lock-free ring buffer
│   ├── udp.c             ← UDP forwarder
│   ├── mqtt.c            ← MQTT via libmosquitto
│   ├── rest.c            ← REST via libuhttpd
│   ├── ubus.c            ← ubus object registration
│   └── analysis/
│       ├── presence.c    ← amplitude variance detector
│       ├── position.c    ← phase-based zone estimation
│       ├── gesture.c     ← temporal pattern classifier
│       └── vitals.c      ← FFT-based respiration + heart rate
├── files/
│   ├── mt76-csi.conf     ← default config
│   └── mt76-csi.init     ← OpenWrt init script
```

### 5.3 Configuration

`/etc/mt76-csi.conf`:
```ini
[csi]
interface   = wlan0
bandwidth   = 80          # MHz

[udp]
enabled     = true
host        = 192.168.1.100
port        = 5500

[mqtt]
enabled     = true
broker      = 192.168.1.1
port        = 1883
topic_raw   = csi/raw
topic_event = csi/event

[rest]
enabled     = true
port        = 8080

[ubus]
enabled     = true
object      = csi

[zones]
# Define up to 8 named zones for positioning/vitals
zone_A      = Living Room
zone_B      = Bedroom
zone_C      = Kitchen
```

---

## 6. REST API

```
GET  /api/csi/status          → daemon health + frame rate
GET  /api/csi/presence        → { present: true, confidence: 0.87, count: 2 }
GET  /api/csi/zones           → { A: "occupied", B: "occupied", C: "empty" }
GET  /api/csi/position        → { zone: "A", confidence: 0.83 }
GET  /api/csi/gesture/last    → { gesture: "wave", confidence: 0.91, ts: 1751234567 }
GET  /api/csi/vitals          → see section 7
POST /api/csi/vitals/focus    → { zone: "B" }
POST /api/csi/config          → update config without restart
```

---

## 7. Vital Signs — Zone-Focused Design

### 7.1 Zone Switching Model

```
┌─────────────────────────────────────────────────────┐
│              Zone Model (per room layout)            │
│                                                      │
│   Zone A          Zone B          Zone C             │
│  [living]        [bedroom]        [kitchen]          │
│                                                      │
│  👤 Person 1     👤 Person 2      (empty)            │
│                                                      │
└─────────────────────────────────────────────────────┘
          │
          ▼
   User selects Zone B via POST /api/csi/vitals/focus
          │
          ▼
   Vitals engine activates on that zone only
```

### 7.2 Feasibility Gates

Vitals are only measured when ALL conditions pass:

| Gate | Threshold | Reason |
|------|-----------|--------|
| Zone presence confidence | > 80% | Person must be reliably detected |
| Motion check | Stationary | Movement invalidates micro-motion signal |
| SNR | > 15 dB | Minimum signal quality |
| Proximity isolation | No other person within 1.5m | Prevents signal contamination |

If any gate fails, API returns `feasible: false` with a reason code.

### 7.3 Vital Signs Processing

```
30-second CSI amplitude window (per zone)
        │
        ▼
    FFT analysis
   ├── 0.1 – 0.5 Hz band → respiration rate (6–30 breaths/min)
   └── 0.8 – 2.0 Hz band → heart rate (48–120 bpm)
        │
        ▼
    Peak detection + confidence scoring
        │
        ▼
    Publish result
```

### 7.4 Vitals API Response

```json
// Feasible:
{
  "zone": "B",
  "feasible": true,
  "respiration_bpm": 14,
  "heart_rate_bpm": 68,
  "confidence": 0.82,
  "window_seconds": 30
}

// Not feasible:
{
  "zone": "B",
  "feasible": false,
  "reason": "motion_detected"
}
```

Reason codes: `motion_detected`, `low_snr`, `low_presence_confidence`, `contamination_risk`, `no_zone_focused`

### 7.5 MQTT Topics

```
csi/raw                → binary CSI frame (UDP-equivalent for remote tools)
csi/event/presence     → {"present": true, "confidence": 0.87, "count": 2}
csi/event/zones        → {"A": "occupied", "B": "occupied", "C": "empty"}
csi/event/gesture      → {"gesture": "wave", "confidence": 0.91}
csi/event/position     → {"zone": "A", "confidence": 0.83}
csi/vitals             → vitals payload (see 7.4)
```

---

## 8. Multi-Person Handling

| Capability | Reliability | Notes |
|------------|-------------|-------|
| Presence detection (any person) | High | >95% single, >85% 2-3 people |
| People counting | Medium | Reliable up to 3 people |
| Zone assignment | Medium | 2-3 people with 2 antennas |
| Vitals (focused zone) | High (1 person) | Degrades if second person within 1.5m |
| Simultaneous multi-person vitals | Not supported | Hardware limitation (2 antennas) |

---

## 9. Hardware Constraints

- **SoC**: MediaTek MT7981B (ARMv8, dual Cortex-A53 @ 1.3GHz)
- **WiFi**: MT7915e — 2T2R 2.4GHz + 2T2R 5GHz (WiFi 6)
- **Antennas**: 2 Rx antennas limits spatial resolution
- **CSI resolution**: Up to 256 subcarriers at 80MHz
- **Kernel**: 5.4.x (patch target)

---

## 10. Dependencies

| Library | Package | Purpose |
|---------|---------|---------|
| libmosquitto | `libmosquitto` | MQTT publishing |
| libubus | `libubus` | ubus IPC |
| libuhttpd | `libuhttpd` | REST HTTP server |
| libnl-tiny | `libnl-tiny` | Netlink socket |

---

## 11. Out of Scope

- Multi-person simultaneous vital signs
- Absolute indoor positioning (requires external anchors)
- Voice/audio analysis
- Video/camera integration
- Support for chipsets other than MT7915e
