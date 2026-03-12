#pragma once
/*
 * FilePanel — Single panel with path bar, virtual-scrolled file table,
 *             and status bar. Shows filesystem OR 7z archive contents.
 *
 * Archive: DirNode trie built once on open.
 * View: model returns data() on-the-fly from sorted dir/file lists.
 *        Only visible rows (~30) trigger string construction.
 */

#include <QWidget>
#include <QTableView>
#include <QLineEdit>
#include <QLabel>
#include <QAbstractTableModel>
#include <QFileInfoList>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QHash>
#include <memory>
#include "archivereader.h"

/* ── Archive directory tree node ─────────────────────────────────── */

struct DirNode {
    QString name;
    QHash<QString, DirNode*> children;
    QVector<int> fileIndices;             // archive indices of files here
    int totalFiles = 0;
    int totalDirs  = 0;
    ~DirNode() { qDeleteAll(children); }
};

/* ── Unified file list model (FS or Archive) ─────────────────────── */

class FileListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColName = 0, ColSize, ColModified, ColType, ColCount };
    enum Mode   { FileSystem, Archive };

    explicit FileListModel(QObject *parent = nullptr);
    ~FileListModel() override;

    void setDirectory(const QString &path);
    void setArchive(ArchiveReader *reader, const QString &archivePath, const QString &prefix = {});

    Mode mode() const { return m_mode; }
    QString currentPath() const { return m_currentPath; }
    QString archivePath() const { return m_archivePath; }
    QString archivePrefix() const { return m_prefix; }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &idx, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    /* Entry for external use (selectedEntries, copy, etc.) */
    struct Entry {
        QString name;
        QString fullPath;
        qint64  size = 0;
        QDateTime modified;
        bool    isDir = false;
        int     archiveIndex = -1;
    };
    Entry entryAt(int row) const;  // constructs on demand
    int entryCount() const { return rowCount(); }

    /* Marking (Total Commander style — yellow text) */
    void toggleMark(int row);
    bool isMarked(int row) const;
    QVector<Entry> markedEntries() const;
    void clearMarks();

    /* Tree stats */
    double treeBuildTimeMs() const { return m_treeBuildMs; }

    /* Collect all file indices + dest paths under a prefix (for copy) */
    struct FileCopyEntry { int archiveIndex; QString relativePath; };
    void collectFiles(const QString &prefix, QVector<FileCopyEntry> &out) const;

    static QString humanSize(qint64 bytes);

private:
    void buildTree();
    void prepareView(const QString &prefix);
    DirNode *lookupNode(const QString &prefix) const;
    bool loadTreeCache();
    void saveTreeCache() const;
    QString treeCachePath() const;

    Mode              m_mode = FileSystem;
    QString           m_currentPath;
    QString           m_archivePath;
    QString           m_prefix;
    ArchiveReader    *m_reader = nullptr;
    QSet<int>         m_marked;

    // Filesystem mode: pre-built entries (small dirs, no virtual scroll needed)
    QVector<Entry>    m_fsEntries;

    // Archive mode: virtual scroll from sorted lists
    DirNode          *m_root = nullptr;
    DirNode          *m_viewNode = nullptr;       // current node being displayed
    QStringList       m_sortedDirNames;           // sorted child dir names
    QVector<int>      m_sortedFileIndices;        // sorted file indices (by name)
    bool              m_hasUpEntry = false;        // show ".." at row 0

    double            m_treeBuildMs = 0;
};

/* ── File Panel Widget ───────────────────────────────────────────── */

class FilePanel : public QWidget {
    Q_OBJECT
public:
    explicit FilePanel(QWidget *parent = nullptr);

    void navigateTo(const QString &path);
    void navigateToArchive(const QString &archivePath, const QString &prefix = {});
    void goUp();

    QString currentPath() const;
    bool    isInArchive() const;
    ArchiveReader *archiveReader() const { return m_archive.get(); }
    FileListModel *model() const { return m_model; }
    QTableView    *tableView() const { return m_table; }

    QVector<FileListModel::Entry> selectedEntries() const;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

signals:
    void statusMessage(const QString &msg);
    void archiveOpened(double parseTimeMs, double treeBuildMs, double totalMs, int fileCount, int dirCount);
    void pathChanged(const QString &newPath);

private slots:
    void onActivated(const QModelIndex &idx);
    void onPathEdited();

private:
    void focusTable();
    QLineEdit      *m_pathEdit;
    QTableView     *m_table;
    QLabel         *m_statusBar;
    FileListModel  *m_model;
    std::unique_ptr<ArchiveReader> m_archive;

    void updateStatus();
};
