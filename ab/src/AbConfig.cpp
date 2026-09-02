// AbConfig.cpp — 读 ai_build.json
#include "AbConfig.h"
#include <QFile>
#include <QJsonDocument>  // Qt 内置, 简化
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

namespace ab {

// 辅助: 把 QStringList 拆 AbJson array 存回去
// (这里我们不写 save, 留作扩展)

AbConfig AbConfig::load(const QString& path) {
    AbConfig cfg;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[AbConfig] cannot open" << path;
        return cfg;
    }
    QByteArray data = f.readAll();
    f.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[AbConfig] JSON parse error:" << err.errorString();
        return cfg;
    }
    QJsonObject root = doc.object();

    // 基础
    cfg.cwd = root["cwd"].toString();
    if (cfg.cwd.isEmpty()) {
        cfg.cwd = QFileInfo(path).absolutePath();
    }
    cfg.cmd = root["cmd"].toString();
    QJsonArray autoArr = root["auto"].toArray();
    for (const auto& v : autoArr) {
        cfg.auto_chain << v.toString();
    }

    // 任务
    QJsonArray tasksArr = root["tasks"].toArray();
    for (const auto& tv : tasksArr) {
        QJsonObject t = tv.toObject();
        Task tk;
        tk.name = t["name"].toString();
        tk.cmd  = t["cmd"].toString();
        tk.description = t["description"].toString();
        if (!tk.name.isEmpty()) cfg.tasks.push_back(tk);
    }

    // UI 段
    QJsonObject ui = root["ui"].toObject();
    if (!ui.isEmpty()) {
        cfg.title = ui["title"].toString(cfg.title);
        cfg.theme = ui["theme"].toString(cfg.theme);
        cfg.auto_start = ui["auto_start"].toBool(cfg.auto_start);
        cfg.show_log_dock = ui["show_log_dock"].toBool(cfg.show_log_dock);

        // window
        QJsonObject win = ui["window"].toObject();
        if (!win.isEmpty()) {
            cfg.window = QSize(win["width"].toInt(1000), win["height"].toInt(700));
        }

        // menus
        QJsonArray menusArr = ui["menus"].toArray();
        for (const auto& mv : menusArr) {
            QJsonObject m = mv.toObject();
            AbMenuDef md;
            md.name = m["name"].toString();
            QJsonArray itemsArr = m["items"].toArray();
            for (const auto& iv : itemsArr) {
                QJsonObject it = iv.toObject();
                AbMenuItem mi;
                QString type = it["type"].toString();
                if (type == "separator") {
                    mi.type = AbMenuItem::Separator;
                } else {
                    mi.type = AbMenuItem::Action;
                }
                mi.id        = it["id"].toString();
                mi.label     = it["label"].toString();
                mi.shortcut  = it["shortcut"].toString();
                mi.task      = it["task"].toString();
                mi.cmd       = it["cmd"].toString();
                mi.tooltip   = it["tooltip"].toString();
                mi.color     = it["color"].toString();
                mi.checkable = it["checkable"].toBool(false);
                md.items.push_back(mi);
            }
            if (!md.name.isEmpty()) cfg.menus.push_back(md);
        }

        // toolbar
        QJsonArray tbArr = ui["toolbar"].toArray();
        for (const auto& tv : tbArr) {
            QJsonObject t = tv.toObject();
            AbButtonDef b;
            b.id        = t["id"].toString();
            b.label     = t["label"].toString();
            b.tooltip   = t["tooltip"].toString();
            b.task      = t["task"].toString();
            b.cmd       = t["cmd"].toString();
            b.shortcut  = t["shortcut"].toString();
            b.color     = t["color"].toString();
            b.checkable = t["checkable"].toBool(false);
            cfg.toolbar.push_back(b);
        }

        // buttons (主按钮行)
        QJsonArray btnArr = ui["buttons"].toArray();
        for (const auto& bv : btnArr) {
            QJsonObject b = bv.toObject();
            AbButtonDef bd;
            bd.id        = b["id"].toString();
            bd.label     = b["label"].toString();
            bd.tooltip   = b["tooltip"].toString();
            bd.task      = b["task"].toString();
            bd.cmd       = b["cmd"].toString();
            bd.shortcut  = b["shortcut"].toString();
            bd.color     = b["color"].toString();
            bd.checkable = b["checkable"].toBool(false);
            cfg.buttons.push_back(bd);
        }

        // run_after_build
        QJsonObject rab = ui["run_after_build"].toObject();
        if (!rab.isEmpty()) {
            cfg.run_after_build.binary_path = rab["binary_path"].toString();
            cfg.run_after_build.auto_run    = rab["auto_run"].toBool(false);
            cfg.run_after_build.button_label = rab["button_label"].toString("🚀 启动");
            cfg.run_after_build.on_success_task = rab["on_success_task"].toString();
            QJsonArray argsArr = rab["args"].toArray();
            for (const auto& a : argsArr) {
                cfg.run_after_build.args << a.toString();
            }
            // env: { "KEY": "VALUE", ... } (2026-09-02 加, 支持 $VAR 展开, 解决 3D 空白 bug)
            // 例: LD_LIBRARY_PATH = "$LD_LIBRARY_PATH:/extra/path" → 拼到原值
            QJsonObject envObj = rab["env"].toObject();
            for (const auto& key : envObj.keys()) {
                cfg.run_after_build.env.insert(key, envObj[key].toString());
            }
        }
    }

    return cfg;
}

QString AbConfig::toJsonString() const {
    // 调试用: 简单 dump
    QString s;
    s += QString("AbConfig {\n");
    s += QString("  cwd=%1\n").arg(cwd);
    s += QString("  cmd=%1\n").arg(cmd);
    s += QString("  auto=[%1]\n").arg(auto_chain.join(","));
    s += QString("  tasks=%1\n").arg(static_cast<int>(tasks.size()));
    s += QString("  ui: title=%1 theme=%2 menus=%3 toolbar=%4 buttons=%5\n")
        .arg(title).arg(theme)
        .arg(static_cast<int>(menus.size()))
        .arg(static_cast<int>(toolbar.size()))
        .arg(static_cast<int>(buttons.size()));
    if (!run_after_build.binary_path.isEmpty()) {
        s += QString("  run_after_build: %1 auto=%2\n")
            .arg(run_after_build.binary_path)
            .arg(run_after_build.auto_run ? "true" : "false");
    }
    s += "}\n";
    return s;
}

}  // namespace ab
