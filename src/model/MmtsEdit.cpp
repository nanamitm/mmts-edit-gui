#include "MmtsEdit.h"

#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

qint64 MmtsEdit::totalDurationMs() const
{
    qint64 total = 0;
    for (const auto& s : segments)
        total += s.durationMs();
    return total;
}

bool MmtsEdit::load(const QString& path, MmtsEdit& out, QString* errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = file.errorString();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Invalid .mmtsedit JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    MmtsEdit edit;
    edit.version = root.value("version").toInt(1);
    edit.source = root.value("source").toString();
    edit.sourceSize = static_cast<quint64>(root.value("sourceSize").toDouble(0));
    edit.map = root.value("map").toString();

    const QJsonArray timeline = root.value("timeline").toArray();
    for (const QJsonValue& v : timeline) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        EditSegment seg;
        seg.startMs = static_cast<qint64>(o.value("sourceStartMs").toDouble(0));
        seg.endMs = static_cast<qint64>(o.value("sourceEndMs").toDouble(seg.startMs));
        if (seg.endMs > seg.startMs)
            edit.segments.push_back(seg);
    }

    out = edit;
    return true;
}

bool MmtsEdit::save(const QString& path, QString* errorMessage) const
{
    QJsonArray timeline;
    for (const auto& seg : segments) {
        QJsonObject o;
        o["sourceStartMs"] = static_cast<double>(seg.startMs);
        o["sourceEndMs"] = static_cast<double>(seg.endMs);
        timeline.append(o);
    }

    QJsonObject root;
    root["version"] = version;
    root["source"] = source;
    root["sourceSize"] = static_cast<double>(sourceSize);
    root["map"] = map;
    root["timeline"] = timeline;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage)
            *errorMessage = file.errorString();
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    return true;
}
