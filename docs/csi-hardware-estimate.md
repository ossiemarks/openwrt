# CSI Network Hardware Estimate — 1km² Coverage

**Area:** 1,000m × 1,000m (1 km²)
**Date:** 2026-06-30

---

## Sensing Range Per Node

| Capability | Outdoor Range | Indoor Range |
|------------|--------------|-------------|
| Presence detection | ~50m radius | ~10m radius |
| Zone positioning | ~25m radius | ~8m radius |
| Gesture recognition | ~5m radius | ~3m radius |
| Vital signs | ~3m radius | ~2m radius |

---

## Scenario A — Outdoor, Presence Only (coarse)

**50m radius → 70m grid spacing → 15×15 = 225 nodes**

| Hardware | Count | Role |
|----------|-------|------|
| GL-MT3000 routers | **225** | CSI receivers (sensing nodes) |
| ESP32 | **225** | Beacon transmitters (one per cell, generates traffic for CSI) |
| Raspberry Pi 4 | **15** | Edge aggregators (1 per 15 routers), runs MQTT + analysis |
| PoE switches (24-port) | **10** | Power + backhaul |

**Power:** 225 × 15W = ~3.4kW continuous

---

## Scenario B — Outdoor, Zone Positioning (20m zones)

**25m radius → 35m grid → 29×29 = 841 nodes**

| Hardware | Count | Role |
|----------|-------|------|
| GL-MT3000 routers | **841** | CSI receivers |
| ESP32 | **841** | Per-cell beacon transmitters |
| Raspberry Pi 4 | **56** | Edge aggregators (1 per 15 routers) |
| PoE switches (48-port) | **18** | Power + backhaul |

**Power:** 841 × 15W = ~12.6kW continuous

---

## Scenario C — Indoor, Zone Positioning (building/campus scale)

**8m radius → 12m grid → 84×84 = 7,056 nodes**

| Hardware | Count | Role |
|----------|-------|------|
| MT7915 APs (cheaper than MT3000) | **7,056** | CSI receivers |
| ESP32 | **7,056** | Reference transmitters |
| Raspberry Pi 4 | **470** | Aggregators (1 per 15 routers) |
| PoE switches (48-port) | **150** | Backhaul |

> Full smart-building deployment — airport / warehouse scale.

**Power:** 7,056 × 15W = ~105kW continuous

---

## Gesture + Vitals at Scale

Gesture/vitals across the full 1km² is **not practical** with this hardware:

| Factor | Value |
|--------|-------|
| Required radius | 5m |
| Required grid spacing | 8m |
| Nodes required | 125×125 = **15,625** |
| Estimated power | ~230kW |

**Recommended approach:** Deploy vitals/gesture only in specific zones of interest within the 1km² area (e.g., 10–20 rooms or monitored stations).

---

## Practical Mixed-Tier Deployment

Most real-world 1km² deployments combine coverage tiers:

```
Outer perimeter:   Presence only     → sparse grid   (~200 routers)
Mid zones:         Zone positioning   → medium grid   (~600 routers)
Key locations:     Gesture + vitals   → dense cluster (~20 rooms with ESP32)
```

### Mixed Deployment Totals

| Hardware | Count | Notes |
|----------|-------|-------|
| GL-MT3000 routers | **~800** | Mix of presence + positioning nodes |
| ESP32 | **~820** | 800 cell beacons + 20 dense vitals clusters |
| Raspberry Pi 4 | **~55** | 1 per 15 routers |
| PoE switches (48-port) | **~20** | Power + Ethernet backhaul |

**Estimated power:** ~12–15kW continuous (outdoor mixed deployment)

---

## Key Variable: Indoor vs Outdoor

Indoor wall attenuation multiplies node count by approximately **10×** compared to outdoor.

| Environment | Nodes per km² (presence) | Nodes per km² (positioning) |
|-------------|--------------------------|------------------------------|
| Open outdoor | 225 | 841 |
| Dense urban outdoor | 400–600 | 1,500–2,000 |
| Indoor (open plan) | 2,500 | 7,000 |
| Indoor (partitioned) | 4,000–6,000 | 10,000+ |

---

## Network Backhaul Architecture

```
ESP32 beacons (transmit WiFi frames)
        │ (WiFi)
        ▼
GL-MT3000 routers (CSI capture)
        │ (Ethernet / PoE)
        ▼
PoE Switch (48-port)
        │ (Gigabit uplink)
        ▼
Raspberry Pi 4 (edge aggregator)
  ├── MQTT broker
  ├── Presence / zone analysis
  └── Forward to central server
        │ (Ethernet / 4G backhaul)
        ▼
Central server / cloud
  ├── Dashboard
  ├── Historical data
  └── Alerts
```

---

## Notes

- ESP32 beacon interval should be set to 10–50ms for dense CSI frame collection
- GL-MT3000 runs `mt76-csi-daemon` publishing to local Raspberry Pi MQTT broker
- Each Raspberry Pi handles up to 15 routers (~750 CSI frames/sec aggregate)
- For outdoor deployment, weatherproof enclosures required for routers and Pis
- PoE budget: 15.4W per port (802.3af) sufficient for GL-MT3000
