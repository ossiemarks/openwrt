#!/usr/bin/env python3
"""Record the daemon's raw UDP CSI stream to a file.

Each datagram is the daemon's udp.c layout: a 24-byte header followed by
data_num interleaved int16 I/Q pairs. The file is a sequence of records:

    <8 bytes: float64 host timestamp> <2 bytes: uint16 payload length> <payload>

Usage:
    csi_capture.py OUTFILE [--port 5500] [--seconds N]

Stop with Ctrl-C when no --seconds is given. Prints a frame-rate summary.
"""
import argparse
import socket
import struct
import sys
import time

HDR = struct.Struct("<I6sbBBBBBHHHH")  # ts, ta, rssi, snr, bw, ch, mode, pad, n, tx, rx, chain


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outfile")
    ap.add_argument("--port", type=int, default=5500)
    ap.add_argument("--seconds", type=float, default=0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)

    n = 0
    t0 = time.time()
    last_report = t0
    stations = {}
    with open(args.outfile, "ab") as f:
        try:
            while True:
                if args.seconds and time.time() - t0 >= args.seconds:
                    break
                try:
                    data, _ = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                now = time.time()
                f.write(struct.pack("<dH", now, len(data)))
                f.write(data)
                n += 1
                if len(data) >= HDR.size:
                    ta = HDR.unpack_from(data)[1].hex(":")
                    stations[ta] = stations.get(ta, 0) + 1
                if now - last_report >= 5:
                    rate = n / (now - t0)
                    print(f"{time.strftime('%H:%M:%S')} frames={n} avg={rate:.1f}/s", file=sys.stderr)
                    last_report = now
        except KeyboardInterrupt:
            pass

    dur = time.time() - t0
    print(f"done: {n} frames in {dur:.0f}s ({n / max(dur, 1e-9):.1f}/s) -> {args.outfile}")
    for ta, c in sorted(stations.items(), key=lambda kv: -kv[1]):
        print(f"  {ta}: {c}")


if __name__ == "__main__":
    main()
