#include "filepanel.h"
#include <QVBoxLayout>
#include <QHeaderView>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QElapsedTimer>
#include <QDataStream>
#include <QFile>
#include <QCryptographicHash>
#include <algorithm>

/* ═════════════════════════════════════════════════════════════════════
 *  FileListModel
 * ═════════════════════════════════════════════════════════════════════ */

FileListModel::FileListModel(QObject *parent)
    : QAbstractTableModel(parent) {}

FileListModel::~FileListModel() {
    delete m_root;
}

/* ── Filesystem mode ─────────────────────────────────────────────── */

void FileListModel::setDirectory(const QString &path) {
    beginResetModel();
    m_mode = FileSystem;
    m_currentPath = QDir::cleanPath(path);
    m_reader = nullptr;
    m_prefix.clear();
    m_fsEntries.clear();
    m_marked.clear();
    m_viewNode = nullptr;
    delete m_root; m_root = nullptr;

    QDir dir(m_currentPath);
    if (!dir.exists()) { endResetModel(); return; }

    if (m_currentPath != "/") {
        Entry e;
        e.name = "..";
        e.fullPath = QDir::cleanPath(m_currentPath + "/..");
        e.isDir = true;
        m_fsEntries.append(e);
    }

    auto items = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo &fi : items) {
        Entry e;
        e.name = fi.fileName();
        e.fullPath = fi.absoluteFilePath();
        e.size = fi.isDir() ? 0 : fi.size();
        e.modified = fi.lastModified();
        e.isDir = fi.isDir();
        m_fsEntries.append(e);
    }
    endResetModel();
}

/* ── Archive mode ────────────────────────────────────────────────── */

void FileListModel::setArchive(ArchiveReader *reader, const QString &archivePath, const QString &prefix) {
    beginResetModel();
    m_mode = Archive;
    m_reader = reader;
    m_archivePath = archivePath;
    m_prefix = prefix;
    m_fsEntries.clear();
    m_marked.clear();

    if (!m_root && reader && reader->isOpen()) {
        if (!loadTreeCache()) {
            buildTree();
            saveTreeCache();
        }
    }

    prepareView(prefix);
    endResetModel();
}

/* ── Build directory tree from archive (once) ────────────────────── */

void FileListModel::buildTree() {
    QElapsedTimer timer;
    timer.start();

    delete m_root;
    m_root = new DirNode;
    m_root->name = "";

    int total = m_reader->totalEntries();

    // Get direct SDK buffer pointers (zero function call overhead in loop)
    const Byte *dirBits = m_reader->isDirBits();
    const size_t *nameOff = m_reader->nameOffsets();
    const Byte *nameBuf = m_reader->nameBuffer();

    DirNode *lastDirNode = m_root;
    const UInt16 *lastDirRaw = nullptr;
    size_t lastDirLen = 0;

    for (int i = 0; i < total; i++) {
        // Inline isDir check (bit array)
        if (dirBits && (dirBits[i >> 3] & (0x80 >> (i & 7)))) continue;

        // Inline rawName (direct buffer access)
        size_t off = nameOff[i];
        size_t nameLen = nameOff[i + 1] - off - 1;
        const UInt16 *raw = reinterpret_cast<const UInt16*>(nameBuf + off * 2);
        if (nameLen == 0) continue;

        size_t lastSlash = 0;
        bool hasSlash = false;
        for (size_t p = 0; p < nameLen; p++) {
            if (raw[p] == '/' || raw[p] == '\\') { lastSlash = p; hasSlash = true; }
        }

        DirNode *dirNode;
        if (!hasSlash) {
            dirNode = m_root;
        } else {
            bool sameDir = (lastDirRaw && lastDirLen == lastSlash);
            if (sameDir) {
                for (size_t k = 0; k < lastSlash; k++) {
                    if (raw[k] != lastDirRaw[k]) { sameDir = false; break; }
                }
            }

            if (sameDir) {
                dirNode = lastDirNode;
            } else {
                dirNode = m_root;
                size_t compStart = 0;
                for (size_t p = 0; p <= lastSlash; p++) {
                    bool isSep = (raw[p] == '/' || raw[p] == '\\');
                    if (isSep || p == lastSlash) {
                        size_t end = (p == lastSlash && !isSep) ? p + 1 : p;
                        if (end > compStart) {
                            QString comp = QString::fromUtf16(
                                reinterpret_cast<const char16_t*>(raw + compStart), end - compStart);
                            DirNode *child = dirNode->children.value(comp, nullptr);
                            if (!child) {
                                child = new DirNode;
                                child->name = comp;
                                dirNode->children.insert(comp, child);
                            }
                            dirNode = child;
                        }
                        compStart = p + 1;
                    }
                }
                lastDirNode = dirNode;
                lastDirRaw = raw;
                lastDirLen = lastSlash;
            }
        }

        dirNode->fileIndices.append(i);
    }

    std::function<void(DirNode*)> computeCounts = [&](DirNode *n) {
        n->totalFiles = n->fileIndices.size();
        n->totalDirs = n->children.size();
        for (auto *child : n->children) {
            computeCounts(child);
            n->totalFiles += child->totalFiles;
            n->totalDirs  += child->totalDirs;
        }
    };
    computeCounts(m_root);

    m_treeBuildMs = timer.elapsed();
    qDebug() << "Tree built in" << m_treeBuildMs << "ms:"
             << m_root->totalFiles << "files,"
             << m_root->totalDirs << "dirs";
}

/* ── Tree cache (avoid re-parsing on repeat opens) ───────────────── */

static const quint32 CACHE_MAGIC = 0x375A5443;  // "7ZTC"
static const quint32 CACHE_VERSION = 1;

QString FileListModel::treeCachePath() const {
    return m_archivePath + ".tree";
}

static void serializeNode(QDataStream &out, const DirNode *n) {
    out << n->name;
    out << (qint32)n->fileIndices.size();
    for (int idx : n->fileIndices) out << (qint32)idx;
    out << n->totalFiles << n->totalDirs;
    out << (qint32)n->children.size();
    for (auto it = n->children.constBegin(); it != n->children.constEnd(); ++it) {
        serializeNode(out, it.value());
    }
}

static DirNode *deserializeNode(QDataStream &in) {
    auto *n = new DirNode;
    in >> n->name;
    qint32 numFiles;
    in >> numFiles;
    n->fileIndices.resize(numFiles);
    for (int j = 0; j < numFiles; j++) {
        qint32 idx; in >> idx;
        n->fileIndices[j] = idx;
    }
    in >> n->totalFiles >> n->totalDirs;
    qint32 numChildren;
    in >> numChildren;
    for (int j = 0; j < numChildren; j++) {
        DirNode *child = deserializeNode(in);
        n->children.insert(child->name, child);
    }
    return n;
}

void FileListModel::saveTreeCache() const {
    if (!m_root) return;
    QFile f(treeCachePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    QDataStream out(&f);
    out << CACHE_MAGIC << CACHE_VERSION;
    // Store archive mod time + size for invalidation
    QFileInfo fi(m_archivePath);
    out << fi.lastModified() << fi.size();
    serializeNode(out, m_root);
    qDebug() << "Tree cache saved:" << f.size() << "bytes";
}

bool FileListModel::loadTreeCache() {
    QFile f(treeCachePath());
    if (!f.open(QIODevice::ReadOnly)) return false;

    QElapsedTimer timer;
    timer.start();

    QDataStream in(&f);
    quint32 magic, version;
    in >> magic >> version;
    if (magic != CACHE_MAGIC || version != CACHE_VERSION) return false;

    QDateTime cachedModTime;
    qint64 cachedSize;
    in >> cachedModTime >> cachedSize;

    // Validate against current archive
    QFileInfo fi(m_archivePath);
    if (fi.lastModified() != cachedModTime || fi.size() != cachedSize) {
        qDebug() << "Tree cache stale, rebuilding";
        return false;
    }

    delete m_root;
    m_root = deserializeNode(in);

    m_treeBuildMs = timer.elapsed();
    qDebug() << "Tree loaded from cache in" << m_treeBuildMs << "ms:"
             << m_root->totalFiles << "files,"
             << m_root->totalDirs << "dirs";
    return true;
}

/* ── Prepare sorted lists for virtual view ───────────────────────── */

void FileListModel::prepareView(const QString &prefix) {
    m_viewNode = lookupNode(prefix);
    m_sortedDirNames.clear();
    m_sortedFileIndices.clear();
    m_hasUpEntry = true;  // always show ".."

    if (!m_viewNode) return;

    // Sort dir names
    m_sortedDirNames = m_viewNode->children.keys();
    m_sortedDirNames.sort(Qt::CaseInsensitive);

    // Sort file indices by name
    m_sortedFileIndices = m_viewNode->fileIndices;
    std::sort(m_sortedFileIndices.begin(), m_sortedFileIndices.end(),
              [this](int a, int b) {
        return m_reader->fileName(a).compare(m_reader->fileName(b), Qt::CaseInsensitive) < 0;
    });
}

/* ── Look up a node by prefix path ───────────────────────────────── */

DirNode *FileListModel::lookupNode(const QString &prefix) const {
    if (!m_root) return nullptr;
    if (prefix.isEmpty()) return m_root;

    DirNode *node = m_root;
    int pos = 0;
    while (pos < prefix.length()) {
        int slash = prefix.indexOf('/', pos);
        if (slash < 0) slash = prefix.length();
        if (slash == pos) { pos++; continue; }

        QString comp = prefix.mid(pos, slash - pos);
        DirNode *child = node->children.value(comp, nullptr);
        if (!child) return nullptr;
        node = child;
        pos = slash + 1;
    }
    return node;
}

/* ── Model interface (virtual scroll) ────────────────────────────── */

int FileListModel::rowCount(const QModelIndex &) const {
    if (m_mode == FileSystem) return m_fsEntries.size();
    if (!m_viewNode) return m_hasUpEntry ? 1 : 0;
    return (m_hasUpEntry ? 1 : 0) + m_sortedDirNames.size() + m_sortedFileIndices.size();
}

int FileListModel::columnCount(const QModelIndex &) const {
    return ColCount;
}

FileListModel::Entry FileListModel::entryAt(int row) const {
    Entry e;
    if (m_mode == FileSystem) {
        if (row >= 0 && row < m_fsEntries.size()) return m_fsEntries[row];
        return e;
    }

    // Archive virtual scroll
    int offset = m_hasUpEntry ? 1 : 0;
    if (m_hasUpEntry && row == 0) {
        e.name = ".."; e.isDir = true; e.fullPath = "..";
        return e;
    }

    int dirRow = row - offset;
    if (dirRow < m_sortedDirNames.size()) {
        const QString &name = m_sortedDirNames[dirRow];
        e.name = name;
        e.fullPath = m_prefix + name + "/";
        e.isDir = true;
        e.size = m_viewNode->children[name]->totalFiles;
        return e;
    }

    int fileRow = dirRow - m_sortedDirNames.size();
    if (fileRow < m_sortedFileIndices.size()) {
        int idx = m_sortedFileIndices[fileRow];
        QString fullName = m_reader->fileName(idx);
        fullName.replace('\\', '/');
        int lastSlash = fullName.lastIndexOf('/');
        e.name = (lastSlash >= 0) ? fullName.mid(lastSlash + 1) : fullName;
        e.fullPath = m_prefix + e.name;
        e.size = m_reader->fileSize(idx);
        e.modified = m_reader->fileModTime(idx);
        e.isDir = false;
        e.archiveIndex = idx;
        return e;
    }
    return e;
}

QVariant FileListModel::data(const QModelIndex &idx, int role) const {
    if (!idx.isValid() || idx.row() >= rowCount()) return {};

    if (role == Qt::ForegroundRole) {
        if (m_marked.contains(idx.row()))
            return QColor(0xff, 0xff, 0x00);
        return {};
    }
    if (role == Qt::TextAlignmentRole) {
        if (idx.column() == ColSize) return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    if (role != Qt::DisplayRole) return {};

    // Construct entry on demand
    Entry e = entryAt(idx.row());

    switch (idx.column()) {
    case ColName: return e.name;
    case ColSize:
        if (e.isDir && e.name != "..") {
            if (m_mode == Archive && e.size > 0)
                return QString("<%1 files>").arg(e.size);
            return QStringLiteral("<DIR>");
        }
        return humanSize(e.size);
    case ColModified:
        return e.modified.isValid() ? e.modified.toString("yyyy-MM-dd HH:mm") : QString();
    case ColType:
        if (e.isDir) return "Directory";
        { int dot = e.name.lastIndexOf('.');
          return dot >= 0 ? e.name.mid(dot+1).toUpper() : "File"; }
    }
    return {};
}

/* ── Marking ─────────────────────────────────────────────────────── */

void FileListModel::toggleMark(int row) {
    if (row < 0 || row >= rowCount()) return;
    Entry e = entryAt(row);
    if (e.name == "..") return;
    if (m_marked.contains(row))
        m_marked.remove(row);
    else
        m_marked.insert(row);
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

bool FileListModel::isMarked(int row) const {
    return m_marked.contains(row);
}

QVector<FileListModel::Entry> FileListModel::markedEntries() const {
    QVector<Entry> result;
    for (int row : m_marked) {
        if (row >= 0 && row < rowCount())
            result.append(entryAt(row));
    }
    return result;
}

void FileListModel::clearMarks() {
    QSet<int> old = m_marked;
    m_marked.clear();
    for (int row : old)
        emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

QVariant FileListModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case ColName:     return "Name";
    case ColSize:     return "Size";
    case ColModified: return "Modified";
    case ColType:     return "Type";
    }
    return {};
}

QString FileListModel::humanSize(qint64 bytes) {
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1024*1024) return QString::number(bytes/1024.0, 'f', 1) + " KB";
    if (bytes < 1024LL*1024*1024) return QString::number(bytes/(1024.0*1024), 'f', 1) + " MB";
    return QString::number(bytes/(1024.0*1024*1024), 'f', 2) + " GB";
}

/* ── Collect files under prefix (for copy) ───────────────────────── */

void FileListModel::collectFiles(const QString &prefix, QVector<FileCopyEntry> &out) const {
    DirNode *node = lookupNode(prefix);
    if (!node) return;

    std::function<void(const DirNode*, const QString&)> gather =
        [&](const DirNode *n, const QString &path) {
        for (int idx : n->fileIndices) {
            QString fullName = m_reader->fileName(idx);
            fullName.replace('\\', '/');
            int lastSlash = fullName.lastIndexOf('/');
            QString fname = (lastSlash >= 0) ? fullName.mid(lastSlash + 1) : fullName;
            out.append({idx, path + fname});
        }
        for (auto it = n->children.constBegin(); it != n->children.constEnd(); ++it) {
            gather(it.value(), path + it.key() + "/");
        }
    };
    gather(node, prefix);
}

/* ═════════════════════════════════════════════════════════════════════
 *  FilePanel
 * ═════════════════════════════════════════════════════════════════════ */

FilePanel::FilePanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setStyleSheet(
        "QLineEdit { background: #1a1a2e; color: #e0e0e0; border: 1px solid #333; "
        "padding: 4px 8px; font-family: 'Menlo', monospace; font-size: 13px; }");
    layout->addWidget(m_pathEdit);

    m_table = new QTableView(this);
    m_model = new FileListModel(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setShowGrid(false);
    m_table->setSortingEnabled(false);
    m_table->setFocusPolicy(Qt::StrongFocus);
    m_table->setStyleSheet(
        "QTableView { background: #0f0f23; color: #e0e0e0; border: none; "
        "font-family: 'Menlo', monospace; font-size: 12px; "
        "outline: none; }"
        "QTableView::item { padding: 2px 4px; background: #0f0f23; }"
        "QTableView::item:alternate { background: #141428; }"
        "QTableView::item:selected { background: #1e3a5f; color: #ffffff; }"
        "QTableView::item:focus { background: #1e3a5f; color: #ffffff; }"
        "QTableView::item:hover { background: #1a1a3e; }"
        "QHeaderView::section { background: #1a1a2e; color: #888; border: none; "
        "padding: 4px; font-weight: bold; }");
    layout->addWidget(m_table, 1);

    m_statusBar = new QLabel(this);
    m_statusBar->setStyleSheet(
        "QLabel { background: #1a1a2e; color: #888; padding: 3px 8px; "
        "font-family: 'Menlo', monospace; font-size: 11px; }");
    layout->addWidget(m_statusBar);

    m_table->setColumnWidth(FileListModel::ColName, 350);
    m_table->setColumnWidth(FileListModel::ColSize, 100);
    m_table->setColumnWidth(FileListModel::ColModified, 150);

    m_table->installEventFilter(this);

    connect(m_table, &QTableView::activated, this, &FilePanel::onActivated);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &FilePanel::onPathEdited);

    navigateTo(QDir::homePath());
}

bool FilePanel::eventFilter(QObject *obj, QEvent *event) {
    if (obj == m_table && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent*>(event);
        switch (ke->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            QModelIndex idx = m_table->currentIndex();
            if (idx.isValid()) onActivated(idx);
            return true;
        }
        case Qt::Key_Backspace:
        case Qt::Key_Delete:
            goUp();
            return true;
        case Qt::Key_Space: {
            QModelIndex idx = m_table->currentIndex();
            if (idx.isValid()) {
                m_model->toggleMark(idx.row());
                int nextRow = idx.row() + 1;
                if (nextRow < m_model->rowCount()) {
                    m_table->setCurrentIndex(m_model->index(nextRow, 0));
                }
            }
            return true;
        }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void FilePanel::goUp() {
    if (m_model->mode() == FileListModel::Archive) {
        QString prefix = m_model->archivePrefix();
        if (prefix.isEmpty()) {
            QString archDir = QFileInfo(m_model->archivePath()).absolutePath();
            navigateTo(archDir);
        } else {
            prefix.chop(1);
            int slash = prefix.lastIndexOf('/');
            QString newPrefix = (slash >= 0) ? prefix.left(slash + 1) : "";
            navigateToArchive(m_model->archivePath(), newPrefix);
        }
    } else {
        QString cur = m_model->currentPath();
        if (cur != "/") {
            navigateTo(QDir::cleanPath(cur + "/.."));
        }
    }
}

void FilePanel::focusTable() {
    if (m_model->rowCount() > 0) {
        m_table->setCurrentIndex(m_model->index(0, 0));
        m_table->scrollToTop();
    }
    m_table->viewport()->update();
    m_table->setFocus();
}

void FilePanel::navigateTo(const QString &path) {
    m_archive.reset();
    m_model->setDirectory(path);
    m_pathEdit->setText(m_model->currentPath());
    updateStatus();
    focusTable();
    emit pathChanged(m_model->currentPath());
}

void FilePanel::navigateToArchive(const QString &archivePath, const QString &prefix) {
    QString safeArchivePath = archivePath;
    QString safePrefix = prefix;
    QElapsedTimer timer;
    timer.start();

    bool freshOpen = false;
    if (safeArchivePath.isEmpty())
        safeArchivePath = m_model->archivePath();

    if (!m_archive || m_archive->isOpen() == false) {
        m_archive = std::make_unique<ArchiveReader>();
        if (!m_archive->open(safeArchivePath)) {
            emit statusMessage("Failed to open archive: " + safeArchivePath);
            m_archive.reset();
            return;
        }
        freshOpen = true;
    }

    m_model->setArchive(m_archive.get(), safeArchivePath, safePrefix);

    double totalMs = timer.nsecsElapsed() / 1e6;

    m_pathEdit->setText(safeArchivePath + ":" + safePrefix);

    int entries = m_model->entryCount() - 1;
    QString msg;
    if (freshOpen) {
        msg = QString("[7z] %1 entries | Index: %2 ms | Tree: %3 ms | Total: %4 ms")
            .arg(entries)
            .arg(m_archive->indexParseTimeMs(), 0, 'f', 1)
            .arg(m_model->treeBuildTimeMs(), 0, 'f', 1)
            .arg(totalMs, 0, 'f', 1);
    } else {
        msg = QString("[7z] %1 entries | Navigate: %2 ms")
            .arg(entries)
            .arg(totalMs, 0, 'f', 1);
    }
    m_statusBar->setText(msg);

    focusTable();
    if (freshOpen) {
        int files = m_archive->fileCount();
        int dirs = m_archive->totalEntries() - files;
        emit archiveOpened(m_archive->indexParseTimeMs(), m_model->treeBuildTimeMs(), totalMs, files, dirs);
    }
    emit pathChanged(safeArchivePath + ":" + safePrefix);
}

QString FilePanel::currentPath() const {
    return m_model->currentPath();
}

bool FilePanel::isInArchive() const {
    return m_model->mode() == FileListModel::Archive;
}

QVector<FileListModel::Entry> FilePanel::selectedEntries() const {
    auto marked = m_model->markedEntries();
    if (!marked.isEmpty()) return marked;

    QModelIndex idx = m_table->currentIndex();
    if (idx.isValid()) {
        auto entry = m_model->entryAt(idx.row());
        if (entry.name != ".." && !entry.name.isEmpty()) {
            QVector<FileListModel::Entry> result;
            result.append(entry);
            return result;
        }
    }
    return {};
}

void FilePanel::onActivated(const QModelIndex &idx) {
    if (!idx.isValid()) return;
    auto entry = m_model->entryAt(idx.row());

    if (m_model->mode() == FileListModel::Archive) {
        if (entry.name == "..") {
            goUp();
        } else if (entry.isDir) {
            navigateToArchive(m_model->archivePath(), entry.fullPath);
        }
        return;
    }

    if (entry.isDir) {
        if (entry.name == "..") {
            goUp();
        } else {
            navigateTo(entry.fullPath);
        }
    } else if (entry.name.endsWith(".7z", Qt::CaseInsensitive)) {
        navigateToArchive(entry.fullPath);
    }
}

void FilePanel::onPathEdited() {
    QString text = m_pathEdit->text().trimmed();
    if (text.contains(':')) {
        int colonPos = text.indexOf(':');
        navigateToArchive(text.left(colonPos), text.mid(colonPos + 1));
    } else {
        navigateTo(text);
    }
}

void FilePanel::updateStatus() {
    int files = 0, dirs = 0;
    qint64 totalSize = 0;
    for (int i = 0; i < m_model->entryCount(); i++) {
        auto e = m_model->entryAt(i);
        if (e.name == "..") continue;
        if (e.isDir) dirs++; else { files++; totalSize += e.size; }
    }
    m_statusBar->setText(QString("%1 files, %2 dirs, %3")
        .arg(files).arg(dirs).arg(FileListModel::humanSize(totalSize)));
}
