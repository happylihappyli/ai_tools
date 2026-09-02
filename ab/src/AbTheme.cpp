// AbTheme.cpp — 应用 dark/light/solarized/nord QSS
// 2026-09-02: 加 solarized (深棕米黄) + nord (深蓝冷色) 主题, 4 选 1
//            修复 current() 用 QJsonDocument 解析 (原来 contains 太脆弱)
//            加 refreshAllWidgets() 强制刷新 (setStyleSheet 后不自动 polish)
#include "AbTheme.h"
#include <QApplication>
#include <QFile>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QWidget>
#include <QStyle>            // 2026-09-02: QWidget::style() 返回 QStyle*, 需要此头
#include <QJsonDocument>
#include <QJsonObject>

namespace ab {

// =====================================================================
// Dark (default) - VS Code 风格深色
// =====================================================================
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
QStatusBar { background: #1e1e1e; color: #d4d4d4; border-top: 1px solid #3c3c3c; }
QStatusBar QLabel { color: #d4d4d4; background: transparent; padding: 4px 10px; }
QStatusBar QLabel[level="ok"]   { background: #1e3a1e; color: #6a9955; font-weight: bold; }
QStatusBar QLabel[level="err"]  { background: #3a1e1e; color: #f48771; font-weight: bold; }
QStatusBar QLabel[level="warn"] { background: #3a2e1e; color: #dcdcaa; font-weight: bold; }
QStatusBar QLabel[level="info"] { background: #1e2a3a; color: #569cd6; font-weight: bold; }
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
QMessageBox { background: #252526; color: #d4d4d4; }
QMessageBox QLabel { color: #d4d4d4; background: transparent; }
)";

// =====================================================================
// Light - 浅色
// =====================================================================
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
QStatusBar { background: #fafafa; color: #212121; border-top: 1px solid #e0e0e0; }
QStatusBar QLabel { color: #212121; background: transparent; padding: 4px 10px; }
QStatusBar QLabel[level="ok"]   { background: #e8f5e9; color: #2e7d32; font-weight: bold; }
QStatusBar QLabel[level="err"]  { background: #ffebee; color: #c62828; font-weight: bold; }
QStatusBar QLabel[level="warn"] { background: #fff3e0; color: #ef6c00; font-weight: bold; }
QStatusBar QLabel[level="info"] { background: #e3f2fd; color: #1565c0; font-weight: bold; }
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
QMessageBox { background: #fafafa; color: #212121; }
QMessageBox QLabel { color: #212121; background: transparent; }
)";

// =====================================================================
// Solarized Dark - 经典深棕米黄 (Ethan Schoonover 设计, 护眼)
// =====================================================================
static const char* SOLARIZED_QSS = R"(
QMainWindow, QDialog { background: #002b36; color: #93a1a1; }
QWidget { background: #002b36; color: #93a1a1; }
QMenuBar { background: #073642; color: #93a1a1; border-bottom: 1px solid #586e75; padding: 2px; }
QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 4px; }
QMenuBar::item:selected { background: #b58900; color: #002b36; }
QMenu { background: #073642; color: #93a1a1; border: 1px solid #586e75; padding: 4px; }
QMenu::item { padding: 6px 24px; border-radius: 4px; }
QMenu::item:selected { background: #b58900; color: #002b36; }
QMenu::separator { height: 1px; background: #586e75; margin: 4px 8px; }
QToolBar { background: #073642; border: 1px solid #586e75; padding: 4px; spacing: 4px; }
QToolButton { background: transparent; color: #93a1a1; border: 1px solid transparent; padding: 5px 10px; border-radius: 4px; }
QToolButton:hover { background: #b58900; color: #002b36; }
QStatusBar { background: #002b36; color: #93a1a1; border-top: 1px solid #586e75; }
QStatusBar QLabel { color: #93a1a1; background: transparent; padding: 4px 10px; }
QStatusBar QLabel[level="ok"]   { background: #073642; color: #859900; font-weight: bold; }
QStatusBar QLabel[level="err"]  { background: #073642; color: #dc322f; font-weight: bold; }
QStatusBar QLabel[level="warn"] { background: #073642; color: #b58900; font-weight: bold; }
QStatusBar QLabel[level="info"] { background: #073642; color: #268bd2; font-weight: bold; }
QPushButton { background: #073642; color: #93a1a1; border: 1px solid #586e75; padding: 6px 14px; border-radius: 4px; min-height: 18px; }
QPushButton:hover  { background: #b58900; color: #002b36; border-color: #b58900; }
QPushButton:pressed { background: #cb4b16; color: #002b36; }
QPushButton:disabled { background: #002b36; color: #586e75; }
QPushButton[role="primary"]  { background: #268bd2; color: #fdf6e3; border-color: #268bd2; }
QPushButton[role="success"]  { background: #859900; color: #fdf6e3; border-color: #859900; }
QPushButton[role="warning"]  { background: #b58900; color: #002b36; border-color: #b58900; }
QPushButton[role="danger"]   { background: #dc322f; color: #fdf6e3; border-color: #dc322f; }
QLineEdit, QTextEdit, QPlainTextEdit { background: #002b36; color: #93a1a1; border: 1px solid #586e75; padding: 4px 6px; border-radius: 3px; selection-background-color: #586e75; }
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus { border: 1px solid #b58900; }
QLabel { color: #93a1a1; background: transparent; }
QGroupBox { background: #073642; color: #eee8d5; border: 1px solid #586e75; border-radius: 5px; margin-top: 12px; padding: 8px; font-weight: bold; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 6px; color: #b58900; }
QFrame { background: #073642; border: 1px solid #586e75; border-radius: 5px; }
QTreeWidget, QTreeView, QListWidget { background: #002b36; color: #93a1a1; alternate-background-color: #073642; border: 1px solid #586e75; selection-background-color: #586e75; selection-color: #fdf6e3; }
QHeaderView::section { background: #073642; color: #eee8d5; padding: 4px 8px; border: 1px solid #586e75; font-weight: bold; }
QProgressBar { background: #073642; color: #93a1a1; border: 1px solid #586e75; border-radius: 4px; text-align: center; min-height: 18px; }
QProgressBar::chunk { background: #b58900; border-radius: 3px; }
QDockWidget::title { background: #073642; color: #eee8d5; padding: 4px 8px; border: 1px solid #586e75; font-weight: bold; }
QScrollBar:vertical { background: #002b36; width: 12px; border: none; }
QScrollBar::handle:vertical { background: #586e75; border-radius: 6px; min-height: 20px; margin: 2px; }
QScrollBar:horizontal { background: #002b36; height: 12px; border: none; }
QScrollBar::handle:horizontal { background: #586e75; border-radius: 6px; min-width: 20px; margin: 2px; }
QToolTip { background: #073642; color: #b58900; border: 1px solid #b58900; padding: 4px 8px; border-radius: 3px; font-weight: bold; }
QMessageBox { background: #002b36; color: #93a1a1; }
QMessageBox QLabel { color: #93a1a1; background: transparent; }
)";

// =====================================================================
// Nord - 北欧冷色蓝灰
// =====================================================================
static const char* NORD_QSS = R"(
QMainWindow, QDialog { background: #2e3440; color: #d8dee9; }
QWidget { background: #2e3440; color: #d8dee9; }
QMenuBar { background: #3b4252; color: #d8dee9; border-bottom: 1px solid #4c566a; padding: 2px; }
QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 4px; }
QMenuBar::item:selected { background: #88c0d0; color: #2e3440; }
QMenu { background: #3b4252; color: #d8dee9; border: 1px solid #4c566a; padding: 4px; }
QMenu::item { padding: 6px 24px; border-radius: 4px; }
QMenu::item:selected { background: #88c0d0; color: #2e3440; }
QMenu::separator { height: 1px; background: #4c566a; margin: 4px 8px; }
QToolBar { background: #3b4252; border: 1px solid #4c566a; padding: 4px; spacing: 4px; }
QToolButton { background: transparent; color: #d8dee9; border: 1px solid transparent; padding: 5px 10px; border-radius: 4px; }
QToolButton:hover { background: #88c0d0; color: #2e3440; }
QStatusBar { background: #2e3440; color: #d8dee9; border-top: 1px solid #4c566a; }
QStatusBar QLabel { color: #d8dee9; background: transparent; padding: 4px 10px; }
QStatusBar QLabel[level="ok"]   { background: #3b4252; color: #a3be8c; font-weight: bold; }
QStatusBar QLabel[level="err"]  { background: #3b4252; color: #bf616a; font-weight: bold; }
QStatusBar QLabel[level="warn"] { background: #3b4252; color: #ebcb8b; font-weight: bold; }
QStatusBar QLabel[level="info"] { background: #3b4252; color: #88c0d0; font-weight: bold; }
QPushButton { background: #3b4252; color: #d8dee9; border: 1px solid #4c566a; padding: 6px 14px; border-radius: 4px; min-height: 18px; }
QPushButton:hover  { background: #88c0d0; color: #2e3440; border-color: #88c0d0; }
QPushButton:pressed { background: #81a1c1; color: #2e3440; }
QPushButton:disabled { background: #2e3440; color: #4c566a; }
QPushButton[role="primary"]  { background: #5e81ac; color: #eceff4; border-color: #5e81ac; }
QPushButton[role="success"]  { background: #a3be8c; color: #2e3440; border-color: #a3be8c; }
QPushButton[role="warning"]  { background: #ebcb8b; color: #2e3440; border-color: #ebcb8b; }
QPushButton[role="danger"]   { background: #bf616a; color: #eceff4; border-color: #bf616a; }
QLineEdit, QTextEdit, QPlainTextEdit { background: #2e3440; color: #d8dee9; border: 1px solid #4c566a; padding: 4px 6px; border-radius: 3px; selection-background-color: #4c566a; }
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus { border: 1px solid #88c0d0; }
QLabel { color: #d8dee9; background: transparent; }
QGroupBox { background: #3b4252; color: #eceff4; border: 1px solid #4c566a; border-radius: 5px; margin-top: 12px; padding: 8px; font-weight: bold; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 6px; color: #88c0d0; }
QFrame { background: #3b4252; border: 1px solid #4c566a; border-radius: 5px; }
QTreeWidget, QTreeView, QListWidget { background: #2e3440; color: #d8dee9; alternate-background-color: #3b4252; border: 1px solid #4c566a; selection-background-color: #4c566a; selection-color: #eceff4; }
QHeaderView::section { background: #3b4252; color: #eceff4; padding: 4px 8px; border: 1px solid #4c566a; font-weight: bold; }
QProgressBar { background: #3b4252; color: #d8dee9; border: 1px solid #4c566a; border-radius: 4px; text-align: center; min-height: 18px; }
QProgressBar::chunk { background: #88c0d0; border-radius: 3px; }
QDockWidget::title { background: #3b4252; color: #eceff4; padding: 4px 8px; border: 1px solid #4c566a; font-weight: bold; }
QScrollBar:vertical { background: #2e3440; width: 12px; border: none; }
QScrollBar::handle:vertical { background: #4c566a; border-radius: 6px; min-height: 20px; margin: 2px; }
QScrollBar:horizontal { background: #2e3440; height: 12px; border: none; }
QScrollBar::handle:horizontal { background: #4c566a; border-radius: 6px; min-width: 20px; margin: 2px; }
QToolTip { background: #3b4252; color: #ebcb8b; border: 1px solid #88c0d0; padding: 4px 8px; border-radius: 3px; font-weight: bold; }
QMessageBox { background: #2e3440; color: #d8dee9; }
QMessageBox QLabel { color: #d8dee9; background: transparent; }
)";

static QString themeConfigPath() {
    QStringList p = QStandardPaths::standardLocations(QStandardPaths::ConfigLocation);
    if (!p.isEmpty()) {
        return p.first() + "/ai_tools/theme.json";
    }
    return QDir::homePath() + "/.ai_tools_theme.json";
}

QString AbTheme::displayName(int kind) {
    switch (kind) {
        case Dark:      return "🌑 暗色 (默认)";
        case Light:     return "☀️ 浅色";
        case Solarized: return "🟤 Solarized 暗色";
        case Nord:      return "❄️ Nord 冷色";
    }
    return "未知";
}

QString AbTheme::shortName(int kind) {
    switch (kind) {
        case Dark:      return "dark";
        case Light:     return "light";
        case Solarized: return "solarized";
        case Nord:      return "nord";
    }
    return "dark";
}

AbTheme::Kind AbTheme::parse(const QString& name) {
    QString n = name.trimmed().toLower();
    if (n == "light")     return Light;
    if (n == "solarized") return Solarized;
    if (n == "nord")      return Nord;
    return Dark;
}

AbTheme::Kind AbTheme::current() {
    QFile f(themeConfigPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return Dark;
    QByteArray data = f.readAll();
    f.close();
    // 2026-09-02: 用 QJsonDocument 解析, 不再 contains 字符串
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return Dark;
    return parse(doc.object().value("theme").toString());
}

void AbTheme::save(Kind kind) {
    QFileInfo fi(themeConfigPath());
    QDir().mkpath(fi.absolutePath());
    QFile f(themeConfigPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QJsonObject o;
    o["theme"] = shortName(kind);
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

void AbTheme::refreshAllWidgets() {
    // 2026-09-02: 强制刷新所有 widget 的 QSS (setStyleSheet 后不自动 polish)
    QApplication* app = qApp;
    if (!app) return;
    for (QWidget* w : app->allWidgets()) {
        if (!w) continue;
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
    }
}

QString AbTheme::embeddedQssForTest(int kind) {
    // 2026-09-02: 纯内嵌 QSS 返回, 不读文件, 不 save. 用于 --doctor dry-run 验证.
    switch (kind) {
        case Light:     return QString::fromUtf8(LIGHT_QSS);
        case Solarized: return QString::fromUtf8(SOLARIZED_QSS);
        case Nord:      return QString::fromUtf8(NORD_QSS);
        case Dark:
        default:        return QString::fromUtf8(DARK_QSS);
    }
}

void AbTheme::apply(int kind) {
    if (kind < 0 || kind >= NumThemes) kind = Dark;
    QApplication* app = qApp;
    if (!app) return;

    // 优先用 QSS 文件 (ui/ab_<name>.qss, 跟 binary 同目录)
    // 2026-09-02: 文件不存在 OR 内容是空/注释 (placeholder), 都 fallback 到内嵌 QSS
    QString exeDir = QCoreApplication::applicationDirPath();
    QString name = shortName(kind);
    QString qssFile = QString("%1/ab_%2.qss").arg(exeDir).arg(name);
    QFile f(qssFile);
    QString qss;
    bool fileLoaded = false;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qss = QString::fromUtf8(f.readAll());
        // 检测是否是有效 QSS: 至少含一个 "{" 或者是 Qt 关键字
        // 占位文件 (e.g. "/* ab_dark.qss placeholder */") 不含 "{"
        if (qss.contains('{')) {
            fileLoaded = true;
        }
    }
    if (!fileLoaded) {
        switch (kind) {
            case Light:     qss = QString::fromUtf8(LIGHT_QSS); break;
            case Solarized: qss = QString::fromUtf8(SOLARIZED_QSS); break;
            case Nord:      qss = QString::fromUtf8(NORD_QSS); break;
            case Dark:
            default:        qss = QString::fromUtf8(DARK_QSS); break;
        }
    }
    app->setStyleSheet(qss);
    save(static_cast<Kind>(kind));
    refreshAllWidgets();
}

}  // namespace ab
