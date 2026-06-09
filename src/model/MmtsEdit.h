#pragma once

#include <QString>
#include <QVector>
#include <cstdint>

// One non-destructive cut: a [startMs, endMs) range of the SOURCE, in relative
// (0-based) milliseconds matching the .mmtsmap duration convention.
struct EditSegment {
    qint64 startMs = 0;
    qint64 endMs = 0;
    qint64 durationMs() const { return endMs > startMs ? endMs - startMs : 0; }
};

// In-memory model of a .mmtsedit sidecar (an EDL: ordered list of source cuts).
//
// File shape (version 1):
//   { "version":1, "source":..., "sourceSize":..., "map":...,
//     "timeline":[ {"sourceStartMs":..,"sourceEndMs":..}, ... ] }
//
// The edited program is the concatenation of the timeline segments in order.
struct MmtsEdit {
    int version = 1;
    QString source;
    quint64 sourceSize = 0;
    QString map;
    QVector<EditSegment> segments;

    // Sum of segment durations (the edited program length).
    qint64 totalDurationMs() const;

    static bool load(const QString& path, MmtsEdit& out, QString* errorMessage = nullptr);
    bool save(const QString& path, QString* errorMessage = nullptr) const;
};
