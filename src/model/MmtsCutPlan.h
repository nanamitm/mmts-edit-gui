#pragma once

#include "MmtsEdit.h"
#include "MmtsMap.h"

#include <QVector>

// Shared cut planning for the exporters: maps each EDL segment to RAP
// boundaries and source byte ranges. IN/OUT stay frame-accurate; copyStart /
// copyEnd are the RAP-aligned bounds of the stream-copyable middle, and the
// byte range [prerollByte, endByte) covers everything a demux of the segment
// needs (RAP at/before IN .. RAP at/after OUT).
struct MmtsCutSegment {
    qint64 inAbsMs = 0;      // frame-accurate IN, absolute presentation ms
    qint64 outAbsMs = 0;     // frame-accurate OUT (exclusive), absolute ms
    qint64 prerollByte = 0;  // demux start: RAP at/before IN
    qint64 endByte = 0;      // demux end: RAP at/after OUT
    qint64 copyStartMs = 0;  // stream-copy video range (abs ms); empty if >= copyEndMs
    qint64 copyEndMs = 0;
    // True if IN falls before the stream's first RAP: those leading frames were
    // never recorded (capture starts mid-GOP), so a lead-in re-encode isn't
    // just unavailable, it's impossible -- there is nothing earlier to decode.
    bool leadInImpossible = false;
};

// Within ~1 frame of a RAP -> treat as RAP-aligned (no boundary re-encode).
constexpr qint64 kMmtsCutRapThresholdMs = 20;

QVector<MmtsCutSegment> planMmtsCutSegments(const MmtsMap& map,
                                            const QVector<EditSegment>& segments,
                                            qint64 sourceSizeBytes);
