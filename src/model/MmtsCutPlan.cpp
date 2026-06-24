#include "MmtsCutPlan.h"

#include <algorithm>

QVector<MmtsCutSegment> planMmtsCutSegments(const MmtsMap& map,
                                            const QVector<EditSegment>& segments,
                                            qint64 sourceSizeBytes)
{
    QVector<MmtsPoint> raps = map.rapPoints;
    std::sort(raps.begin(), raps.end(), [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });

    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;

    QVector<MmtsCutSegment> out;
    for (const auto& seg : segments) {
        MmtsCutSegment s;
        s.inAbsMs = seg.startMs + firstPts;
        s.outAbsMs = seg.endMs + firstPts;
        if (s.outAbsMs <= s.inAbsMs)
            continue;

        // RAPs around IN: prev (demux start) and next (copy start). A RAP exactly
        // at IN must satisfy both -- otherwise an IN that already lands on a RAP
        // (e.g. the very start of the stream, or a "Snap to RAP" cut) looks like
        // it needs a lead-in re-encode when no boundary work is needed at all.
        qint64 prevByte = 0;
        qint64 rNextMs = -1;
        for (const auto& p : raps) {
            if (p.timeMs <= s.inAbsMs) prevByte = static_cast<qint64>(p.offset);
            if (p.timeMs >= s.inAbsMs) { rNextMs = p.timeMs; break; }
        }
        if (rNextMs < 0) rNextMs = s.inAbsMs;

        // A recording's very first GOP is usually incomplete (capture starts at
        // an arbitrary point, not on a keyframe boundary): if IN falls before
        // the stream's first RAP, the leading frames were simply never
        // recorded, so a lead-in re-encode is not just unavailable but
        // impossible -- there is no earlier reference frame to decode from.
        // Flag this so the exporters can snap to the first RAP without first
        // attempting (and always failing) a decode.
        const bool leadInImpossible = !raps.isEmpty() && s.inAbsMs < raps.front().timeMs;

        // OUT (frame-accurate): rLast = last RAP at/before OUT (the tail boundary,
        // an IRAP we can decode-cleanly cut at); rEnd = first RAP at/after OUT
        // (how far to demux so audio/subtitle up to OUT are captured).
        const qint64 outFaMs = s.outAbsMs;
        qint64 rLastMs = -1;
        qint64 endByte = sourceSizeBytes;
        for (const auto& p : raps) {
            if (p.timeMs <= outFaMs) rLastMs = p.timeMs;
            else { endByte = static_cast<qint64>(p.offset); break; }
        }
        if (rLastMs < 0) rLastMs = outFaMs;

        const bool reencIn = (rNextMs - s.inAbsMs) > kMmtsCutRapThresholdMs;
        const bool reencOut = (outFaMs - rLastMs) > kMmtsCutRapThresholdMs;
        s.copyStartMs = reencIn ? rNextMs : s.inAbsMs;
        s.copyEndMs = reencOut ? rLastMs : outFaMs;
        s.outAbsMs = outFaMs;
        s.leadInImpossible = leadInImpossible;

        // Segment shorter than the copy span (IN/OUT inside one GOP, or copyStart
        // past copyEnd): no stream copy -- re-encode the whole [IN, OUT) as lead-in.
        if (s.copyStartMs >= s.copyEndMs) {
            s.copyStartMs = s.outAbsMs;
            s.copyEndMs = s.outAbsMs;
        }

        s.prerollByte = std::clamp<qint64>(prevByte, 0, sourceSizeBytes);
        s.endByte = std::clamp<qint64>(endByte, 0, sourceSizeBytes);

        if (s.endByte > s.prerollByte && s.outAbsMs > s.inAbsMs)
            out.push_back(s);
    }
    return out;
}
