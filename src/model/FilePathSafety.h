#pragma once
#include <QString>
#include <filesystem>
inline bool distinctOutput(const QString& source, const QString& output, QString* error) {
    try {
        const std::filesystem::path a(source.toStdWString()), b(output.toStdWString());
        std::error_code ec;
        const bool equivalent = std::filesystem::equivalent(a, b, ec);
        const auto ca = std::filesystem::weakly_canonical(a);
        const auto cb = std::filesystem::weakly_canonical(b);
#ifdef _WIN32
        const bool sameName = QString::fromStdWString(ca.wstring()).compare(QString::fromStdWString(cb.wstring()), Qt::CaseInsensitive) == 0;
#else
        const bool sameName = ca == cb;
#endif
        if (!equivalent && !sameName) return true;
        if (error) *error = QStringLiteral("Output must differ from the source (including file aliases).");
    } catch (const std::filesystem::filesystem_error&) {
        if (error) *error = QStringLiteral("Unable to validate output file identity.");
    }
    return false;
}
