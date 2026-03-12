#include "mainwindow.h"
#include <QVBoxLayout>
#include <QSplitter>
#include <QKeyEvent>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QApplication>
#include <QThread>
#include <QScrollBar>
#include <time.h>

static double nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ── Bottom toolbar (Total Commander style) ──────────────────────── */

QWidget *MainWindow::createToolbar() {
    auto *bar = new QWidget(this);
    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto makeBtn = [&](const QString &label, auto slot) {
        auto *btn = new QPushButton(label, bar);
        btn->setMinimumHeight(28);
        btn->setStyleSheet(
            "QPushButton { background: #1a1a2e; color: #ccc; border: 1px solid #333; "
            "padding: 2px 12px; font-family: 'SF Mono','Menlo',monospace; font-size: 11px; }"
            "QPushButton:hover { background: #2a2a4e; color: #fff; }"
            "QPushButton:pressed { background: #3a3a6e; }");
        connect(btn, &QPushButton::clicked, this, slot);
        lay->addWidget(btn, 1);
        return btn;
    };

    makeBtn("F3 View",   [](){});  // placeholder
    makeBtn("F4 Edit",   [](){});  // placeholder
    makeBtn("F5 Copy",   &MainWindow::doCopy);
    makeBtn("F6 Move",   [](){});  // placeholder
    makeBtn("F7 MkDir",  [](){});  // placeholder
    makeBtn("F8 Delete", [](){});  // placeholder
    makeBtn("F10 Quit",  &MainWindow::close);

    return bar;
}

/* ── Constructor ─────────────────────────────────────────────────── */

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("7z File Manager — Prototype");
    resize(1600, 900);

    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // Splitter for dual panels
    auto *splitter = new QSplitter(Qt::Horizontal, central);
    m_leftPanel  = new FilePanel(splitter);
    m_rightPanel = new FilePanel(splitter);
    splitter->addWidget(m_leftPanel);
    splitter->addWidget(m_rightPanel);
    splitter->setSizes({800, 800});
    splitter->setStyleSheet("QSplitter::handle { background: #333; width: 2px; }");
    mainLayout->addWidget(splitter, 1);

    // Progress bar (hidden by default)
    m_progressBar = new QProgressBar(central);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setVisible(false);
    m_progressBar->setMaximumHeight(20);
    m_progressBar->setStyleSheet(
        "QProgressBar { background: #1a1a2e; border: 1px solid #333; border-radius: 3px; "
        "color: #fff; font-family: 'SF Mono','Menlo',monospace; font-size: 11px; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, "
        "stop:0 #1e5a2e, stop:1 #2e8a4e); border-radius: 3px; }");
    mainLayout->addWidget(m_progressBar);

    // Log view
    m_logView = new QTextEdit(central);
    m_logView->setReadOnly(true);
    m_logView->setMaximumHeight(180);
    m_logView->setStyleSheet(
        "QTextEdit { background: #0a0a1a; color: #6dff6d; border: 1px solid #333; "
        "font-family: 'SF Mono', 'Menlo', monospace; font-size: 11px; padding: 4px; }");
    mainLayout->addWidget(m_logView);

    // Status line
    m_globalStatus = new QLabel("Ready", central);
    m_globalStatus->setStyleSheet(
        "QLabel { background: #16213e; color: #0f0; padding: 4px 8px; "
        "font-family: 'SF Mono', 'Menlo', monospace; font-size: 12px; font-weight: bold; }");
    mainLayout->addWidget(m_globalStatus);

    // Bottom toolbar with F-key buttons
    m_toolbar = createToolbar();
    mainLayout->addWidget(m_toolbar);

    setCentralWidget(central);

    // Active panel
    setActivePanel(m_leftPanel);

    // Connect signals
    connect(m_leftPanel, &FilePanel::statusMessage, this, &MainWindow::log);
    connect(m_rightPanel, &FilePanel::statusMessage, this, &MainWindow::log);

    auto logArchive = [this](double parseMs, double treeMs, double totalMs, int files, int dirs) {
        log(QString("📦 Archive opened: %1 files, %2 dirs | Index: %3 ms | Tree: %4 ms | Total: %5 ms")
            .arg(files).arg(dirs)
            .arg(parseMs, 0, 'f', 1).arg(treeMs, 0, 'f', 1).arg(totalMs, 0, 'f', 1));
    };
    connect(m_leftPanel, &FilePanel::archiveOpened, this, logArchive);
    connect(m_rightPanel, &FilePanel::archiveOpened, this, logArchive);

    // Focus tracking
    auto trackFocus = [this](FilePanel *panel) {
        connect(panel->tableView(), &QTableView::clicked,
                this, [this, panel]() { setActivePanel(panel); });
    };
    trackFocus(m_leftPanel);
    trackFocus(m_rightPanel);

    // Default paths
    m_leftPanel->navigateTo(QDir::homePath());
    m_rightPanel->navigateTo(QDir::homePath());

    log("7z File Manager started. Navigate with arrows, Enter, Backspace. Space to select. F5 to copy.");
}

void MainWindow::setActivePanel(FilePanel *panel) {
    m_activePanel = panel;
    QString active = "border: 1px solid #4488ff;";
    QString inactive = "border: 1px solid #333;";
    m_leftPanel->setStyleSheet(panel == m_leftPanel ? active : inactive);
    m_rightPanel->setStyleSheet(panel == m_rightPanel ? active : inactive);
}

FilePanel *MainWindow::sourcePanel() const { return m_activePanel; }
FilePanel *MainWindow::destPanel() const {
    return m_activePanel == m_leftPanel ? m_rightPanel : m_leftPanel;
}

void MainWindow::log(const QString &msg) {
    m_logView->append(msg);
    m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
}

/* ── Keyboard handling ───────────────────────────────────────────── */

void MainWindow::keyPressEvent(QKeyEvent *event) {
    switch (event->key()) {
    case Qt::Key_F5:
        doCopy();
        break;
    case Qt::Key_F10:
        close();
        break;
    case Qt::Key_Tab:
        setActivePanel(m_activePanel == m_leftPanel ? m_rightPanel : m_leftPanel);
        m_activePanel->tableView()->setFocus();
        break;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

/* ── Copy operation (with detailed timing + progress bar) ─────────── */

void MainWindow::doCopy() {
    FilePanel *src = sourcePanel();
    FilePanel *dst = destPanel();
    if (!src || !dst) return;

    auto selected = src->selectedEntries();
    if (selected.isEmpty()) {
        log("⚠ No files selected for copy.");
        return;
    }

    if (dst->isInArchive()) {
        log("⚠ Cannot copy into an archive (read-only).");
        return;
    }
    QString destDir = dst->currentPath();

    log(QString("━━━ Copy %1 files to %2 ━━━").arg(selected.size()).arg(destDir));

    // Show progress bar
    m_progressBar->setMaximum(selected.size());
    m_progressBar->setValue(0);
    m_progressBar->setFormat("Copying... %v / %m");
    m_progressBar->setVisible(true);

    if (src->isInArchive()) {
        ArchiveReader *reader = src->archiveReader();
        if (!reader) { log("⚠ No archive reader!"); return; }

        // Build batch using tree index (instant, no scanning)
        QVector<ArchiveReader::BatchEntry> batch;
        QString archivePrefix = src->model()->archivePrefix();

        for (const auto &entry : selected) {
            if (entry.isDir) {
                // Use tree to collect all files under this directory
                QVector<FileListModel::FileCopyEntry> files;
                src->model()->collectFiles(entry.fullPath, files);
                log(QString("  📁 %1 → %2 files").arg(entry.fullPath).arg(files.size()));
                for (const auto &fc : files) {
                    QString relPath = fc.relativePath.mid(archivePrefix.length());
                    batch.append({fc.archiveIndex, destDir + "/" + relPath});
                }
            } else if (entry.archiveIndex >= 0) {
                batch.append({entry.archiveIndex, destDir + "/" + entry.name});
                log(QString("  📄 %1 (idx %2)").arg(entry.name).arg(entry.archiveIndex));
            }
        }
        if (batch.isEmpty()) { log("⚠ No extractable files found."); m_progressBar->setVisible(false); return; }

        log(QString("  Total files to extract: %1").arg(batch.size()));

        m_progressBar->setMaximum(0);  // indeterminate mode
        m_globalStatus->setText(QString("Extracting %1 files...").arg(batch.size()));
        m_progressBar->repaint();
        m_globalStatus->repaint();

        double totalT0 = nowUs();
        int threads = qMin(QThread::idealThreadCount(), 8);
        log(QString("  Using %1 threads for decompression").arg(threads));

        auto results = reader->extractBatch(batch, threads);
        double totalDt = (nowUs() - totalT0) / 1000.0;

        double totalDecomp = 0, totalWrite = 0;
        qint64 totalBytes = 0;
        int ok = 0, fail = 0;

        for (int i = 0; i < results.size(); i++) {
            const auto &r = results[i];
            QString name = QFileInfo(batch[i].destPath).fileName();
            if (r.ok) {
                log(QString("  ✓ %1 — %2 — decomp: %3 µs, write: %4 µs")
                    .arg(name, -40)
                    .arg(FileListModel::humanSize(r.size), 10)
                    .arg(r.decompUs, 0, 'f', 0)
                    .arg(r.writeUs, 0, 'f', 0));
                totalDecomp += r.decompUs;
                totalWrite  += r.writeUs;
                totalBytes  += r.size;
                ok++;
            } else {
                log(QString("  ✗ %1 — %2").arg(name).arg(r.error));
                fail++;
            }
        }

        log(QString("━━━ Done: %1 ok, %2 failed, %3 total ━━━")
            .arg(ok).arg(fail).arg(FileListModel::humanSize(totalBytes)));
        log(QString("  Total decompress: %1 ms | Total write: %2 ms | Wall: %3 ms")
            .arg(totalDecomp / 1000.0, 0, 'f', 2)
            .arg(totalWrite / 1000.0, 0, 'f', 2)
            .arg(totalDt, 0, 'f', 2));
        if (totalDt > 0) {
            double throughput = (totalBytes / (1024.0*1024.0)) / (totalDt / 1000.0);
            log(QString("  Throughput: %1 MB/s").arg(throughput, 0, 'f', 1));
        }

        disconnect(reader, &ArchiveReader::batchProgress, this, nullptr);
        src->model()->clearMarks();
        dst->navigateTo(destDir);

    } else {
        int ok = 0, fail = 0, i = 0;
        for (const auto &entry : selected) {
            if (entry.isDir) continue;
            QString destPath = destDir + "/" + entry.name;

            double t0 = nowUs();
            bool copied = QFile::copy(entry.fullPath, destPath);
            double dt = (nowUs() - t0) / 1000.0;

            m_progressBar->setValue(++i);
            m_progressBar->setFormat(
                QString("Copying %1/%2: %3").arg(i).arg(selected.size()).arg(entry.name));
            QApplication::processEvents();

            if (copied) {
                log(QString("  ✓ %1 — %2 ms").arg(entry.name).arg(dt, 0, 'f', 2));
                ok++;
            } else {
                log(QString("  ✗ %1 — copy failed").arg(entry.name));
                fail++;
            }
        }
        log(QString("━━━ FS Copy: %1 ok, %2 failed ━━━").arg(ok).arg(fail));
        dst->navigateTo(destDir);
    }

    m_progressBar->setVisible(false);
    m_globalStatus->setText("Ready");
}
