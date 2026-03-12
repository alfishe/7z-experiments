#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QFont>
#include "mainwindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("7z-fm");
    app.setStyle(QStyleFactory::create("Fusion"));

    // Dark palette
    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(0x0f, 0x0f, 0x23));
    pal.setColor(QPalette::WindowText,      QColor(0xe0, 0xe0, 0xe0));
    pal.setColor(QPalette::Base,            QColor(0x0f, 0x0f, 0x23));
    pal.setColor(QPalette::AlternateBase,   QColor(0x14, 0x14, 0x28));
    pal.setColor(QPalette::ToolTipBase,     QColor(0x1a, 0x1a, 0x2e));
    pal.setColor(QPalette::ToolTipText,     QColor(0xe0, 0xe0, 0xe0));
    pal.setColor(QPalette::Text,            QColor(0xe0, 0xe0, 0xe0));
    pal.setColor(QPalette::Button,          QColor(0x1a, 0x1a, 0x2e));
    pal.setColor(QPalette::ButtonText,      QColor(0xe0, 0xe0, 0xe0));
    pal.setColor(QPalette::BrightText,      QColor(0x00, 0xff, 0x00));
    pal.setColor(QPalette::Link,            QColor(0x44, 0x88, 0xff));
    pal.setColor(QPalette::Highlight,       QColor(0x1e, 0x3a, 0x5f));
    pal.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    app.setPalette(pal);

    // Monospace font
    QFont font("SF Mono", 12);
    font.setStyleHint(QFont::Monospace);
    app.setFont(font);

    MainWindow w;
    w.show();
    return app.exec();
}
