#pragma once
/*
 * ArchiveReader — Fast in-memory 7z index with multi-threaded extraction.
 *
 * Design:
 *   - Index (CSzArEx) is parsed once and cached in memory.
 *   - Each extraction opens a SEPARATE file descriptor so multiple
 *     blocks can be decompressed concurrently.
 *   - Only one decompressed block is kept per extraction context.
 */

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QVector>
#include <QDateTime>
#include <QMutex>

extern "C" {
#include "Precomp.h"
#include "7z.h"
#include "7zAlloc.h"
#include "7zBuf.h"
#include "7zCrc.h"
#include "7zFile.h"
}

/* ── Extraction result with timing ─────────────────────────────── */

struct ExtractResult {
    QByteArray data;
    double seekUs     = 0;   // time to seek + read packed data
    double decompUs   = 0;   // decompression time
    double totalUs    = 0;   // total extraction time
    quint32 folder    = 0;
    qint64  packed    = 0;
    qint64  unpacked  = 0;
    bool    ok        = false;
    QString error;
};

/* ── Archive Reader ────────────────────────────────────────────── */

class ArchiveReader : public QObject {
    Q_OBJECT
public:
    explicit ArchiveReader(QObject *parent = nullptr);
    ~ArchiveReader() override;

    /* Open + parse index (returns time in ms) */
    bool   open(const QString &path);
    void   close();
    bool   isOpen() const { return m_opened; }
    double indexParseTimeMs() const { return m_indexTimeMs; }

    /* In-memory index queries (all O(1)) */
    int      fileCount()               const;
    int      totalEntries()            const;  // files + dirs
    QString  fileName(int idx)         const;
    qint64   fileSize(int idx)         const;
    bool     isDir(int idx)            const;
    quint32  fileCRC(int idx)          const;
    quint32  fileFolder(int idx)       const;
    QDateTime fileModTime(int idx)     const;
    int      folderCount()             const;

    /* Raw UTF-16 name access (zero-copy, for fast tree building) */
    const UInt16 *rawName(int idx, size_t &len) const {
        if (!m_opened || idx < 0 || idx >= (int)m_db.NumFiles) { len = 0; return nullptr; }
        size_t off = m_db.FileNameOffsets[idx];
        len = m_db.FileNameOffsets[idx + 1] - off - 1;  // exclude null terminator
        return reinterpret_cast<const UInt16*>(m_db.FileNames + off * 2);
    }

    /* Direct SDK buffer access for hot-loop iteration */
    const Byte *isDirBits() const { return m_opened ? m_db.IsDirs : nullptr; }
    const size_t *nameOffsets() const { return m_opened ? m_db.FileNameOffsets : nullptr; }
    const Byte *nameBuffer() const { return m_opened ? m_db.FileNames : nullptr; }
    const UInt64 *unpackPositions() const { return m_opened ? m_db.UnpackPositions : nullptr; }

    /* Single-file extraction (thread-safe, opens its own fd) */
    ExtractResult extract(int fileIndex) const;

    /* Multi-file extraction grouped by block (parallel) */
    struct BatchEntry {
        int     fileIndex;
        QString destPath;
    };
    struct BatchResult {
        int     fileIndex;
        double  decompUs = 0;
        double  writeUs  = 0;
        qint64  size     = 0;
        bool    ok       = false;
        QString error;
    };
    QVector<BatchResult> extractBatch(const QVector<BatchEntry> &entries,
                                      int threadCount = 4) const;

signals:
    void batchProgress(int done, int total,
                       double decompMs, double writeMs);

private:
    /* UTF-16 → QString conversion */
    QString nameAt(int idx) const;

    /* Microsecond timer */
    static double nowUs();

    /* Archive state */
    bool              m_opened = false;
    QString           m_path;
    double            m_indexTimeMs = 0;

    CFileInStream     m_archiveStream;
    CLookToRead2      m_lookStream;
    CSzArEx           m_db;
    Byte             *m_lookBuf = nullptr;

    static constexpr size_t INPUT_BUF_SIZE = (1 << 18);
    static const ISzAlloc s_alloc;
};
