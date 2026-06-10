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
    for (const auto& t : map.tracks) {
        if (t.type == MmtsTrack::Type::Video && videoPid == 0)
            videoPid = t.packetId;
        else if (t.type == MmtsTrack::Type::Audio || t.type == MmtsTrack::Type::Subtitle)
            gatedPids.push_back(t.packetId);
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
    std::unordered_map<quint64, quint64> mpuTimeUs; // (pid,seq) -> presentation us
    qint64 doneBytes = 0;

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

        // --- single streaming pass over [scanStart, endByte)
        enum class Region { FilterIn, Copy, FilterOut };
        SigTracker sig;
        Region region = Region::FilterIn;
        bool leadEmitted = false;

        const auto emitPackets = [&](const std::vector<std::vector<uint8_t>>& pkts) {
            for (const auto& p : pkts)
                writeBytes(p.data(), p.size());
        };
        const auto flushPending = [&] {
            for (auto& [pid, st] : sig.pids()) {
                for (const auto& pp : st.pending)
                    writeBytes(pp.bytes.data(), pp.bytes.size());
                st.pending.clear();
            }
        };
        const auto enterCopy = [&] {
            flushPending();
            if (!leadEmitted) {
                emitPackets(leadPackets);
                leadEmitted = true;
            }
        };

        const auto handleMpt = [&](SigTracker::PidState& st, const std::vector<uint8_t>& mpt,
                                   uint8_t paVersion, bool filterRegion, bool isLeadBoundary) {
            for (const auto& e : MmtTlv::Splice::collectMpuTimestamps(mpt))
                mpuTimeUs[pidSeqKey(e.packetId, e.mpuSequenceNumber)] = e.presentationTimeUs;
            if (!filterRegion)
                return;
            quint32 dropMin, dropMax;
            const std::vector<MmtTlv::Splice::MpuTiming>* newMpus;
            if (isLeadBoundary) {
                dropMin = 0;
                dropMax = bS.mpuSeq > 0 ? bS.mpuSeq - 1 : 0;
                newMpus = &leadTimings;
            } else {
                dropMin = bE.mpuSeq;
                dropMax = 0xFFFFFFFFu;
                newMpus = &tailTimings;
            }
            const auto patched = MmtTlv::Splice::patchMptVideoTimestamps(mpt, videoPid, *newMpus, dropMin, dropMax);
            if (!patched || st.pending.empty()) {
                for (const auto& pp : st.pending)
                    writeBytes(pp.bytes.data(), pp.bytes.size());
            } else {
                const auto& first = st.pending.front().info;
                const auto pkt = MmtTlv::Splice::packPaMptPacket(
                    *patched, first.packetId, first.contextId, first.packetSequenceNumber,
                    first.deliveryTimestamp, paVersion);
                writeBytes(pkt.data(), pkt.size());
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
            const bool filter = region != Region::Copy;

            if (!info.mmtp) {
                writeBytes(data, info.size); // NTP / TCS / null packets pass everywhere
            } else if (info.signaling) {
                const bool buffering = filter;
                if (!buffering)
                    writeBytes(data, info.size);
                auto& st = sig.state(info.packetId);
                bool replaced = false;
                for (auto& msg : sig.feed(data, info, buffering)) {
                    std::vector<uint8_t> mpt;
                    uint8_t paVersion = 0;
                    if (extractMptFromMessage(msg, &mpt, &paVersion)) {
                        handleMpt(st, mpt, paVersion, filter, region == Region::FilterIn);
                        replaced = true;
                    }
                }
                if (buffering && !replaced && !st.inFragment) {
                    // Message(s) completed without an MPT: keep the original packets.
                    for (const auto& pp : st.pending)
                        writeBytes(pp.bytes.data(), pp.bytes.size());
                    st.pending.clear();
                }
            } else if (info.packetId == videoPid) {
                if (!filter)
                    writeBytes(data, info.size);
                // dropped in boundary regions (replaced by the synthesized MPUs)
            } else if (filter && info.mpu && isGatedPid(info.packetId)) {
                const auto it = mpuTimeUs.find(pidSeqKey(info.packetId, info.mpuSequenceNumber));
                if (it != mpuTimeUs.end() && it->second >= gateInUs && it->second < gateOutUs)
                    writeBytes(data, info.size);
            } else {
                writeBytes(data, info.size);
            }

            if (offset - lastProgressPos >= 4 * 1024 * 1024) {
                doneBytes += offset - lastProgressPos;
                lastProgressPos = offset;
                if (progress && !progress(doneBytes, totalBytes))
                    return fail(QStringLiteral("Cancelled"));
            }
        }
        if (corrupt)
            return fail(QString("Segment #%1: source does not parse as TLV near %2.").arg(segmentNo).arg(offset));

        // End of segment: flush any buffered signaling, then the tail MPU.
        flushPending();
        if (!leadEmitted)
            emitPackets(leadPackets); // copy region empty and never entered
        emitPackets(tailPackets);

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
