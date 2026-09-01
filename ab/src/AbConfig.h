#ifndef AB_CONFIG_H
#define AB_CONFIG_H
// SPDX-License-Identifier: MIT
//
// AbConfig — 读 ai_build.json + ui 段
//
// 支持字段:
//   cwd, cmd, auto
//   tasks: [{name, cmd, description}]
//   ui: {
//     title, window: {width, height},
//     theme: "dark"|"light", auto_start: bool,
//     show_log_dock: bool,
//     menus: [{name, items: [{type, id, label, shortcut, task, cmd, ...}]}],
//     toolbar: [{id, label, tooltip, task, ...}],
//     buttons: [{id, label, color, task, ...}],
//     run_after_build: {binary_path, args, auto: bool}
//   }

#include <QString>
#include <QList>
#include <QSize>
#include <vector>
#include <string>
#include "AbJson.h"

namespace ab {

struct AbButtonDef {
    QString id;
    QString label;
    QString tooltip;
    QString task;        // 任务名 (从 tasks 找)
    QString cmd;         // 或直接命令
    QString shortcut;
    QString color;       // primary / success / warn / danger / default
    bool    checkable = false;
    bool    enabled    = true;
};

struct AbMenuItem {
    enum Type { Action, Separator, Button };
    Type type = Action;
    QString id;
    QString label;
    QString shortcut;
    QString task;
    QString cmd;
    QString tooltip;
    QString color;
    bool    checkable = false;
};

struct AbMenuDef {
    QString name;
    std::vector<AbMenuItem> items;
};

struct AbRunAfterBuild {
    QString binary_path;  // e.g. "bin/Debug/cloud_main"
    QStringList args;     // e.g. ["--rendering-driver","vulkan",...]
    bool auto_run = false;  // 编译成功后自动跑 (false=只启用按钮)
    QString button_label = "🚀 启动";
    QString on_success_task;  // 备用: 或跑指定 task
};

struct AbConfig {
    // 基础
    QString cwd;
    QString cmd;
    QStringList auto_chain;   // e.g. ["build+deploy","diag","view-log"]

    // 任务列表
    struct Task {
        QString name;
        QString cmd;
        QString description;
    };
    std::vector<Task> tasks;

    // UI
    QString title      = "ab — AI Build";
    QSize   window     = QSize(1000, 700);
    QString theme      = "dark";        // "dark" | "light"
    bool    auto_start = true;
    bool    show_log_dock = true;

    std::vector<AbMenuDef> menus;
    std::vector<AbButtonDef> toolbar;
    std::vector<AbButtonDef> buttons;
    AbRunAfterBuild run_after_build;

    // 加载 / 保存
    static AbConfig load(const QString& path);
    QString toJsonString() const;  // 调试用
};

}  // namespace ab

#endif
