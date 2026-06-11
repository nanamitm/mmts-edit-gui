#!/usr/bin/env python3
"""Scan an exported .mmts (TLV+MMTP) and report video-only interleave runs.

Mirrors MmtTlv::Splice::parsePacket just enough to classify each TLV packet by
MMTP packetId / payloadType. A "video-only run" is a stretch of consecutive
MPU packets on the video pid with no audio MPU packet interleaved; long runs
at a segment head starve players of audio and show up as a few seconds of
stutter.
"""
import sys, struct

def be16(b, o): return (b[o] << 8) | b[o + 1]
def be32(b, o): return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]

def parse(pkt):
    # returns dict or None; pkt starts at 0x7F
    if len(pkt) < 4 or pkt[0] != 0x7F:
        return None
    typ = pkt[1]
    info = {"tlvType": typ, "mmtp": False, "size": len(pkt)}
    if typ != 0x03:
        return info
    p = 4
    left = len(pkt) - 4
    if left < 3:
        return info
    headerType = pkt[p + 2]
    pos = 3
    if headerType == 0x60:
        if left < pos + 42:
            return info
        pos += 42
    elif headerType not in (0x61, 0x20, 0x21):
        return info
    if left < pos + 12:
        return info
    b0 = pkt[p + pos]
    pcf = (b0 >> 5) & 1
    ehf = (b0 >> 1) & 1
    info["payloadType"] = pkt[p + pos + 1] & 0x3F
    info["packetId"] = be16(pkt, p + pos + 2)
    pos += 12
    if pcf:
        pos += 4
    if ehf:
        if left < pos + 4:
            return info
        ehLen = be16(pkt, p + pos + 2)
        pos += 4 + ehLen
    info["mmtp"] = True
    if info["payloadType"] == 0x00 and left >= pos + 8:
        info["mpu"] = True
        info["mpuSeq"] = be32(pkt, p + pos + 4)
    return info

def packets(path, start=0, end=None):
    f = open(path, "rb")
    f.seek(start)
    buf = b""
    pos = start
    CHUNK = 8 << 20
    while True:
        if len(buf) < 4:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            buf += chunk
        if buf[0] != 0x7F:
            return
        size = 4 + be16(buf, 2)
        while len(buf) < size:
            chunk = f.read(CHUNK)
            if not chunk:
                return
            buf += chunk
        yield pos, buf[:size]
        pos += size
        buf = buf[size:]
        if end is not None and pos >= end:
            break

def main():
    path = sys.argv[1]
    # Pass 1: byte volume per pid to find the video pid.
    vol = {}
    n = 0
    for off, pkt in packets(path):
        info = parse(pkt)
        n += 1
        if info and info.get("mmtp") and "packetId" in info:
            vol[info["packetId"]] = vol.get(info["packetId"], 0) + info["size"]
    top = sorted(vol.items(), key=lambda kv: -kv[1])
    print(f"packets={n} total pids={len(vol)}")
    for pid, v in top[:8]:
        print(f"  pid=0x{pid:04X} bytes={v/1e6:.1f}MB")
    videoPid = top[0][0]
    print(f"video pid = 0x{videoPid:04X}")

    # Pass 2: detect video-only runs (consecutive video MPU packets with no
    # non-video MPU packet between them). Report the longest by byte span.
    runs = []
    run_start = None
    run_bytes = 0
    run_pkts = 0
    last_nonvideo_off = 0
    for off, pkt in packets(path):
        info = parse(pkt)
        if not info or not info.get("mpu"):
            continue
        if info["packetId"] == videoPid:
            if run_start is None:
                run_start = off
            run_bytes += info["size"]
            run_pkts += 1
        else:
            # non-video MPU (audio/subtitle): closes any open video run
            if run_start is not None:
                runs.append((run_start, off, run_bytes, run_pkts))
                run_start = None
                run_bytes = 0
                run_pkts = 0
    if run_start is not None:
        runs.append((run_start, None, run_bytes, run_pkts))

    runs.sort(key=lambda r: -r[2])
    print("\nTop video-only runs (no audio/subtitle MPU interleaved):")
    print(f"{'startOff':>12} {'endOff':>12} {'spanMB':>8} {'vpkts':>7}")
    for s, e, b, p in runs[:15]:
        es = f"{e:12d}" if e is not None else f"{'EOF':>12}"
        print(f"{s:12d} {es} {b/1e6:8.2f} {p:7d}")

if __name__ == "__main__":
    main()
