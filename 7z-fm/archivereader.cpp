#include "archivereader.h"
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QtConcurrent>
#include <QThreadPool>
#include <algorithm>
#include <time.h>

/* ── Static allocator ────────────────────────────────────────────── */

const ISzAlloc ArchiveReader::s_alloc = { SzAlloc, SzFree };

/* ── Timer ───────────────────────────────────────────────────────── */

double ArchiveReader::nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ── Constructor / Destructor ────────────────────────────────────── */

ArchiveReader::ArchiveReader(QObject *parent) : QObject(parent) {
    CrcGenerateTable();
}

ArchiveReader::~ArchiveReader() {
    close();
}

/* ── Open archive and parse index ────────────────────────────────── */

bool ArchiveReader::open(const QString &path) {
    close();
    m_path = path;

    QByteArray pathUtf8 = path.toUtf8();

    if (InFile_Open(&m_archiveStream.file, pathUtf8.constData())) {
        return false;
    }

    FileInStream_CreateVTable(&m_archiveStream);
    LookToRead2_CreateVTable(&m_lookStream, 0);

    m_lookBuf = (Byte *)ISzAlloc_Alloc(&s_alloc, INPUT_BUF_SIZE);
    if (!m_lookBuf) { File_Close(&m_archiveStream.file); return false; }

    m_lookStream.buf      = m_lookBuf;
    m_lookStream.bufSize  = INPUT_BUF_SIZE;
    m_lookStream.realStream = &m_archiveStream.vt;
    LookToRead2_INIT(&m_lookStream);

    SzArEx_Init(&m_db);

    double t0 = nowUs();
    SRes res = SzArEx_Open(&m_db, &m_lookStream.vt, &s_alloc, &s_alloc);
    m_indexTimeMs = (nowUs() - t0) / 1000.0;

    if (res != SZ_OK) {
        ISzAlloc_Free(&s_alloc, m_lookBuf);
        m_lookBuf = nullptr;
        File_Close(&m_archiveStream.file);
        return false;
    }

    m_opened = true;
    return true;
}

void ArchiveReader::close() {
    if (m_opened) {
        SzArEx_Free(&m_db, &s_alloc);
        m_opened = false;
    }
    if (m_lookBuf) {
        ISzAlloc_Free(&s_alloc, m_lookBuf);
        m_lookBuf = nullptr;
    }
    File_Close(&m_archiveStream.file);
}

/* ── Index queries ───────────────────────────────────────────────── */

int ArchiveReader::fileCount() const {
    if (!m_opened) return 0;
    int count = 0;
    for (UInt32 i = 0; i < m_db.NumFiles; i++)
        if (!SzArEx_IsDir(&m_db, i)) count++;
    return count;
}

int ArchiveReader::totalEntries() const {
    return m_opened ? (int)m_db.NumFiles : 0;
}

QString ArchiveReader::nameAt(int idx) const {
    if (!m_opened || idx < 0 || idx >= (int)m_db.NumFiles) return {};

    size_t len = SzArEx_GetFileNameUtf16(&m_db, idx, nullptr);
    if (len <= 1) return {};

    QVector<UInt16> buf(len);
    SzArEx_GetFileNameUtf16(&m_db, idx, buf.data());

    // Convert UInt16 (UTF-16LE) to QString
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()), len - 1);
}

QString  ArchiveReader::fileName(int idx)  const { return nameAt(idx); }
qint64   ArchiveReader::fileSize(int idx)  const {
    return m_opened ? (qint64)SzArEx_GetFileSize(&m_db, idx) : 0;
}
bool ArchiveReader::isDir(int idx) const {
    return m_opened && SzArEx_IsDir(&m_db, idx);
}
quint32 ArchiveReader::fileCRC(int idx) const {
    if (!m_opened) return 0;
    if (SzBitWithVals_Check(&m_db.CRCs, idx))
        return m_db.CRCs.Vals[idx];
    return 0;
}
quint32 ArchiveReader::fileFolder(int idx) const {
    return m_opened ? m_db.FileToFolder[idx] : 0;
}
QDateTime ArchiveReader::fileModTime(int idx) const {
    if (!m_opened || !SzBitWithVals_Check(&m_db.MTime, idx))
        return {};
    // NTFS timestamp: 100-ns intervals since 1601-01-01
    const CNtfsFileTime &ft = m_db.MTime.Vals[idx];
    quint64 ticks = ((quint64)ft.High << 32) | ft.Low;
    // Convert to Unix epoch (subtract NTFS-to-Unix offset)
    static const quint64 NTFS_EPOCH_DIFF = 116444736000000000ULL;
    if (ticks < NTFS_EPOCH_DIFF) return {};
    qint64 unixSecs = (ticks - NTFS_EPOCH_DIFF) / 10000000ULL;
    return QDateTime::fromSecsSinceEpoch(unixSecs);
}
int ArchiveReader::folderCount() const {
    return m_opened ? (int)m_db.db.NumFolders : 0;
}

/* ── Single-file extraction (thread-safe, own fd) ────────────────── */

ExtractResult ArchiveReader::extract(int fileIndex) const {
    ExtractResult r;
    if (!m_opened || fileIndex < 0 || fileIndex >= (int)m_db.NumFiles) {
        r.error = "Invalid index";
        return r;
    }

    QByteArray pathUtf8 = m_path.toUtf8();

    // Open a separate file descriptor for thread safety
    CFileInStream strm;
    if (InFile_Open(&strm.file, pathUtf8.constData())) {
        r.error = "Cannot open archive file";
        return r;
    }
    FileInStream_CreateVTable(&strm);

    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, 0);
    Byte *buf = (Byte *)ISzAlloc_Alloc(&s_alloc, INPUT_BUF_SIZE);
    look.buf = buf;
    look.bufSize = INPUT_BUF_SIZE;
    look.realStream = &strm.vt;
    LookToRead2_INIT(&look);

    UInt32 blockIdx = (UInt32)-1;
    Byte  *outBuf = nullptr;
    size_t outBufSize = 0;
    size_t offset = 0, outSize = 0;

    double t0 = nowUs();
    SRes res = SzArEx_Extract(
        &m_db, &look.vt, (UInt32)fileIndex,
        &blockIdx, &outBuf, &outBufSize,
        &offset, &outSize, &s_alloc, &s_alloc);
    double dt = nowUs() - t0;

    if (res == SZ_OK) {
        r.data = QByteArray(reinterpret_cast<const char*>(outBuf + offset), outSize);
        r.ok = true;
        r.totalUs = dt;
        r.decompUs = dt;  // extraction is decompression-dominated
        r.folder = blockIdx;
        r.unpacked = outSize;
        // Packed size approximation
        UInt32 fi = m_db.FileToFolder[fileIndex];
        if (fi < m_db.db.NumFolders) {
            UInt32 ps = m_db.db.FoStartPackStreamIndex[fi];
            UInt32 pe = m_db.db.FoStartPackStreamIndex[fi + 1];
            r.packed = 0;
            for (UInt32 p = ps; p < pe; p++)
                r.packed += m_db.db.PackPositions[p+1] - m_db.db.PackPositions[p];
        }
    } else {
        r.error = QString("Extract failed (err %1)").arg(res);
    }

    ISzAlloc_Free(&s_alloc, outBuf);
    ISzAlloc_Free(&s_alloc, buf);
    File_Close(&strm.file);
    return r;
}

/* ── Batch extraction (multi-threaded, one block at a time per thread) ── */

QVector<ArchiveReader::BatchResult>
ArchiveReader::extractBatch(const QVector<BatchEntry> &entries, int threadCount) const
{
    QVector<BatchResult> results(entries.size());
    if (!m_opened || entries.isEmpty()) return results;

    // Sort entries by folder index for sequential I/O
    QVector<int> order(entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        UInt32 fa = m_db.FileToFolder[entries[a].fileIndex];
        UInt32 fb = m_db.FileToFolder[entries[b].fileIndex];
        return fa < fb;
    });

    // Group by folder
    struct FolderGroup {
        UInt32 folder;
        QVector<int> entryIndices; // indices into 'entries'
    };
    QVector<FolderGroup> groups;
    UInt32 curFolder = (UInt32)-1;
    for (int oi : order) {
        UInt32 f = m_db.FileToFolder[entries[oi].fileIndex];
        if (f >= m_db.db.NumFolders) {
            // Empty file, no block
            results[oi].ok = true;
            results[oi].size = 0;
            continue;
        }
        if (groups.isEmpty() || groups.last().folder != f) {
            FolderGroup g; g.folder = f;
            groups.append(g);
        }
        groups.last().entryIndices.append(oi);
    }

    // Process groups in parallel using QtConcurrent
    QAtomicInt doneCount(0);
    int total = entries.size();

    auto processGroup = [&](const FolderGroup &grp) {
        // Each thread opens its own file descriptor
        QByteArray pathUtf8 = m_path.toUtf8();
        CFileInStream strm;
        if (InFile_Open(&strm.file, pathUtf8.constData())) {
            for (int oi : grp.entryIndices) {
                results[oi].error = "Cannot open archive";
            }
            return;
        }
        FileInStream_CreateVTable(&strm);

        CLookToRead2 look;
        LookToRead2_CreateVTable(&look, 0);
        Byte *buf = (Byte *)ISzAlloc_Alloc(&s_alloc, INPUT_BUF_SIZE);
        look.buf = buf;
        look.bufSize = INPUT_BUF_SIZE;
        look.realStream = &strm.vt;
        LookToRead2_INIT(&look);

        UInt32 blockIdx = (UInt32)-1;
        Byte  *outBuf = nullptr;
        size_t outBufSize = 0;

        for (int oi : grp.entryIndices) {
            const BatchEntry &entry = entries[oi];
            BatchResult &res = results[oi];

            size_t offset = 0, outSize = 0;

            double t0 = nowUs();
            SRes sres = SzArEx_Extract(
                &m_db, &look.vt, (UInt32)entry.fileIndex,
                &blockIdx, &outBuf, &outBufSize,
                &offset, &outSize, &s_alloc, &s_alloc);
            double decompDt = nowUs() - t0;

            if (sres != SZ_OK) {
                res.error = QString("Extract err %1").arg(sres);
                continue;
            }

            res.decompUs = decompDt;
            res.size = outSize;

            // Write to destination (create parent dirs if needed)
            double wt0 = nowUs();
            QDir().mkpath(QFileInfo(entry.destPath).absolutePath());
            QFile out(entry.destPath);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(reinterpret_cast<const char*>(outBuf + offset), outSize);
                out.close();
                res.ok = true;
            } else {
                res.error = "Cannot write " + entry.destPath;
            }
            res.writeUs = nowUs() - wt0;

            int done = doneCount.fetchAndAddRelaxed(1) + 1;
            emit const_cast<ArchiveReader*>(this)->batchProgress(
                done, total,
                res.decompUs / 1000.0, res.writeUs / 1000.0);
        }

        // Free block buffer (one block at a time)
        ISzAlloc_Free(&s_alloc, outBuf);
        ISzAlloc_Free(&s_alloc, buf);
        File_Close(&strm.file);
    };

    // Use thread pool for parallel folder decompression
    QThreadPool pool;
    pool.setMaxThreadCount(threadCount);

    QVector<QFuture<void>> futures;
    for (const FolderGroup &grp : groups) {
        futures.append(QtConcurrent::run(&pool, processGroup, grp));
    }
    for (auto &f : futures) f.waitForFinished();

    return results;
}
