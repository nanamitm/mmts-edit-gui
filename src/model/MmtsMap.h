#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <cstdint>

struct MmtsTrack {
    enum class Type {
        Unknown,
        Video,
        Audio,
        Subtitle,
    };

    Type type = Type::Unknown;
    int streamIndex = -1;
    uint16_t packetId = 0;
    int componentTag = -1;
    uint32_t samplingRate = 0;
    bool latm = false;
    uint8_t audioMode = 0;
    uint8_t channels = 0;
};

struct MmtsPoint {
    qint64 timeMs = -1;
    quint64 offset = 0;
};

struct MmtsMptChange {
    qint64 timeMs = -1;
    quint64 offset = 0;
    QVector<int> trackIndices;
};

struct MmtsMap {
    QString path;
    QString format;
    quint64 sourceSize = 0;
    qint64 durationMs = 0;
    qint64 firstVideoPtsMs = -1;
    qint64 lastVideoPtsMs = -1;
    QVector<MmtsTrack> tracks;
    QVector<MmtsMptChange> mptChanges;
    QVector<MmtsPoint> rapPoints;
    QVector<MmtsPoint> seekPoints;
};

Q_DECLARE_METATYPE(MmtsMap)

class MmtsMapReader {
public:
    static bool load(const QString& path, MmtsMap& out, QString* errorMessage = nullptr);

    static QString trackTypeName(MmtsTrack::Type type);

private:
    static bool loadBinary(const QByteArray& data, MmtsMap& out, QString* errorMessage);
    static bool loadText(const QByteArray& data, MmtsMap& out, QString* errorMessage);
};
