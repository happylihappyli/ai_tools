// ab — AI Build 编译/调试 GUI (C++ Qt5/6)
// 用法:
//   ab                          # 自动在 cwd 找 ai_build.json
//   ab /path/to/project         # 指定项目 (找 ai_build.json)
//   ab --project /path/to/proj
//   ab --config ai_build.json   # 指定配置文件
//   ab --no-auto                # 不自动跑 auto 链
//   ab --doctor                 # 环境自检
//   ab --theme dark|light       # 强制主题
//   ab --help                   # 帮助

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <iostream>
#include "AbConfig.h"
#include "AbTheme.h"
#include "AbMainWindow.h"

using namespace ab;

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
        << "  ab --config /path/to/ai_build.json\n"
        << "  ab --no-auto                    # 不自动跑 auto 链\n"
        << "  ab --doctor                     # 环境自检\n"
        << "  ab --theme dark|light           # 强制主题\n"
        << "  ab --help                       # 帮助\n"
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

    std::cout << "\n[3] 默认主题: "
        << (AbTheme::current() == AbTheme::Dark ? "dark" : "light") << "\n";
    return 0;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ab");
    QApplication::setApplicationVersion("1.0.0");
    QApplication::setOrganizationName("ai_tools");

    // 解析参数 (简化)
    QString configPath;
    bool noAuto = false;
    bool doctorMode = false;
    QString forcedTheme;
    for (int i = 1; i < argc; ++i) {
        QString a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        if (a == "--doctor") doctorMode = true;
        else if (a == "--no-auto") noAuto = true;
        else if (a.startsWith("--config=")) configPath = a.mid(9);
        else if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (a.startsWith("--theme=")) forcedTheme = a.mid(8);
        else if (a == "--theme" && i + 1 < argc) forcedTheme = argv[++i];
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
    }

    if (doctorMode) return runDoctor();

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
    if (!forcedTheme.isEmpty()) cfg.theme = forcedTheme;

    std::cerr << "[ab] 配置: " << configPath.toStdString() << "\n";
    std::cerr << cfg.toJsonString().toStdString();

    // 应用主题
    AbTheme::apply(static_cast<int>(AbTheme::parse(cfg.theme)));

    // 创建窗口
    ab::AbMainWindow w(cfg);
    w.show();
    return app.exec();
}
