#include "FilePathSafety.h"
#include "MmtsMapBuilder.h"

#include "mmtTlvDemuxer.h"
#include "mmtsMapWriter.h"
#include "stream.h"

#include <QFile>
#include <filesystem>
#include <fstream>
#include <vector>

bool MmtsMapBuilder::build(const QString& srcPath, const QString& mapPath, QString* errorMessage,
                           const std::function<bool(qint64, qint64)>& progress)
{
    if (!distinctOutput(srcPath, mapPath, errorMessage)) return false;
    std::ifstream ifs(std::filesystem::path(srcPath.toStdWString()), std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot open source: %1").arg(srcPath);
        return false;
    }
    const std::streamoff total = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    MmtTlv::MmtsMapWriter writer;
    if (!writer.open(std::filesystem::path(mapPath.toStdWString()), MmtTlv::MmtsMapWriter::Format::Binary)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot create map: %1").arg(mapPath);
        return false;
    }
    writer.setSourceSize(static_cast<uint64_t>(total > 0 ? total : 0));

    MmtTlv::MmtTlvDemuxer demuxer;
    // Each decoded (pass-through) TLV packet advances the writer's byte offset,
    // so recorded RAP/seek/MPT offsets are source-file offsets.
    demuxer.setDecodedDumpCallback([&writer](const uint8_t*, size_t size) {
        writer.noteOutputPacket(size);
    });
    demuxer.setDemuxerHandler(writer);

    constexpr size_t kChunk = 4 * 1024 * 1024;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    long long readBytes = 0;
    bool cancelled = false;

    while (true) {
        if (buf.size() < 2 * 1024 * 1024) {
            const size_t old = buf.size();
            buf.resize(old + kChunk);
            ifs.read(reinterpret_cast<char*>(buf.data() + old), kChunk);
            const std::streamsize got = ifs.gcount();
            buf.resize(old + static_cast<size_t>(got));
            if (got > 0)
                readBytes += got;
            if (got == 0 && buf.empty())
                break;
        }

        size_t consumed = 0;
        {
            MmtTlv::Common::ReadStream stream(buf);
            while (!stream.isEof()) {
                const size_t before = stream.getPos();
                MmtTlv::DemuxStatus status;
                try {
                    status = demuxer.demux(stream);
                } catch (...) {
                    break;
                }
                if (status == MmtTlv::DemuxStatus::NotEnoughBuffer)
                    break;
                if (stream.getPos() == before)
                    break;
            }
            consumed = buf.size() - stream.leftBytes();
        }
        if (consumed > 0)
            buf.erase(buf.begin(), buf.begin() + consumed);
        else if (ifs.eof() || !ifs.good())
            break;

        if (progress && !progress(readBytes - static_cast<long long>(buf.size()), total)) {
            cancelled = true;
            break;
        }
    }

    writer.close();
    if (cancelled) {
        QFile::remove(mapPath);
        if (errorMessage)
            *errorMessage = QStringLiteral("Cancelled");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void MmtsMapBuildWorker::configure(const QString& srcPath, const QString& mapPath)
{
    m_srcPath = srcPath;
    m_mapPath = mapPath;
    m_cancel.store(false);
}

void MmtsMapBuildWorker::run()
{
    QString error;
    const bool ok = MmtsMapBuilder::build(
        m_srcPath, m_mapPath, &error,
        [this](qint64 done, qint64 total) {
            emit progress(done, total);
            return !m_cancel.load();
        });
    emit finished(ok, error);
}
