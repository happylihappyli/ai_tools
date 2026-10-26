// ab — AI Build 编译/调试 GUI (C++ Qt5/6)
// 用法:
//   ab                          # 自动在 cwd 找 ai_build.json
//   ab /path/to/project         # 指定项目 (找 ai_build.json)
//   ab cloud_main               # 找 ai_build.cloud_main.json (2026-09-18 加)
//   ab cloud_main_qt            # 找 ai_build.cloud_main_qt.json (Qt dock 测试)
//   ab --project /path/to/proj
//   ab --config ai_build.json   # 指定配置文件
//   ab --no-auto                # 不自动跑 auto 链
//   ab --auto-exit              # 2026-10-09 加 — auto 链全部完成后自动退出 (适合 AI/脚本调用)
//   ab --doctor                 # 环境自检
//   ab --theme dark|light|solarized|nord  # 强制主题 (2026-09-02: 加 solarized + nord)
//   ab --help                   # 帮助

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QIcon>
#include <QFont>
#include <QPen>
#include <QBrush>
#include <QLinearGradient>
#include <iostream>
#include "AbConfig.h"
#include "AbTheme.h"
#include "AbLang.h"      // 2026-10-26: 语言管理 (zh 默认 / en 可切, 存 lang.json)
#include "AbMainWindow.h"

using namespace ab;

// 2026-10-15: 程序化生成 ab 图标 (任务栏/窗口标题栏用)
//   64x64, 深底 + 亮蓝渐变 + 白色 "ab" 文字 + 一个对角小扳手 (识别度高)
static QPixmap makeAbIcon() {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 圆角矩形底, 深蓝→亮蓝渐变
    QLinearGradient g(0, 0, 64, 64);
    g.setColorAt(0.0, QColor("#1e3a5f"));
    g.setColorAt(1.0, QColor("#4a90e2"));
    p.setBrush(QBrush(g));
    p.setPen(QPen(QColor("#0a1828"), 2));
    p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);

    // 左上角小三角装饰 (代表"编译输出")
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#ffcc44"));
    QPolygonF tri;
    tri << QPointF(8, 8) << QPointF(20, 8) << QPointF(8, 20);
    p.drawPolygon(tri);

    // 中间 "ab" 文字
    p.setPen(QColor("#ffffff"));
    QFont f = p.font();
    f.setBold(true);
    f.setPointSize(26);
    p.setFont(f);
    p.drawText(QRectF(0, 14, 64, 42), Qt::AlignCenter, "ab");

    // 右下小扳手
    p.setPen(QPen(QColor("#88ff88"), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(48, 48), 6, 6);
    p.drawLine(48, 42, 48, 38);

    p.end();
    return pm;
}

static void printHelp() {
    std::cout
        << "ab — AI Build 编译/调试 GUI (C++ Qt5/6)\n"
        << "===========================================\n"
        << "\n"
        << "通用 AI 编译启动器, 通过 ai_build.json 配置 UI.\n"
        << "按钮/菜单/工具栏都能在 ai_build.json 的 ui 段配置.\n"
        << "\n"
        << "用法:\n"
        << "  ab                              # 当前目录找 ai_build.json\n"
        << "  ab /path/to/project             # 指定项目\n"
        << "  ab <name>                       # 命名配置 (ai_build.<name>.json) — 2026-09-18 加\n"
        << "    例: ab cloud_main_qt          # → ai_build.cloud_main_qt.json (聚焦 dock 测试)\n"
        << "        ab cloud_main             # → ai_build.cloud_main.json (聚焦 cloud_main 编译)\n"
        << "        ab both_3d                # → ai_build.both_3d.json (cloud_main + qtdock 链路)\n"
        << "  ab --config /path/to/ai_build.json\n"
        << "  ab --no-auto                    # 不自动跑 auto 链\n"
        << "  ab --auto-exit                  # auto 链全部完成后自动退出 (AI/脚本调用)\n"
        << "  ab --doctor                     # 环境自检\n"
        << "  ab --theme dark|light|solarized|nord  # 强制主题\n"
        << "  ab --lang zh|en                       # 强制语言 (默认中文, 存 lang.json)\n"
        << "  ab --help                       # 帮助\n"
        << "\n"
        << "task cmd 规则 (重要!):\n"
        << "  cmd 字段禁止 | tail / | head / | grep / | less 等 pipe. pipe 子进程会缓冲,\n"
        << "  ab 实时 log 区看不到任何东西. 直接写 cmd, ab 走 QProcess 实时读 stdout/stderr.\n"
        << "  例: ❌ 'scons ... 2>&1 | tail -30'  ✅ 'scons ...'\n"
        << "  例外: 'tail -f log' 允许 (持续输出); 真要截行用 'cmd > log 2>&1; echo exit=$?'\n"
        << "\n"
        << "ai_build.json 示例 ui 段:\n"
        << "  {\n"
        << "    \"tasks\": [ ... ],\n"
        << "    \"ui\": {\n"
        << "      \"title\": \"我的项目\",\n"
        << "      \"theme\": \"dark\",\n"
        << "      \"auto_start\": true,\n"
        << "      \"menus\": [ { \"name\": \"文件(&F)\", \"items\": [ ... ] } ],\n"
        << "      \"toolbar\": [ { \"id\": \"build\", \"label\": \"⚡ 编译\", \"task\": \"build+deploy\" } ],\n"
        << "      \"buttons\": [ { \"id\": \"run\",  \"label\": \"🚀 启动\", \"color\": \"success\" } ],\n"
        << "      \"run_after_build\": { \"binary_path\": \"bin/Debug/cloud_main\", \"auto_run\": false }\n"
        << "    }\n"
        << "  }\n";
}

static int runDoctor() {
    std::cout << "===== ab — 环境自检 =====\n\n";
    std::cout << "[1] 显示环境\n";
    std::cout << "  DISPLAY=" << (qgetenv("DISPLAY").isEmpty() ? "(未设)" : qgetenv("DISPLAY").constData()) << "\n";
    std::cout << "  WAYLAND_DISPLAY=" << (qgetenv("WAYLAND_DISPLAY").isEmpty() ? "(未设)" : qgetenv("WAYLAND_DISPLAY").constData()) << "\n";
    std::cout << "  XDG_SESSION_TYPE=" << (qgetenv("XDG_SESSION_TYPE").isEmpty() ? "(未设)" : qgetenv("XDG_SESSION_TYPE").constData()) << "\n";

    std::cout << "\n[2] Qt\n";
    std::cout << "  QT_VERSION=" << qVersion() << "\n";
    std::cout << "  platform=" << QApplication::platformName().toStdString() << "\n";

    std::cout << "\n[3] 当前主题 (theme.json): "
        << AbTheme::shortName(AbTheme::current()).toStdString() << "\n";

    // 2026-09-02: 4 主题 QSS dry-run 验证, 不写 theme.json
    std::cout << "\n[4] 内嵌 QSS 验证 (4 主题 dry-run, 不写 theme.json):\n";
    for (int k = 0; k < AbTheme::NumThemes; ++k) {
        QString name = AbTheme::shortName(k);
        QString qss = AbTheme::embeddedQssForTest(k);
        QString firstLine = qss.section('\n', 1, 1).trimmed();
        std::cout << "  " << name.toStdString()
                  << "  len=" << qss.length()
                  << "  starts=\"" << firstLine.toStdString() << "\"\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ab");
    QApplication::setApplicationVersion("1.0.0");
    QApplication::setOrganizationName("ai_tools");
    // 2026-10-09 fix-auto-exit-debug: 默认 Qt 在"最后一个窗口关掉"时退出 app.
    //   AI 调用场景: 副窗口 (inspector/task_runner) 被关时不能让 main 退 — auto 链跑完后
    //   才用 cfg_.auto_exit 主动 quit. 之前没显式设, 副窗口关掉 → main 跟着退 (就是上次
    //   用户看到 "auto 链没退但 ab 退" 的根因).
    QApplication::setQuitOnLastWindowClosed(false);

    // 解析参数 (简化)
    QString configPath;
    bool noAuto = false;
    bool doctorMode = false;
    bool autoExit = false;  // 2026-10-09 加 — auto 链完成后自动退出
    QString forcedTheme;
    QString forcedLang;  // 2026-10-26 加 — 强制语言 (--lang=zh|en), 覆盖 lang.json
    for (int i = 1; i < argc; ++i) {
        QString a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        if (a == "--doctor") doctorMode = true;
        else if (a == "--no-auto") noAuto = true;
        else if (a == "--auto-exit") autoExit = true;  // 2026-10-09 加
        else if (a.startsWith("--config=")) configPath = a.mid(9);
        else if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (a.startsWith("--theme=")) forcedTheme = a.mid(8);
        else if (a == "--theme" && i + 1 < argc) forcedTheme = argv[++i];
        else if (a.startsWith("--lang=")) forcedLang = a.mid(7);  // 2026-10-26 加
        else if (a == "--lang" && i + 1 < argc) forcedLang = argv[++i];
        else if (a.startsWith("--project=")) {
            QString p = a.mid(10);
            QDir d(p);
            if (d.exists()) configPath = d.filePath("ai_build.json");
        }
        else if (a == "--project" && i + 1 < argc) {
            QDir d(argv[++i]);
            if (d.exists()) configPath = d.filePath("ai_build.json");
        }
        else if (configPath.isEmpty() && QFileInfo(a).isDir()) {
            QDir d(a);
            if (d.exists("ai_build.json")) configPath = d.filePath("ai_build.json");
        }
        else if (configPath.isEmpty() && QFileInfo(a).isFile()) {
            configPath = a;
        }
        // 2026-09-18: `ab <name>` 自动找 ai_build.<name>.json (跟 `ab cloud_main_qt` 直接对应一个聚焦配置)
        //   优先级: ai_build.<name>.json (相对 cwd) → 失败继续走默认 ai_build.json 查找
        //   例: `ab cloud_main_qt` → ai_build.cloud_main_qt.json (Qt dock 测试)
        //       `ab cloud_main`    → ai_build.cloud_main.json (cloud_main 编译/启动)
        //       `ab both_3d`       → ai_build.both_3d.json (cloud_main + qtdock 一起跑)
        else if (configPath.isEmpty() && !a.startsWith("-")) {
            QDir cwd = QDir::current();
            QString named = cwd.filePath(QString("ai_build.%1.json").arg(a));
            if (QFileInfo(named).isFile()) {
                configPath = named;
                std::cerr << "[ab] 命名配置: " << named.toStdString() << "\n";
            }
        }
    }

    if (doctorMode) {
        // 2026-09-02: --doctor 也支持 --theme 强制应用 (写 theme.json)
        if (!forcedTheme.isEmpty()) {
            AbTheme::apply(static_cast<int>(AbTheme::parse(forcedTheme)));
        }
        return runDoctor();
    }

    // 找配置
    if (configPath.isEmpty()) {
        QDir d = QDir::current();
        if (d.exists("ai_build.json")) configPath = d.filePath("ai_build.json");
    }
    if (configPath.isEmpty()) {
        std::cerr << "✗ 没找到 ai_build.json (在当前目录或 --project 指定)\n";
        std::cerr << "  用法: ab /path/to/project\n";
        return 1;
    }

    // 加载配置
    AbConfig cfg = AbConfig::load(configPath);
    if (noAuto) cfg.auto_start = false;
    if (autoExit) cfg.auto_exit = true;  // 2026-10-09 加 — CLI --auto-exit 覆盖配置
    if (!forcedTheme.isEmpty()) cfg.theme = forcedTheme;

    std::cerr << "[ab] 配置: " << configPath.toStdString() << "\n";
    std::cerr << cfg.toJsonString().toStdString();

    // 应用主题
    AbTheme::apply(static_cast<int>(AbTheme::parse(cfg.theme)));

    // 2026-10-26: 语言 — --lang=zh|en 强制覆盖 (写 lang.json 持久化); 否则用 lang.json 里的 (默认中文)
    if (!forcedLang.isEmpty()) {
        AbLang::apply(static_cast<int>(AbLang::parse(forcedLang)));
    }
    std::cerr << "[ab] 语言: " << AbLang::displayName(static_cast<int>(AbLang::current())).toStdString() << "\n";

    // 创建窗口
    ab::AbMainWindow w(cfg, configPath);
    // 2026-10-15: 设置程序化生成的 ab 图标 (任务栏/标题栏/任务切换器都用这个)
    //   之前是默认 Qt 图标, 多个 Qt 程序切换时容易混
    QApplication::setWindowIcon(QIcon(makeAbIcon()));
    w.setWindowIcon(QIcon(makeAbIcon()));
    w.show();
    return app.exec();
}
