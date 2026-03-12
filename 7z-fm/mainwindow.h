#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include "filepanel.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void doCopy();

private:
    FilePanel    *m_leftPanel;
    FilePanel    *m_rightPanel;
    FilePanel    *m_activePanel = nullptr;
    QLabel       *m_globalStatus;
    QTextEdit    *m_logView;
    QProgressBar *m_progressBar;
    QWidget      *m_toolbar;

    void log(const QString &msg);
    FilePanel *sourcePanel() const;
    FilePanel *destPanel()   const;
    void setActivePanel(FilePanel *panel);
    QWidget *createToolbar();
};
