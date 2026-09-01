// AbTheme.cpp — 应用 dark/light QSS
#include "AbTheme.h"
#include <QApplication>
#include <QFile>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>

namespace ab {

// 内嵌 fallback QSS (dark) — 跟 Python ac_themes.py 一致
static const char* DARK_QSS = R"(
QMainWindow, QDialog { background: #1e1e1e; color: #d4d4d4; }
QWidget { background: #1e1e1e; color: #d4d4d4; }
QMenuBar { background: #252526; color: #d4d4d4; border-bottom: 1px solid #3c3c3c; padding: 2px; }
QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 4px; }
QMenuBar::item:selected { background: #569cd6; color: white; }
QMenu { background: #252526; color: #d4d4d4; border: 1px solid #3c3c3c; padding: 4px; }
QMenu::item { padding: 6px 24px; border-radius: 4px; }
QMenu::item:selected { background: #569cd6; color: white; }
QMenu::separator { height: 1px; background: #3c3c3c; margin: 4px 8px; }
QToolBar { background: #252526; border: 1px solid #3c3c3c; padding: 4px; spacing: 4px; }
QToolButton { background: transparent; color: #d4d4d4; border: 1px solid transparent; padding: 5px 10px; border-radius: 4px; }
QToolButton:hover { background: #569cd6; color: white; }
QStatusBar { background: #3a3520; color: #ffe082; border-top: 1px solid #3c3c3c; font-weight: bold; }
QStatusBar QLabel { color: #ffe082; background: #3a3520; padding: 4px 10px; font-weight: bold; }
QStatusBar QLabel[level="ok"]   { background: #1e3a1e; color: #6a9955; }
QStatusBar QLabel[level="err"]  { background: #3a1e1e; color: #f48771; }
QStatusBar QLabel[level="warn"] { background: #3a2e1e; color: #dcdcaa; }
QStatusBar QLabel[level="info"] { background: #3a3520; color: #569cd6; }
QPushButton { background: #252526; color: #d4d4d4; border: 1px solid #3c3c3c; padding: 6px 14px; border-radius: 4px; min-height: 18px; }
QPushButton:hover  { background: #569cd6; color: white; border-color: #569cd6; }
QPushButton:pressed { background: #4ec9b0; }
QPushButton:disabled { background: #3c3c3c; color: #858585; }
QPushButton[role="primary"]  { background: #1565c0; color: white; border-color: #1565c0; }
QPushButton[role="success"]  { background: #2e7d32; color: white; border-color: #2e7d32; }
QPushButton[role="warning"]  { background: #ef6c00; color: white; border-color: #ef6c00; }
QPushButton[role="danger"]   { background: #c62828; color: white; border-color: #c62828; }
QLineEdit, QTextEdit, QPlainTextEdit { background: #1e1e1e; color: #d4d4d4; border: 1px solid #3c3c3c; padding: 4px 6px; border-radius: 3px; selection-background-color: #264f78; }
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus { border: 1px solid #569cd6; }
QLabel { color: #d4d4d4; background: transparent; }
QGroupBox { background: #2d2d30; color: #cccccc; border: 1px solid #3c3c3c; border-radius: 5px; margin-top: 12px; padding: 8px; font-weight: bold; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 6px; color: #569cd6; }
QFrame { background: #2d2d30; border: 1px solid #3c3c3c; border-radius: 5px; }
QTreeWidget, QTreeView, QListWidget { background: #1e1e1e; color: #d4d4d4; alternate-background-color: #252526; border: 1px solid #3c3c3c; selection-background-color: #264f78; selection-color: #d4d4d4; }
QHeaderView::section { background: #252526; color: #cccccc; padding: 4px 8px; border: 1px solid #3c3c3c; font-weight: bold; }
QProgressBar { background: #252526; color: #d4d4d4; border: 1px solid #3c3c3c; border-radius: 4px; text-align: center; min-height: 18px; }
QProgressBar::chunk { background: #569cd6; border-radius: 3px; }
QDockWidget::title { background: #252526; color: #cccccc; padding: 4px 8px; border: 1px solid #3c3c3c; font-weight: bold; }
QScrollBar:vertical { background: #252526; width: 12px; border: none; }
QScrollBar::handle:vertical { background: #3c3c3c; border-radius: 6px; min-height: 20px; margin: 2px; }
QScrollBar:horizontal { background: #252526; height: 12px; border: none; }
QScrollBar::handle:horizontal { background: #3c3c3c; border-radius: 6px; min-width: 20px; margin: 2px; }
QToolTip { background: #3a3520; color: #ffe082; border: 1px solid #569cd6; padding: 4px 8px; border-radius: 3px; font-weight: bold; }
QMessageBox { background: #3a3520; color: #ffe082; }
QMessageBox QLabel { color: #ffe082; background: transparent; font-weight: bold; }
)";

static const char* LIGHT_QSS = R"(
QMainWindow, QDialog { background: #ffffff; color: #212121; }
QWidget { background: #ffffff; color: #212121; }
QMenuBar { background: #f5f5f5; color: #212121; border-bottom: 1px solid #e0e0e0; padding: 2px; }
QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 4px; }
QMenuBar::item:selected { background: #1565c0; color: white; }
QMenu { background: #f5f5f5; color: #212121; border: 1px solid #e0e0e0; padding: 4px; }
QMenu::item { padding: 6px 24px; border-radius: 4px; }
QMenu::item:selected { background: #1565c0; color: white; }
QMenu::separator { height: 1px; background: #e0e0e0; margin: 4px 8px; }
QToolBar { background: #f5f5f5; border: 1px solid #e0e0e0; padding: 4px; spacing: 4px; }
QToolButton { background: transparent; color: #212121; border: 1px solid transparent; padding: 5px 10px; border-radius: 4px; }
QToolButton:hover { background: #1565c0; color: white; }
QStatusBar { background: #fff8e1; color: #5d4037; border-top: 1px solid #e0e0e0; font-weight: bold; }
QStatusBar QLabel { color: #5d4037; background: #fff8e1; padding: 4px 10px; font-weight: bold; }
QStatusBar QLabel[level="ok"]   { background: #e8f5e9; color: #2e7d32; }
QStatusBar QLabel[level="err"]  { background: #ffebee; color: #c62828; }
QStatusBar QLabel[level="warn"] { background: #fff3e0; color: #ef6c00; }
QStatusBar QLabel[level="info"] { background: #fff8e1; color: #1565c0; }
QPushButton { background: #f5f5f5; color: #212121; border: 1px solid #e0e0e0; padding: 6px 14px; border-radius: 4px; min-height: 18px; }
QPushButton:hover  { background: #1565c0; color: white; border-color: #1565c0; }
QPushButton:pressed { background: #1976d2; }
QPushButton:disabled { background: #e0e0e0; color: #757575; }
QPushButton[role="primary"]  { background: #1565c0; color: white; border-color: #1565c0; }
QPushButton[role="success"]  { background: #2e7d32; color: white; border-color: #2e7d32; }
QPushButton[role="warning"]  { background: #ef6c00; color: white; border-color: #ef6c00; }
QPushButton[role="danger"]   { background: #c62828; color: white; border-color: #c62828; }
QLineEdit, QTextEdit, QPlainTextEdit { background: #ffffff; color: #212121; border: 1px solid #e0e0e0; padding: 4px 6px; border-radius: 3px; selection-background-color: #bbdefb; }
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus { border: 1px solid #1565c0; }
QLabel { color: #212121; background: transparent; }
QGroupBox { background: #fafafa; color: #424242; border: 1px solid #e0e0e0; border-radius: 5px; margin-top: 12px; padding: 8px; font-weight: bold; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 6px; color: #1565c0; }
QFrame { background: #fafafa; border: 1px solid #e0e0e0; border-radius: 5px; }
QTreeWidget, QTreeView, QListWidget { background: #ffffff; color: #212121; alternate-background-color: #f5f5f5; border: 1px solid #e0e0e0; selection-background-color: #bbdefb; selection-color: #212121; }
QHeaderView::section { background: #f5f5f5; color: #424242; padding: 4px 8px; border: 1px solid #e0e0e0; font-weight: bold; }
QProgressBar { background: #f5f5f5; color: #212121; border: 1px solid #e0e0e0; border-radius: 4px; text-align: center; min-height: 18px; }
QProgressBar::chunk { background: #1565c0; border-radius: 3px; }
QDockWidget::title { background: #f5f5f5; color: #424242; padding: 4px 8px; border: 1px solid #e0e0e0; font-weight: bold; }
QScrollBar:vertical { background: #f5f5f5; width: 12px; border: none; }
QScrollBar::handle:vertical { background: #e0e0e0; border-radius: 6px; min-height: 20px; margin: 2px; }
QScrollBar:horizontal { background: #f5f5f5; height: 12px; border: none; }
QScrollBar::handle:horizontal { background: #e0e0e0; border-radius: 6px; min-width: 20px; margin: 2px; }
QToolTip { background: #fff8e1; color: #5d4037; border: 1px solid #1565c0; padding: 4px 8px; border-radius: 3px; font-weight: bold; }
QMessageBox { background: #fff8e1; color: #5d4037; }
QMessageBox QLabel { color: #5d4037; background: transparent; font-weight: bold; }
)";

void AbTheme::apply(int kind) {
    QApplication* app = qApp;
    if (!app) return;
    // 优先用 QSS 文件 (ui/ab_*.qss, 跟 binary 同目录)
    QString exeDir = QCoreApplication::applicationDirPath();
    QString qssFile = QString("%1/ab_%2.qss")
        .arg(exeDir)
        .arg(kind == Dark ? "dark" : "light");
    QFile f(qssFile);
    QString qss;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qss = QString::fromUtf8(f.readAll());
    } else {
        qss = QString::fromUtf8(kind == Dark ? DARK_QSS : LIGHT_QSS);
    }
    app->setStyleSheet(qss);
    save(static_cast<Kind>(kind));
}

AbTheme::Kind AbTheme::parse(const QString& name) {
    if (name.compare("light", Qt::CaseInsensitive) == 0) return Light;
    return Dark;
}

static QString themeConfigPath() {
    QStringList p = QStandardPaths::standardLocations(QStandardPaths::ConfigLocation);
    if (!p.isEmpty()) {
        return p.first() + "/ai_tools/theme.json";
    }
    return QDir::homePath() + "/.ai_tools_theme.json";
}

AbTheme::Kind AbTheme::current() {
    QFile f(themeConfigPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return Dark;
    QByteArray data = f.readAll();
    f.close();
    if (data.contains("\"theme\":\"light\"")) return Light;
    return Dark;
}

void AbTheme::save(Kind kind) {
    QFileInfo fi(themeConfigPath());
    QDir().mkpath(fi.absolutePath());
    QFile f(themeConfigPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(QString("{\"theme\":\"%1\"}\n").arg(kind == Dark ? "dark" : "light").toUtf8());
}

}  // namespace ab
