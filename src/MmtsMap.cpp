#include "MmtsMap.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <cstring>

namespace {

template <typename T>
bool readPod(const QByteArray& data, qsizetype& pos, T& value)
{
    if (pos < 0 || pos + static_cast<qsizetype>(sizeof(T)) > data.size())
        return false;
    std::memcpy(&value, data.constData() + pos, sizeof(T));
    pos += static_cast<qsizetype>(sizeof(T));
    return true;
}

MmtsTrack::Type trackTypeFromCode(quint8 code)
{
    switch (code) {
    case 1: return MmtsTrack::Type::Video;
    case 2: return MmtsTrack::Type::Audio;
    case 3: return MmtsTrack::Type::Subtitle;
    default: return MmtsTrack::Type::Unknown;
    }
}

MmtsTrack::Type trackTypeFromText(const QString& text)
{
    if (text == "video")
        return MmtsTrack::Type::Video;
    if (text == "audio")
        return MmtsTrack::Type::Audio;
    if (text == "subtitle")
        return MmtsTrack::Type::Subtitle;
    return MmtsTrack::Type::Unknown;
}

QHash<QString, QString> parseKeyValues(const QString& line)
{
    QHash<QString, QString> values;
    for (const QString& token : line.split(' ', Qt::SkipEmptyParts)) {
        const int eq = token.indexOf('=');
        if (eq > 0)
            values.insert(token.left(eq), token.mid(eq + 1));
    }
    return values;
}

int findTrackIndex(const QVector<MmtsTrack>& tracks, const MmtsTrack& needle)
{
    for (int i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        if (t.type == needle.type &&
            t.streamIndex == needle.streamIndex &&
            t.packetId == needle.packetId &&
            t.componentTag == needle.componentTag) {
            return i;
        }
    }
    return -1;
}

void appendUniqueTrack(QVector<MmtsTrack>& tracks, const MmtsTrack& track)
{
    if (findTrackIndex(tracks, track) < 0)
        tracks.push_back(track);
}

bool parseTrackSpec(const QString& spec, MmtsTrack::Type type, MmtsTrack& track)
{
    const QStringList parts = spec.split(':');
    const bool audio = type == MmtsTrack::Type::Audio;
    if ((audio && parts.size() != 5) || (!audio && parts.size() != 3))
        return false;

    bool ok = false;
    track.type = type;
    track.streamIndex = parts[0].toInt(&ok, 0);
    if (!ok)
        return false;
    track.packetId = static_cast<uint16_t>(parts[1].toUInt(&ok, 0));
    if (!ok)
        return false;
    track.componentTag = parts[2].toInt(&ok, 0);
    if (!ok)
        return false;
    if (audio) {
        track.samplingRate = parts[3].toUInt(&ok, 0);
        if (!ok)
            return false;
        track.latm = parts[4].toInt(&ok, 0) != 0;
        if (!ok)
            return false;
    }
    return true;
}

} // namespace

bool MmtsMapReader::load(const QString& path, MmtsMap& out, QString* errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = file.errorString();
        return false;
    }

    const QByteArray data = file.readAll();
    MmtsMap map;
    map.path = path;

    bool ok = false;
    if (data.startsWith("MMTSMAP2"))
        ok = loadBinary(data, map, errorMessage);
    else
        ok = loadText(data, map, errorMessage);
    if (!ok)
        return false;

    out = map;
    return true;
}

QString MmtsMapReader::trackTypeName(MmtsTrack::Type type)
{
    switch (type) {
    case MmtsTrack::Type::Video: return "video";
    case MmtsTrack::Type::Audio: return "audio";
    case MmtsTrack::Type::Subtitle: return "subtitle";
    default: return "unknown";
    }
}

bool MmtsMapReader::loadBinary(const QByteArray& data, MmtsMap& out, QString* errorMessage)
{
    qsizetype pos = 8;
    quint32 version = 0;
    quint32 flags = 0;
    quint32 trackCount = 0;
    quint32 mptCount = 0;
    quint32 rapCount = 0;
    quint32 seekCount = 0;

    if (!readPod(data, pos, version) || !readPod(data, pos, flags) ||
        !readPod(data, pos, out.sourceSize) || !readPod(data, pos, out.durationMs) ||
        !readPod(data, pos, out.firstVideoPtsMs) || !readPod(data, pos, out.lastVideoPtsMs) ||
        !readPod(data, pos, trackCount) || !readPod(data, pos, mptCount) ||
        !readPod(data, pos, rapCount) || !readPod(data, pos, seekCount)) {
        if (errorMessage)
            *errorMessage = "Truncated MMTSMAP2 header";
        return false;
    }
    if (version != 2 || flags != 0) {
        if (errorMessage)
            *errorMessage = "Unsupported MMTSMAP2 version or flags";
        return false;
    }

    out.format = "MMTSMAP2";
    out.tracks.reserve(static_cast<int>(trackCount));
    for (quint32 i = 0; i < trackCount; ++i) {
        quint8 type = 0;
        quint8 trackFlags = 0;
        quint16 reserved = 0;
        qint32 streamIndex = -1;
        quint16 packetId = 0;
        qint16 componentTag = -1;
        quint32 samplingRate = 0;
        quint32 reserved2 = 0;

        if (!readPod(data, pos, type) || !readPod(data, pos, trackFlags) ||
            !readPod(data, pos, reserved) || !readPod(data, pos, streamIndex) ||
            !readPod(data, pos, packetId) || !readPod(data, pos, componentTag) ||
            !readPod(data, pos, samplingRate) || !readPod(data, pos, reserved2)) {
            if (errorMessage)
                *errorMessage = "Truncated MMTSMAP2 track table";
            return false;
        }

        MmtsTrack track;
        track.type = trackTypeFromCode(type);
        track.streamIndex = streamIndex;
        track.packetId = packetId;
        track.componentTag = componentTag;
        track.samplingRate = samplingRate;
        track.latm = (trackFlags & 1) != 0;
        out.tracks.push_back(track);
    }

    out.mptChanges.reserve(static_cast<int>(mptCount));
    for (quint32 i = 0; i < mptCount; ++i) {
        MmtsMptChange change;
        quint32 mptTrackCount = 0;
        if (!readPod(data, pos, change.timeMs) || !readPod(data, pos, change.offset) ||
            !readPod(data, pos, mptTrackCount)) {
            if (errorMessage)
                *errorMessage = "Truncated MMTSMAP2 MPT table";
            return false;
        }
        change.trackIndices.reserve(static_cast<int>(mptTrackCount));
        for (quint32 j = 0; j < mptTrackCount; ++j) {
            quint32 index = 0;
            if (!readPod(data, pos, index)) {
                if (errorMessage)
                    *errorMessage = "Truncated MMTSMAP2 MPT track list";
                return false;
            }
            if (index < static_cast<quint32>(out.tracks.size()))
                change.trackIndices.push_back(static_cast<int>(index));
        }
        out.mptChanges.push_back(change);
    }

    auto readPoint = [&data, &pos](QVector<MmtsPoint>& points, quint32 count) {
        points.reserve(static_cast<int>(count));
        for (quint32 i = 0; i < count; ++i) {
            MmtsPoint point;
            if (!readPod(data, pos, point.timeMs) || !readPod(data, pos, point.offset))
                return false;
            points.push_back(point);
        }
        return true;
    };
    if (!readPoint(out.rapPoints, rapCount) || !readPoint(out.seekPoints, seekCount)) {
        if (errorMessage)
            *errorMessage = "Truncated MMTSMAP2 point table";
        return false;
    }

    return true;
}

bool MmtsMapReader::loadText(const QByteArray& data, MmtsMap& out, QString* errorMessage)
{
    const QString text = QString::fromUtf8(data);
    const QStringList lines = text.split('\n');
    if (lines.isEmpty() || lines.first().trimmed() != "MMTSMAP 1") {
        if (errorMessage)
            *errorMessage = "Unsupported map format";
        return false;
    }

    out.format = "MMTSMAP 1";
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = lines[i].trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith("source_size=")) {
            out.sourceSize = line.mid(12).toULongLong();
        } else if (line.startsWith("duration_ms=")) {
            out.durationMs = line.mid(12).toLongLong();
        } else if (line.startsWith("first_video_pts_ms=")) {
            out.firstVideoPtsMs = line.mid(19).toLongLong();
        } else if (line.startsWith("last_video_pts_ms=")) {
            out.lastVideoPtsMs = line.mid(18).toLongLong();
        } else if (line.startsWith("track ")) {
            const auto values = parseKeyValues(line.mid(6));
            MmtsTrack track;
            track.type = trackTypeFromText(values.value("type"));
            track.streamIndex = values.value("streamIndex").toInt();
            track.packetId = static_cast<uint16_t>(values.value("packetId").toUInt(nullptr, 0));
            track.componentTag = values.value("componentTag").toInt();
            track.samplingRate = values.value("rate").toUInt();
            track.latm = values.value("latm").toInt() != 0;
            appendUniqueTrack(out.tracks, track);
        } else if (line.startsWith("mpt ")) {
            const auto values = parseKeyValues(line.mid(4));
            MmtsMptChange change;
            change.timeMs = values.value("time_ms", "-1").toLongLong();
            change.offset = values.value("offset", "0").toULongLong();

            auto addTrackSpecs = [&](const QString& list, MmtsTrack::Type type) {
                if (list.isEmpty() || list == "-")
                    return;
                for (const QString& spec : list.split(',', Qt::SkipEmptyParts)) {
                    MmtsTrack track;
                    if (!parseTrackSpec(spec, type, track))
                        continue;
                    appendUniqueTrack(out.tracks, track);
                    const int index = findTrackIndex(out.tracks, track);
                    if (index >= 0)
                        change.trackIndices.push_back(index);
                }
            };
            addTrackSpecs(values.value("audio"), MmtsTrack::Type::Audio);
            addTrackSpecs(values.value("subtitle"), MmtsTrack::Type::Subtitle);
            out.mptChanges.push_back(change);
        } else if (line.startsWith("rap ") || line.startsWith("seek ")) {
            const bool rap = line.startsWith("rap ");
            const auto values = parseKeyValues(line.mid(rap ? 4 : 5));
            MmtsPoint point;
            point.timeMs = values.value("time_ms", "-1").toLongLong();
            point.offset = values.value("offset", "0").toULongLong();
            if (rap)
                out.rapPoints.push_back(point);
            else
                out.seekPoints.push_back(point);
        }
    }

    return true;
}
