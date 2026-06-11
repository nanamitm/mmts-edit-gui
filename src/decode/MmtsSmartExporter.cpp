#include "MmtsSmartExporter.h"

#include "HevcReencoder.h"
#include "MmtsCutPlan.h"
#include "mmtFragment.h"
#include "mmtSplice.h"

#include <QFile>
#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

using MmtTlv::Splice::PacketInfo;

quint64 pidSeqKey(quint16 pid, quint32 seq)
{
    return (static_cast<quint64>(pid) << 32) | seq;
}

quint16 be16At(const std::vector<uint8_t>& v, size_t pos)
{
    return static_cast<quint16>((v[pos] << 8) | v[pos + 1]);
}

// ---------------------------------------------------------------------------
// Sequential TLV packet iteration over a file range with a rolling buffer.
// ---------------------------------------------------------------------------
class PacketScanner {
public:
    PacketScanner(std::ifstream& ifs, qint64 begin, qint64 end)
        : m_ifs(ifs), m_pos(begin), m_readPos(begin), m_end(end)
    {
        m_ifs.clear();
        m_ifs.seekg(begin, std::ios::beg);
        m_buf.reserve(kChunk * 2);
    }

    // Yields the next whole TLV packet (pointer valid until the next call).
    // Returns false at the end of the range; *corrupt is set when the stream
    // does not look like TLV packets anymore.
    bool next(const uint8_t** data, PacketInfo* info, qint64* offset, bool* corrupt)
    {
        *corrupt = false;
        if (!ensure(4))
            return false;
        if (m_buf[m_cur] != 0x7F) {
            *corrupt = true;
            return false;
        }
        const size_t need = 4 + ((m_buf[m_cur + 2] << 8) | m_buf[m_cur + 3]);
        if (!ensure(need))
            return false; // truncated trailing packet: treat as end of range
        if (!MmtTlv::Splice::parsePacket(m_buf.data() + m_cur, need, *info)) {
            *corrupt = true;
            return false;
        }
        *data = m_buf.data() + m_cur;
        *offset = m_pos;
        m_cur += need;
        m_pos += need;
        return true;
    }

private:
    static constexpr size_t kChunk = 4 * 1024 * 1024;

    bool ensure(size_t n)
    {
        while (m_buf.size() - m_cur < n) {
            if (m_readPos >= m_end)
                return false;
            if (m_cur > 0 && m_buf.size() > kChunk) {
                m_buf.erase(m_buf.begin(), m_buf.begin() + m_cur);
                m_cur = 0;
            }
            const qint64 want = std::min<qint64>(kChunk, m_end - m_readPos);
            const size_t old = m_buf.size();
            m_buf.resize(old + static_cast<size_t>(want));
            m_ifs.clear();
            m_ifs.read(reinterpret_cast<char*>(m_buf.data() + old), want);
            const std::streamsize got = m_ifs.gcount();
            m_buf.resize(old + static_cast<size_t>(got));
            m_readPos += got;
            if (got <= 0)
                return false;
        }
        return true;
    }

    std::ifstream& m_ifs;
    qint64 m_pos;      // file offset of m_buf[m_cur]
    qint64 m_readPos;  // next file read position
    qint64 m_end;
    std::vector<uint8_t> m_buf;
    size_t m_cur = 0;
};

// ---------------------------------------------------------------------------
// Signaling message reassembly (mirrors MmtTlvDemuxer::processSignalingMessages)
// with optional raw-packet buffering so a completed MPT-bearing message can be
// replaced by a single rewritten packet.
// ---------------------------------------------------------------------------
struct PendingPacket {
    std::vector<uint8_t> bytes;
    PacketInfo info;
};

class SigTracker {
public:
    struct PidState {
        std::vector<PendingPacket> pending;
        std::vector<uint8_t> assembling;
        bool inFragment = false;
    };

    // Returns the messages completed by this packet.
    std::vector<std::vector<uint8_t>> feed(const uint8_t* pkt, const PacketInfo& info, bool buffering)
    {
        auto& st = m_pids[info.packetId];
        if (buffering)
            st.pending.push_back(PendingPacket{ { pkt, pkt + info.size }, info });

        std::vector<std::vector<uint8_t>> completed;
        const uint8_t* payload = pkt + info.signalingPayloadOffset;
        const size_t payloadLen = info.size - info.signalingPayloadOffset;
        const auto fi = static_cast<MmtTlv::FragmentationIndicator>(info.signalingFragmentationIndicator);

        if (info.signalingAggregation) {
            if (fi != MmtTlv::FragmentationIndicator::NotFragmented)
                return completed;
            size_t pos = 0;
            while (pos < payloadLen) {
                size_t len, hdr;
                if (info.signalingLengthExtension) {
                    if (pos + 4 > payloadLen) break;
                    len = (static_cast<size_t>(payload[pos]) << 24) | (payload[pos + 1] << 16) |
                          (payload[pos + 2] << 8) | payload[pos + 3];
                    hdr = 4;
                } else {
                    if (pos + 2 > payloadLen) break;
                    len = (payload[pos] << 8) | payload[pos + 1];
                    hdr = 2;
                }
                if (pos + hdr + len > payloadLen) break;
                completed.emplace_back(payload + pos + hdr, payload + pos + hdr + len);
                pos += hdr + len;
            }
            st.inFragment = false;
            st.assembling.clear();
            return completed;
        }

        switch (fi) {
        case MmtTlv::FragmentationIndicator::NotFragmented:
            completed.emplace_back(payload, payload + payloadLen);
            st.inFragment = false;
            st.assembling.clear();
            break;
        case MmtTlv::FragmentationIndicator::FirstFragment:
            st.assembling.assign(payload, payload + payloadLen);
            st.inFragment = true;
            break;
        case MmtTlv::FragmentationIndicator::MiddleFragment:
            if (st.inFragment)
                st.assembling.insert(st.assembling.end(), payload, payload + payloadLen);
            break;
        case MmtTlv::FragmentationIndicator::LastFragment:
            if (st.inFragment) {
                st.assembling.insert(st.assembling.end(), payload, payload + payloadLen);
                completed.push_back(std::move(st.assembling));
            }
            st.assembling.clear();
            st.inFragment = false;
            break;
        }
        return completed;
    }

    PidState& state(quint16 pid) { return m_pids[pid]; }
    std::map<quint16, PidState>& pids() { return m_pids; }

private:
    std::map<quint16, PidState> m_pids;
};

// PA message carrying an MPT? (table layout mirrored from dantto4k's parser)
bool extractMptFromMessage(const std::vector<uint8_t>& msg, std::vector<uint8_t>* mpt, uint8_t* paVersion)
{
    if (msg.size() < 9)
        return false;
    if (be16At(msg, 0) != 0x0000) // PA message
        return false;
    const size_t numberOfTables = msg[7];
    const size_t tableStart = 8 + 32 * numberOfTables;
    if (tableStart + 1 > msg.size() || msg[tableStart] != 0x20) // MPT
        return false;
    if (paVersion)
        *paVersion = msg[2];
    if (mpt)
        mpt->assign(msg.begin() + tableStart, msg.end());
    return true;
}

// ---------------------------------------------------------------------------
// MPU timing for the re-encoded AUs (single synthesized MPU).
// ---------------------------------------------------------------------------
MmtTlv::Splice::MpuTiming buildMpuTiming(const std::vector<HevcReencoder::Au>& aus,
                                         qint64 firstPtsMs, quint32 mpuSeq, quint32 timescale)
{
    MmtTlv::Splice::MpuTiming t;
    t.mpuSequenceNumber = mpuSeq;
    const auto ticks = [&](qint64 ms) { return ms * static_cast<qint64>(timescale) / 1000; };

    qint64 minPtsMs = LLONG_MAX;
    for (const auto& au : aus)
        minPtsMs = std::min(minPtsMs, au.ptsMs);
    t.presentationTimeUs = static_cast<quint64>(minPtsMs + firstPtsMs) * 1000ULL;

    const auto dtsOf = [](const HevcReencoder::Au& au) { return au.dtsMs >= 0 ? au.dtsMs : au.ptsMs; };
    t.mpuDecodingTimeOffset = static_cast<quint16>(
        std::clamp<qint64>(ticks(minPtsMs) - ticks(dtsOf(aus.front())), 0, 0xFFFF));

    qint64 lastDelta = ticks(17); // ~1 frame fallback for the (unused) final slot
    for (size_t i = 0; i < aus.size(); i++) {
        const qint64 dtsPts = ticks(aus[i].ptsMs) - ticks(dtsOf(aus[i]));
        t.dtsPtsOffsets.push_back(static_cast<quint16>(std::clamp<qint64>(dtsPts, 0, 0xFFFF)));
        qint64 delta = lastDelta;
        if (i + 1 < aus.size())
            delta = ticks(dtsOf(aus[i + 1])) - ticks(dtsOf(aus[i]));
        lastDelta = delta;
        t.ptsOffsets.push_back(static_cast<quint16>(std::clamp<qint64>(delta, 0, 0xFFFF)));
    }
    return t;
}

std::vector<MmtTlv::Splice::VideoAu> toVideoAus(const std::vector<HevcReencoder::Au>& aus)
{
    std::vector<MmtTlv::Splice::VideoAu> out;
    out.reserve(aus.size());
    for (const auto& au : aus)
        out.push_back(MmtTlv::Splice::VideoAu{ au.data, au.keyframe });
    return out;
}

// ---------------------------------------------------------------------------
// Boundary prescan: find the MPU that starts the copy at/after `targetOffset`
// (its sequence number, the file offset of its first packet, the packet
// sequence number there), plus an MPT sample for the video timescale.
// ---------------------------------------------------------------------------
struct MpuBoundary {
    bool found = false;
    quint32 mpuSeq = 0;
    qint64 mpuStartOffset = 0;
    quint32 pktSeqAtStart = 0;
    quint16 videoContextId = 0;
    quint32 videoDeliveryTs = 0;
    bool anyVideo = false;
    quint32 lastVideoSeq = 0;
    quint32 lastVideoPktSeq = 0;
    std::vector<uint8_t> firstMpt;
};

bool prescanBoundary(std::ifstream& ifs, qint64 scanFrom, qint64 targetOffset, qint64 limit,
                     quint16 videoPid, MpuBoundary& out, QString* errorMessage)
{
    PacketScanner scanner(ifs, scanFrom, limit);
    SigTracker sig;
    std::unordered_map<quint32, std::pair<qint64, quint32>> firstSeen; // mpuSeq -> (offset, pktSeq)

    const uint8_t* data;
    PacketInfo info;
    qint64 offset = scanFrom;
    bool corrupt = false;
    while (scanner.next(&data, &info, &offset, &corrupt)) {
        if (!info.mmtp)
            continue;
        if (info.signaling && out.firstMpt.empty()) {
            for (auto& msg : sig.feed(data, info, false)) {
                std::vector<uint8_t> mpt;
                if (extractMptFromMessage(msg, &mpt, nullptr))
                    out.firstMpt = std::move(mpt);
            }
        }
        if (info.packetId != videoPid || !info.mpu)
            continue;
        out.anyVideo = true;
        out.lastVideoSeq = info.mpuSequenceNumber;
        out.lastVideoPktSeq = info.packetSequenceNumber;
        out.videoContextId = info.contextId;
        out.videoDeliveryTs = info.deliveryTimestamp;
        auto it = firstSeen.find(info.mpuSequenceNumber);
        if (it == firstSeen.end())
            it = firstSeen.emplace(info.mpuSequenceNumber,
                                   std::make_pair(offset, info.packetSequenceNumber)).first;
        if (offset >= targetOffset) {
            out.found = true;
            out.mpuSeq = info.mpuSequenceNumber;
            out.mpuStartOffset = it->second.first;
            out.pktSeqAtStart = it->second.second;
            return true;
        }
    }
    if (corrupt) {
        if (errorMessage) *errorMessage = QStringLiteral("Source does not parse as TLV at %1").arg(offset);
        return false;
    }
    // Target beyond the last video packet (e.g. OUT at end of file): the next
    // MPU "starts" at the end of the range.
    if (out.anyVideo) {
        out.found = true;
        out.mpuSeq = out.lastVideoSeq + 1;
        out.mpuStartOffset = limit < targetOffset ? limit : targetOffset;
        out.pktSeqAtStart = out.lastVideoPktSeq + 1;
        return true;
    }
    if (errorMessage) *errorMessage = QStringLiteral("No video packets found while scanning a cut boundary.");
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
bool MmtsSmartExporter::exportMmts(const QString& srcPath, const QString& dstPath,
                                   const MmtsMap& map, const QVector<EditSegment>& segments,
                                   qint64 sourceSizeBytes, QString* errorMessage,
                                   const std::function<bool(qint64, qint64)>& progress,
                                   QStringList* warnings,
                                   bool requireFrameAccurate)
{
    const QVector<MmtsCutSegment> plan = planMmtsCutSegments(map, segments, sourceSizeBytes);
    if (plan.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("Nothing to export (no segments map to RAP ranges).");
        return false;
    }
    const qint64 firstPts = map.firstVideoPtsMs > 0 ? map.firstVideoPtsMs : 0;

    quint16 videoPid = 0;
    std::vector<quint16> gatedPids; // audio + subtitle: time-gated to [IN, OUT)
    std::vector<quint16> audioPids;
    for (const auto& t : map.tracks) {
        if (t.type == MmtsTrack::Type::Video && videoPid == 0)
            videoPid = t.packetId;
        else if (t.type == MmtsTrack::Type::Audio || t.type == MmtsTrack::Type::Subtitle)
            gatedPids.push_back(t.packetId);
        if (t.type == MmtsTrack::Type::Audio)
            audioPids.push_back(t.packetId);
    }
    if (videoPid == 0) {
        if (errorMessage) *errorMessage = QStringLiteral("The map has no video track.");
        return false;
    }
    const auto isGatedPid = [&](quint16 pid) {
        return std::find(gatedPids.begin(), gatedPids.end(), pid) != gatedPids.end();
    };

    QVector<MmtsPoint> raps = map.rapPoints;
    std::sort(raps.begin(), raps.end(), [](const MmtsPoint& a, const MmtsPoint& b) { return a.timeMs < b.timeMs; });
    const auto rapOffsetBefore = [&](qint64 t) {
        qint64 off = 0;
        for (const auto& p : raps) {
            if (p.timeMs < t) off = static_cast<qint64>(p.offset);
            else break;
        }
        return off;
    };
    const auto rapOffsetAt = [&](qint64 t) {
        for (const auto& p : raps) {
            if (p.timeMs >= t)
                return static_cast<qint64>(p.offset);
        }
        return sourceSizeBytes;
    };

    std::ifstream ifs(std::filesystem::path(srcPath.toStdWString()), std::ios::binary);
    if (!ifs.is_open()) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot open source: %1").arg(srcPath);
        return false;
    }
    std::ofstream ofs(std::filesystem::path(dstPath.toStdWString()), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot create output: %1").arg(dstPath);
        return false;
    }
    const auto fail = [&](const QString& msg) {
        if (errorMessage) *errorMessage = msg;
        ofs.close();
        QFile::remove(dstPath);
        return false;
    };

    struct SegRange {
        MmtsCutSegment cut;
        qint64 scanStart = 0;
    };
    std::vector<SegRange> ranges;
    qint64 totalBytes = 0;
    for (const auto& cut : plan) {
        SegRange r;
        r.cut = cut;
        r.scanStart = std::min(cut.prerollByte, rapOffsetBefore(cut.copyStartMs));
        ranges.push_back(r);
        totalBytes += cut.endByte - r.scanStart;
    }

    HevcReencoder reenc;
    std::unordered_map<quint64, quint64> mpuTimeUs; // (pid,seq) -> presentation us (source timeline)
    qint64 doneBytes = 0;
    qint64 desiredStartUs = -1; // output-timeline cursor; first segment keeps its own base

    // Sequence renumbering state: the output stream gets continuous MMTP
    // packet sequence numbers, CIP sequence numbers and MPU sequence numbers
    // per pid across the segment seams (header fields only; payloads are
    // untouched). First-seen values are adopted from the source so the first
    // segment stays close to the original numbering.
    std::map<quint16, quint32> nextPktSeq;  // per packetId
    std::map<quint16, quint8> nextCipSeq;   // per contextId (4-bit)
    std::map<quint16, qint64> lastOutMpu;   // per packetId, output numbering

    std::vector<uint8_t> outBuf;
    outBuf.reserve(8 * 1024 * 1024);
    const auto flushOut = [&] {
        if (!outBuf.empty()) {
            ofs.write(reinterpret_cast<const char*>(outBuf.data()),
                      static_cast<std::streamsize>(outBuf.size()));
            outBuf.clear();
        }
    };
    const auto writeBytes = [&](const uint8_t* d, size_t n) {
        outBuf.insert(outBuf.end(), d, d + n);
        if (outBuf.size() >= 4 * 1024 * 1024)
            flushOut();
    };

    int segmentNo = 0;
    for (const auto& r : ranges) {
        ++segmentNo;
        const MmtsCutSegment& seg = r.cut;
        const bool copyEmpty = seg.copyStartMs >= seg.copyEndMs;

        // --- boundary re-encodes (same policy as the TS exporter)
        std::vector<HevcReencoder::Au> leadIn, tail;
        qint64 effInMs = seg.inAbsMs;
        qint64 effOutMs = seg.outAbsMs;
        if (seg.copyStartMs - seg.inAbsMs > kMmtsCutRapThresholdMs) {
            QString rerr;
            if (!reenc.reencode(srcPath, map, seg.inAbsMs - firstPts, seg.copyStartMs - firstPts, leadIn, &rerr)) {
                const QString message = QString("Segment #%1 lead-in re-encode failed; IN cannot be exported frame-accurately. %2")
                    .arg(segmentNo).arg(rerr);
                if (requireFrameAccurate)
                    return fail(message);
                if (warnings)
                    warnings->push_back(message + QStringLiteral(" IN was moved to the next RAP boundary."));
                leadIn.clear();
                effInMs = seg.copyStartMs;
            }
        }
        if (seg.outAbsMs - seg.copyEndMs > kMmtsCutRapThresholdMs) {
            QString rerr;
            if (!reenc.reencode(srcPath, map, seg.copyEndMs - firstPts, seg.outAbsMs - firstPts, tail, &rerr)) {
                const QString message = QString("Segment #%1 tail re-encode failed; OUT cannot be exported frame-accurately. %2")
                    .arg(segmentNo).arg(rerr);
                if (requireFrameAccurate)
                    return fail(message);
                if (warnings)
                    warnings->push_back(message + QStringLiteral(" OUT was moved to the previous RAP boundary."));
                tail.clear();
                effOutMs = seg.copyEndMs;
            }
        }

        // --- locate the MPU boundaries of the copy region
        const qint64 scanLimit = std::min<qint64>(seg.endByte + 32 * 1024 * 1024, sourceSizeBytes);
        QString perr;
        MpuBoundary bS;
        if (!prescanBoundary(ifs, r.scanStart, rapOffsetAt(seg.copyStartMs), scanLimit, videoPid, bS, &perr))
            return fail(QString("Segment #%1: %2").arg(segmentNo).arg(perr));
        MpuBoundary bE;
        if (copyEmpty) {
            bE = bS;
        } else if (!prescanBoundary(ifs, rapOffsetBefore(seg.copyEndMs), rapOffsetAt(seg.copyEndMs), scanLimit,
                                    videoPid, bE, &perr)) {
            return fail(QString("Segment #%1: %2").arg(segmentNo).arg(perr));
        }
        if (bE.mpuStartOffset < bS.mpuStartOffset)
            bE = bS;

        quint32 timescale = 90000;
        if (!bS.firstMpt.empty()) {
            const quint32 ts = MmtTlv::Splice::videoTimescale(bS.firstMpt, videoPid);
            if (ts > 0)
                timescale = ts;
        }

        // --- synthesized boundary MPUs
        std::vector<MmtTlv::Splice::MpuTiming> leadTimings, tailTimings;
        std::vector<std::vector<uint8_t>> leadPackets, tailPackets;
        if (!leadIn.empty()) {
            leadTimings.push_back(buildMpuTiming(leadIn, firstPts, bS.mpuSeq - 1, timescale));
            MmtTlv::Splice::VideoMpuParams p;
            p.packetId = videoPid;
            p.contextId = bS.videoContextId;
            p.deliveryTimestamp = bS.videoDeliveryTs;
            p.mpuSequenceNumber = bS.mpuSeq - 1;
            const auto aus = toVideoAus(leadIn);
            p.firstPacketSequenceNumber =
                bS.pktSeqAtStart - static_cast<quint32>(MmtTlv::Splice::countVideoMpuPackets(aus));
            leadPackets = MmtTlv::Splice::packVideoMpu(aus, p);
        }
        if (!tail.empty()) {
            tailTimings.push_back(buildMpuTiming(tail, firstPts, bE.mpuSeq, timescale));
            MmtTlv::Splice::VideoMpuParams p;
            p.packetId = videoPid;
            p.contextId = bE.videoContextId ? bE.videoContextId : bS.videoContextId;
            p.deliveryTimestamp = bE.videoDeliveryTs;
            p.mpuSequenceNumber = bE.mpuSeq;
            p.firstPacketSequenceNumber = bE.pktSeqAtStart;
            tailPackets = MmtTlv::Splice::packVideoMpu(toVideoAus(tail), p);
        }

        const quint64 gateInUs = static_cast<quint64>(effInMs) * 1000ULL;
        const quint64 gateOutUs = static_cast<quint64>(effOutMs) * 1000ULL;

        // Timestamp rebase: segments after the first are shifted back so the
        // output timeline is continuous (media packets stay byte-identical;
        // only MPT timestamp entries and NTP packets carry absolute time).
        if (desiredStartUs < 0)
            desiredStartUs = effInMs * 1000;
        const qint64 shiftUs = desiredStartUs - effInMs * 1000;

        // Per-pid MPU renumbering offsets (output = source + offset) for this
        // segment. Video is known up front from the boundary prescan; audio /
        // subtitle anchor on their lowest in-gate MPT entry (announced before
        // the MPU itself arrives), refinable downward until the pid's first
        // emission. Other MPU pids (data carousels) keep their source numbers:
        // their sequence numbers identify carousel items and cycle by design.
        std::map<quint16, qint64> mpuOff;
        std::map<quint16, bool> mpuOffFrozen;
        {
            const quint64 firstSrcVideo = leadIn.empty() ? bS.mpuSeq : bS.mpuSeq - 1;
            const auto it = lastOutMpu.find(videoPid);
            mpuOff[videoPid] = (it != lastOutMpu.end())
                ? (it->second + 1) - static_cast<qint64>(firstSrcVideo)
                : 0;
        }

        // Single emission path for MMTP packets: appends the packet and
        // rewrites the CIP sequence, packet sequence and MPU sequence fields
        // so the output numbering is continuous per pid across the seams.
        const auto emitMmtp = [&](const uint8_t* d, const PacketInfo& pi) {
            const size_t base = outBuf.size();
            outBuf.insert(outBuf.end(), d, d + pi.size);
            if (pi.size >= 7) { // CIP sequence: low 4 bits of the byte at +5
                quint8& cs = nextCipSeq.try_emplace(pi.contextId,
                                                    static_cast<quint8>(outBuf[base + 5] & 0x0F)).first->second;
                outBuf[base + 5] = static_cast<uint8_t>((outBuf[base + 5] & 0xF0) | (cs & 0x0F));
                cs = (cs + 1) & 0x0F;
            }
            if (pi.mmtpSequenceOffset) {
                quint32& s = nextPktSeq.try_emplace(pi.packetId, pi.packetSequenceNumber).first->second;
                uint8_t* p = outBuf.data() + base + pi.mmtpSequenceOffset;
                p[0] = static_cast<uint8_t>(s >> 24);
                p[1] = static_cast<uint8_t>(s >> 16);
                p[2] = static_cast<uint8_t>(s >> 8);
                p[3] = static_cast<uint8_t>(s);
                ++s;
            }
            if (pi.mpu && pi.mpuSequenceOffset) {
                auto off = mpuOff.find(pi.packetId);
                if (off == mpuOff.end()) {
                    // Gated pid emitted before any in-gate entry (should not
                    // happen: gating needs a known entry) falls back to the
                    // arrival anchor; other pids keep their source numbers.
                    qint64 v = 0;
                    if (isGatedPid(pi.packetId)) {
                        const auto last = lastOutMpu.find(pi.packetId);
                        if (last != lastOutMpu.end())
                            v = (last->second + 1) - static_cast<qint64>(pi.mpuSequenceNumber);
                    }
                    off = mpuOff.emplace(pi.packetId, v).first;
                }
                mpuOffFrozen[pi.packetId] = true;
                const quint32 outSeq = static_cast<quint32>(pi.mpuSequenceNumber + off->second);
                uint8_t* p = outBuf.data() + base + pi.mpuSequenceOffset;
                p[0] = static_cast<uint8_t>(outSeq >> 24);
                p[1] = static_cast<uint8_t>(outSeq >> 16);
                p[2] = static_cast<uint8_t>(outSeq >> 8);
                p[3] = static_cast<uint8_t>(outSeq);
                lastOutMpu[pi.packetId] = outSeq;
            }
            if (outBuf.size() >= 4 * 1024 * 1024)
                flushOut();
        };
        const auto emitOwnPacket = [&](const std::vector<uint8_t>& pkt) {
            PacketInfo pi;
            if (MmtTlv::Splice::parsePacket(pkt.data(), pkt.size(), pi) && pi.mmtp)
                emitMmtp(pkt.data(), pi);
            else
                writeBytes(pkt.data(), pkt.size());
        };

        // At each segment head the copy region's video leads its in-gate audio
        // by ~0.5-1 s in byte order (the broadcast mux interleave) and the
        // pre-IN audio is gated out, so the head would otherwise be one long
        // video-only run: players starve for audio, the clock stalls and the
        // framerate collapses to a few fps for a couple of seconds. Instead we
        // buffer a window at the head and emit it with the byte-delayed audio
        // pulled forward and spread evenly across the head video. Video keeps
        // its byte position (no late frames); the in-gate audio is delivered
        // early (harmlessly buffered by the player); the head gets a natural
        // A/V interleave. After the window normal streaming resumes -- the
        // steady-state "video leads audio by ~D" interleave is exactly what the
        // mux carries everywhere else and what players expect.
        // The window spans the whole segment head: the FilterIn region (video
        // dropped, only in-gate audio kept) plus the synthesized lead-in GOP
        // plus the first ~MB of the copy region. Without this the seam carries
        // a ~2 MB video-only-absent stretch (FilterIn audio + preroll) before
        // the copy video starts, so mid-playback the video freezes for ~0.6 s
        // until it arrives. We distribute the lead-in video across the FilterIn
        // byte span and spread the byte-delayed in-gate audio across the whole
        // window: video data is then present continuously across the seam and
        // audio is delivered slightly early (harmlessly buffered).
        enum class HeadKind { Video, Audio, Other };
        struct HeadPkt {
            std::vector<uint8_t> bytes;
            PacketInfo info;
            qint64 offset = 0;
            HeadKind kind = HeadKind::Other;
            bool raw = false;  // write verbatim (e.g. already-shifted NTP)
            bool lead = false; // synthesized lead-in video (no real byte offset)
        };
        std::vector<HeadPkt> headBuf;
        bool headMerge = !audioPids.empty();
        bool markLead = false;            // tag video emitted as lead-in
        qint64 headStart = r.scanStart;   // window opens at the FilterIn start
        qint64 headCopyStart = bS.mpuStartOffset; // copy region (lead-in spans up to here)
        qint64 headOffset = headStart;    // file offset attributed to the next emitVideo()
        constexpr qint64 kHeadWindowBytes = 12 * 1024 * 1024;
        const auto pushHead = [&](HeadKind kind, const uint8_t* d, const PacketInfo& pi,
                                  qint64 off, bool raw = false, bool lead = false) {
            headBuf.push_back(HeadPkt{ std::vector<uint8_t>(d, d + pi.size), pi, off, kind, raw, lead });
        };
        // Emit the buffered head window. Lead-in video spreads across the
        // FilterIn byte span [headStart, headCopyStart); copy video and "other"
        // packets keep their natural byte order; the gated audio/subtitle
        // packets (which sit ~D bytes later in the source) spread evenly across
        // the whole window. Renumbering happens here, in final file order.
        const auto flushHeadMerge = [&] {
            if (!headMerge)
                return;
            headMerge = false;
            if (headBuf.empty())
                return;
            qint64 na = 0, nLead = 0, maxOff = headStart;
            for (const auto& h : headBuf) {
                if (h.kind == HeadKind::Audio)
                    ++na;
                else if (h.lead)
                    ++nLead;
                else
                    maxOff = std::max(maxOff, h.offset);
            }
            const double span = std::max<double>(1.0, static_cast<double>(maxOff - headStart));
            const double leadFrac = std::clamp(static_cast<double>(headCopyStart - headStart) / span, 0.0, 1.0);
            qint64 ia = 0, il = 0;
            std::vector<std::pair<double, size_t>> order;
            order.reserve(headBuf.size());
            for (size_t i = 0; i < headBuf.size(); i++) {
                double key;
                if (headBuf[i].kind == HeadKind::Audio)
                    key = na > 0 ? (static_cast<double>(ia++) + 0.5) / static_cast<double>(na) : 0.0;
                else if (headBuf[i].lead)
                    key = nLead > 0 ? ((static_cast<double>(il++) + 0.5) / static_cast<double>(nLead)) * leadFrac : 0.0;
                else
                    key = static_cast<double>(headBuf[i].offset - headStart) / span;
                order.emplace_back(key, i);
            }
            std::stable_sort(order.begin(), order.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [k, i] : order) {
                HeadPkt& h = headBuf[i];
                if (h.raw)
                    writeBytes(h.bytes.data(), h.bytes.size());
                else
                    emitMmtp(h.bytes.data(), h.info);
            }
            headBuf.clear();
        };
        const auto emitVideo = [&](const uint8_t* d, const PacketInfo& pi) {
            if (headMerge)
                pushHead(HeadKind::Video, d, pi, headOffset, /*raw=*/false, /*lead=*/markLead);
            else
                emitMmtp(d, pi);
        };

        // --- single streaming pass over [scanStart, endByte)
        enum class Region { FilterIn, Copy, FilterOut };
        SigTracker sig;
        Region region = Region::FilterIn;
        bool leadEmitted = false;

        const auto emitPackets = [&](const std::vector<std::vector<uint8_t>>& pkts) {
            for (const auto& p : pkts) { // synthesized lead/tail video MPUs
                PacketInfo pi;
                if (MmtTlv::Splice::parsePacket(p.data(), p.size(), pi) && pi.mmtp)
                    emitVideo(p.data(), pi);
                else
                    emitOwnPacket(p);
            }
        };
        const auto flushPending = [&] {
            for (auto& [pid, st] : sig.pids()) {
                for (const auto& pp : st.pending)
                    emitMmtp(pp.bytes.data(), pp.info);
                st.pending.clear();
            }
        };
        const auto enterCopy = [&] {
            flushPending();
            if (!leadEmitted) {
                // Lead-in video: tagged so the merge spreads it across the
                // FilterIn byte span (filling the seam's video-absent stretch).
                markLead = true;
                headOffset = headCopyStart;
                emitPackets(leadPackets);
                markLead = false;
                leadEmitted = true;
            }
        };

        const auto handleMpt = [&](SigTracker::PidState& st, const std::vector<uint8_t>& mpt,
                                   uint8_t paVersion, Region rgn) {
            for (const auto& e : MmtTlv::Splice::collectMpuTimestamps(mpt)) {
                mpuTimeUs[pidSeqKey(e.packetId, e.mpuSequenceNumber)] = e.presentationTimeUs;
                // Anchor the pid's renumbering offset on the lowest announced
                // in-gate MPU (the entry precedes the MPU itself, so sparse
                // pids like subtitles get their offset in time). MPTs can
                // re-announce older MPUs, so refine downward until the pid's
                // first emission freezes the offset.
                if (e.packetId != videoPid && isGatedPid(e.packetId) &&
                    !mpuOffFrozen[e.packetId] && lastOutMpu.count(e.packetId) &&
                    e.presentationTimeUs >= gateInUs && e.presentationTimeUs < gateOutUs) {
                    const qint64 cand =
                        (lastOutMpu[e.packetId] + 1) - static_cast<qint64>(e.mpuSequenceNumber);
                    const auto it = mpuOff.find(e.packetId);
                    if (it == mpuOff.end())
                        mpuOff[e.packetId] = cand;
                    else if (cand > it->second)
                        it->second = cand;
                }
            }
            quint32 dropMin = 1, dropMax = 0; // empty range (copy region: shift only)
            const std::vector<MmtTlv::Splice::MpuTiming> kNone;
            const std::vector<MmtTlv::Splice::MpuTiming>* newMpus = &kNone;
            if (rgn == Region::FilterIn) {
                dropMin = 0;
                dropMax = bS.mpuSeq > 0 ? bS.mpuSeq - 1 : 0;
                newMpus = &leadTimings;
            } else if (rgn == Region::FilterOut) {
                dropMin = bE.mpuSeq;
                dropMax = 0xFFFFFFFFu;
                newMpus = &tailTimings;
            }
            const auto patched = MmtTlv::Splice::patchMptVideoTimestamps(mpt, videoPid, *newMpus,
                                                                         dropMin, dropMax, shiftUs, &mpuOff);
            if (!patched || st.pending.empty()) {
                for (const auto& pp : st.pending)
                    emitMmtp(pp.bytes.data(), pp.info);
            } else {
                const auto& first = st.pending.front().info;
                const auto pkt = MmtTlv::Splice::packPaMptPacket(
                    *patched, first.packetId, first.contextId, first.packetSequenceNumber,
                    first.deliveryTimestamp, paVersion);
                emitOwnPacket(pkt);
            }
            st.pending.clear();
        };

        PacketScanner scanner(ifs, r.scanStart, seg.endByte);
        const uint8_t* data;
        PacketInfo info;
        qint64 offset = r.scanStart;
        bool corrupt = false;
        qint64 lastProgressPos = r.scanStart;
        while (scanner.next(&data, &info, &offset, &corrupt)) {
            // Region transitions (offsets are packet starts).
            if (region == Region::FilterIn && offset >= bS.mpuStartOffset) {
                enterCopy();
                region = (offset >= bE.mpuStartOffset) ? Region::FilterOut : Region::Copy;
            } else if (region == Region::Copy && offset >= bE.mpuStartOffset) {
                region = Region::FilterOut;
            }
            // The head window spans FilterIn + the copy head; flush it once we
            // reach FilterOut (small copy regions land here straight away).
            if (headMerge && region == Region::FilterOut)
                flushHeadMerge();
            const bool filter = region != Region::Copy;

            if (!info.mmtp) {
                // NTP / TCS / null packets pass everywhere; NTP carries absolute
                // time (PCR source), so rebased segments shift it too.
                std::vector<uint8_t> raw(data, data + info.size);
                if (shiftUs != 0 && info.tlvType == 0x02)
                    MmtTlv::Splice::shiftTlvNtpTimestamps(raw, shiftUs);
                if (headMerge)
                    pushHead(HeadKind::Other, raw.data(), info, offset, /*raw=*/true);
                else
                    writeBytes(raw.data(), raw.size());
            } else if (info.signaling) {
                // Signaling is buffered per message in every region: MPT-bearing
                // PA messages are replaced by a rewritten packet (synthesized MPU
                // entries at the boundaries, timestamp shift everywhere), other
                // messages are passed through unchanged on completion.
                auto& st = sig.state(info.packetId);
                bool replaced = false;
                for (auto& msg : sig.feed(data, info, true)) {
                    std::vector<uint8_t> mpt;
                    uint8_t paVersion = 0;
                    if (extractMptFromMessage(msg, &mpt, &paVersion)) {
                        handleMpt(st, mpt, paVersion, region);
                        replaced = true;
                    }
                }
                if (!replaced && !st.inFragment) {
                    // Message(s) completed without an MPT: keep the original packets.
                    for (const auto& pp : st.pending)
                        emitMmtp(pp.bytes.data(), pp.info);
                    st.pending.clear();
                }
            } else if (info.packetId == videoPid) {
                if (!filter) {
                    headOffset = offset;
                    emitVideo(data, info);
                }
                // dropped in boundary regions (replaced by the synthesized MPUs)
            } else if (info.mpu && isGatedPid(info.packetId)) {
                // Gate audio/subtitle MPUs to [IN, OUT): in the boundary
                // windows an unknown presentation time drops the MPU; in the
                // copy region only a confirmed out-of-gate time does (audio
                // interleaved around the copy start can predate IN, and
                // passing it would also break the renumbering anchor).
                const auto it = mpuTimeUs.find(pidSeqKey(info.packetId, info.mpuSequenceNumber));
                const bool known = it != mpuTimeUs.end();
                const bool inGate = known && it->second >= gateInUs && it->second < gateOutUs;
                const bool pass = filter ? inGate : (!known || inGate);
                if (qEnvironmentVariableIsSet("MMTS_EXPORT_DEBUG") && info.packetId == gatedPids.front()) {
                    fprintf(stderr, "audio pid=%04X seq=%u region=%d known=%d t=%lld gateIn=%lld pass=%d off=%llx\n",
                            info.packetId, info.mpuSequenceNumber, (int)region, known,
                            known ? (long long)(it->second / 1000) : -1,
                            (long long)(gateInUs / 1000), pass ? 1 : 0, (unsigned long long)offset);
                }
                if (pass) {
                    if (headMerge)
                        pushHead(HeadKind::Audio, data, info, offset);
                    else
                        emitMmtp(data, info);
                }
            } else {
                if (headMerge)
                    pushHead(HeadKind::Other, data, info, offset);
                else
                    emitMmtp(data, info);
            }

            if (headMerge && offset - headStart >= kHeadWindowBytes)
                flushHeadMerge(); // window full: rest of the copy flows naturally

            if (offset - lastProgressPos >= 4 * 1024 * 1024) {
                doneBytes += offset - lastProgressPos;
                lastProgressPos = offset;
                if (progress && !progress(doneBytes, totalBytes))
                    return fail(QStringLiteral("Cancelled"));
            }
        }
        if (corrupt)
            return fail(QString("Segment #%1: source does not parse as TLV near %2.").arg(segmentNo).arg(offset));

        // End of segment: flush the head window (short copy regions never hit
        // the byte budget or a FilterOut transition), then any buffered
        // signaling and the tail MPU.
        flushHeadMerge();
        flushPending();
        if (!leadEmitted)
            emitPackets(leadPackets); // copy region empty and never entered
        emitPackets(tailPackets);
        desiredStartUs += (effOutMs - effInMs) * 1000;

        doneBytes += seg.endByte - lastProgressPos;
        if (progress && !progress(doneBytes, totalBytes))
            return fail(QStringLiteral("Cancelled"));
    }

    flushOut();
    ofs.flush();
    return true;
}

// ---------------------------------------------------------------------------
void MmtsSmartExportWorker::configure(const QString& srcPath, const QString& dstPath, const MmtsMap& map,
                                      const QVector<EditSegment>& segments, qint64 sourceSizeBytes,
                                      bool requireFrameAccurate)
{
    m_srcPath = srcPath;
    m_dstPath = dstPath;
    m_map = map;
    m_segments = segments;
    m_sourceSize = sourceSizeBytes;
    m_requireFrameAccurate = requireFrameAccurate;
    m_cancel.store(false);
}

void MmtsSmartExportWorker::run()
{
    QString error;
    QStringList warnings;
    const bool ok = MmtsSmartExporter::exportMmts(
        m_srcPath, m_dstPath, m_map, m_segments, m_sourceSize, &error,
        [this](qint64 done, qint64 total) {
            emit progress(done, total);
            return !m_cancel.load();
        },
        &warnings,
        m_requireFrameAccurate);
    emit finished(ok, error, warnings);
}
