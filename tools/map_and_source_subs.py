#!/usr/bin/env python3
"""Parse a .mmtsmap, report its tracks, and count subtitle MPU packets in the
source byte range of each edit segment -- to tell whether the smart .mmts
export is dropping subtitles or the source simply has few there."""
import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scan_interleave as s

def parse_map(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'MMTSMAP2', 'not MMTSMAP2'
    pos = 8
    (version, flags, sourceSize, durationMs, firstPts, lastPts,
     trackCount, mptCount, rapCount, seekCount) = struct.unpack_from('<IIQqqqIIII', d, pos)
    pos += 4*2 + 8*4 + 4*4
    tracks = []
    typename = {1:'Video',2:'Audio',3:'Subtitle'}
    for _ in range(trackCount):
        typ, tflags, _r, streamIndex, packetId, compTag, sr, _r2 = struct.unpack_from('<BBHiHhII', d, pos)
        pos += 20
        tracks.append((typename.get(typ,'?'), packetId, compTag, sr))
    for _ in range(mptCount):
        _t, _o, n = struct.unpack_from('<qQI', d, pos); pos += 20 + 4*n
    raps = []
    for _ in range(rapCount):
        t, o = struct.unpack_from('<qQ', d, pos); pos += 16
        raps.append((t, o))
    return tracks, raps, firstPts

def off_before(raps, t):
    o = 0
    for tm, of in raps:
        if tm < t: o = of
        else: break
    return o

def off_at(raps, t):
    for tm, of in raps:
        if tm >= t: return of
    return raps[-1][1]

def count_pids_range(src, a, b, pids):
    c = {p:0 for p in pids}
    for off, pkt in s.packets(src, a, b):
        info = s.parse(pkt)
        if not info or not info.get('mpu'): continue
        if info['packetId'] in c: c[info['packetId']] += 1
    return c

mapp = sys.argv[1]; src = sys.argv[2]
tracks, raps, firstPts = parse_map(mapp)
print(f'map: {len(tracks)} tracks, {len(raps)} raps, firstPts={firstPts}')
for t in tracks:
    print(f'  {t[0]:9s} pid=0x{t[1]:04X} compTag={t[2]} sr={t[3]}')
subpids = [t[1] for t in tracks if t[0]=='Subtitle']
audpids = [t[1] for t in tracks if t[0]=='Audio']
print(f'subtitle pids={[hex(p) for p in subpids]}  audio pids={[hex(p) for p in audpids]}')

# Edit times are relative (0-based); rap timeMs are absolute. abs = rel + firstPts.
segs = [(39939,122906,'seg1'),(1474906,1541740,'seg2'),(1195444,1236718,'seg3')]
probe = subpids + audpids + [0xF340,0xF360,0xF370,0xF361]
for a0,b0,name in segs:
    a = a0 + firstPts; b = b0 + firstPts
    ba = off_before(raps, a); bb = off_at(raps, b)
    print(f'\n{name} src {a0}..{b0} ms (rel) -> bytes {ba}..{bb} ({(bb-ba)/1e6:.0f}MB)')
    c = count_pids_range(src, ba, bb, probe)
    for p in probe:
        print(f'    pid=0x{p:04X}: {c[p]}')
