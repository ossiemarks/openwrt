#!/usr/bin/env python3
"""Summarise a csi_capture.py file: rate, stations, chains per measurement.

Records sharing a firmware timestamp form one measurement; chain_info
bit 15 marks the last chain of a measurement.
"""
import struct
import sys
from collections import Counter, defaultdict

HDR = struct.Struct("<I6sbBBBBBHHHH")


def load(path):
    out = []
    with open(path, "rb") as f:
        while True:
            h = f.read(10)
            if len(h) < 10:
                break
            t, n = struct.unpack("<dH", h)
            data = f.read(n)
            if len(data) < HDR.size:
                continue
            ts, ta, rssi, snr, bw, ch, mode, _pad, num, tx, rx, chain = HDR.unpack_from(data)
            out.append((t, ts, ta.hex(":"), rssi, snr, bw, num, tx, rx, chain))
    return out


def main():
    recs = load(sys.argv[1])
    if not recs:
        print("no records")
        return
    dur = recs[-1][0] - recs[0][0]
    print(f"records={len(recs)} span={dur:.1f}s rate={len(recs) / max(dur, 1e-9):.1f}/s")
    print("stations:", dict(Counter(r[2] for r in recs).most_common(6)))
    print("bw:", dict(Counter(r[5] for r in recs)), "subcarriers:", dict(Counter(r[6] for r in recs)))
    print("tx_idx:", dict(Counter(r[7] for r in recs)), "rx_idx:", dict(Counter(r[8] for r in recs)))
    last = sum(1 for r in recs if r[9] & 0x8000)
    print(f"chain_info bit15 set: {last}/{len(recs)}")
    groups = defaultdict(int)
    for r in recs:
        groups[(r[2], r[1])] += 1
    per = Counter(groups.values())
    print("chains per measurement (by ta+ts):", dict(sorted(per.items())))
    rssi = [r[3] for r in recs]
    print(f"rssi mean={sum(rssi) / len(rssi):.1f} min={min(rssi)} max={max(rssi)}")


if __name__ == "__main__":
    main()
