// AbTaskInspector.cpp — 任务 + 进程检查器
// 2026-09-02: 新增
#include "AbTaskInspector.h"

#include <QListWidget>  // 2026-09-16 v3: sub-task 列表
#include <QProgressBar> // 2026-09-16 v3: sub-task 进度条
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDateTime>
#include <QMessageBox>
#include <QDebug>

namespace ab {

static QString historyPath() {
    QStringList p = QStandardPaths::standardLocations(QStandardPaths::ConfigLocation);
    if (!p.isEmpty()) return p.first() + "/ai_tools/task_history.json";
    return QDir::homePath() + "/.ai_tools_task_history.json";
}

AbTaskInspector::AbTaskInspector(QWidget* parent)
    : QDockWidget("任务检查器", parent) {
    setObjectName("AbTaskInspectorDock");
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    buildUi();
    loadHistory();
    refreshTasks();

    auto_timer_.setInterval(3000);
    connect(&auto_timer_, &QTimer::timeout, this, &AbTaskInspector::onAutoTick);
    auto_timer_.start();

    refreshProcesses();
}

void AbTaskInspector::buildUi() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* vl = new QVBoxLayout(w);
    vl->setContentsMargins(8, 8, 8, 8);
    vl->setSpacing(6);

    // 顶部工具行
    QHBoxLayout* top = new QHBoxLayout();
    filter_edit_ = new QLineEdit();
    filter_edit_->setPlaceholderText("🔍 过滤 (任务名 / 进程名 / PID)");
    filter_edit_->setClearButtonEnabled(true);
    connect(filter_edit_, &QLineEdit::textChanged, this, &AbTaskInspector::onFilterChanged);
    top->addWidget(filter_edit_, 1);

    auto_chk_ = new QCheckBox("自动刷新");
    auto_chk_->setChecked(true);
    connect(auto_chk_, &QCheckBox::toggled, this, &AbTaskInspector::onAutoRefreshToggled);
    top->addWidget(auto_chk_);

    refresh_btn_ = new QPushButton("🔄 刷新");
    connect(refresh_btn_, &QPushButton::clicked, this, &AbTaskInspector::onRefreshClicked);
    top->addWidget(refresh_btn_);
    vl->addLayout(top);

    // 状态行
    info_lbl_ = new QLabel("就绪");
    info_lbl_->setStyleSheet("color: #888; font-size: 11px;");
    vl->addWidget(info_lbl_);

    // tab
    tabs_ = new QTabWidget();
    // -- 任务 tab --
    task_tree_ = new QTreeWidget();
    // 2026-09-08 v2: 加 "命令" 列 (从主窗口 GroupBox 合并过来的功能, 让这里双击能跑)
    task_tree_->setHeaderLabels({"状态", "任务名", "说明", "上次结果", "耗时", "时间", "命令"});
    task_tree_->setRootIsDecorated(false);
    task_tree_->setAlternatingRowColors(true);
    task_tree_->setColumnWidth(0, 50);
    task_tree_->setColumnWidth(1, 140);
    task_tree_->setColumnWidth(2, 200);
    task_tree_->setColumnWidth(3, 80);
    task_tree_->setColumnWidth(4, 70);
    task_tree_->setColumnWidth(5, 130);
    task_tree_->setColumnWidth(6, 320);
    connect(task_tree_, &QTreeWidget::itemDoubleClicked, this, &AbTaskInspector::onItemDoubleClicked);
    tabs_->addTab(task_tree_, "📋 任务");

    // -- 进程 tab --
    proc_tree_ = new QTreeWidget();
    proc_tree_->setHeaderLabels({"PID", "用户", "已运行", "CPU%", "内存%", "命令"});
    proc_tree_->setRootIsDecorated(false);
    proc_tree_->setAlternatingRowColors(true);
    proc_tree_->setColumnWidth(0, 60);
    proc_tree_->setColumnWidth(1, 90);
    proc_tree_->setColumnWidth(2, 100);
    proc_tree_->setColumnWidth(3, 60);
    proc_tree_->setColumnWidth(4, 60);
    connect(proc_tree_, &QTreeWidget::itemDoubleClicked, this, &AbTaskInspector::onItemDoubleClicked);
    tabs_->addTab(proc_tree_, "⚙️ 进程");

    // -- 2026-09-16 v3: sub-task tab --
    sub_task_panel_ = new QWidget();
    QVBoxLayout* sub_vl = new QVBoxLayout(sub_task_panel_);
    sub_vl->setContentsMargins(8, 8, 8, 8);
    sub_vl->setSpacing(4);

    sub_task_title_ = new QLabel("⏸ 暂无任务在跑");
    sub_task_title_->setStyleSheet("color: #88ccff; font-size: 12px; font-weight: bold;");
    sub_vl->addWidget(sub_task_title_);

    sub_task_prog_ = new QProgressBar();
    sub_task_prog_->setRange(0, 1);  // 防止 0/0 显示异常
    sub_task_prog_->setValue(0);
    sub_task_prog_->setFormat("%v / %m  (sub-task)");
    sub_task_prog_->setFixedHeight(16);
    sub_vl->addWidget(sub_task_prog_);

    sub_task_list_ = new QListWidget();
    sub_task_list_->setAlternatingRowColors(true);
    sub_task_list_->setWordWrap(true);  // sub-task desc 可能很长, 自动换行
    sub_vl->addWidget(sub_task_list_, 1);

    sub_task_info_ = new QLabel("");
    sub_task_info_->setStyleSheet("color: #888; font-size: 11px;");
    sub_vl->addWidget(sub_task_info_);

    tabs_->addTab(sub_task_panel_, "📊 sub-task");

    vl->addWidget(tabs_, 1);
    setWidget(w);
}

void AbTaskInspector::setConfig(const AbConfig& cfg) {
    cfg_ = cfg;
    refreshTasks();
}

// =====================================================================
// 状态持久化 (~/.config/ai_tools/task_history.json)
// =====================================================================
void AbTaskInspector::loadHistory() {
    stats_.clear();
    QFile f(historyPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        QJsonObject v = it.value().toObject();
        TaskStat s;
        s.status       = v.value("status").toString("ready");
        s.last_rc      = v.value("last_rc").toInt(0);
        s.last_elapsed = v.value("last_elapsed").toDouble(0);
        s.last_time    = v.value("last_time").toString();
        s.run_count    = v.value("run_count").toInt(0);
        s.err_count    = v.value("err_count").toInt(0);
        stats_[it.key()] = s;
    }
}

void AbTaskInspector::saveHistory() {
    QFileInfo fi(historyPath());
    QDir().mkpath(fi.absolutePath());
    QFile f(historyPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QJsonObject obj;
    for (auto it = stats_.begin(); it != stats_.end(); ++it) {
        QJsonObject v;
        v["status"]       = it.value().status;
        v["last_rc"]      = it.value().last_rc;
        v["last_elapsed"] = it.value().last_elapsed;
        v["last_time"]    = it.value().last_time;
        v["run_count"]    = it.value().run_count;
        v["err_count"]    = it.value().err_count;
        obj[it.key()] = v;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

// =====================================================================
// 任务刷新
// =====================================================================
void AbTaskInspector::refreshTasks() {
    QString filter = filter_edit_ ? filter_edit_->text().toLower() : QString();
    task_tree_->clear();
    int shown = 0;
    for (const auto& t : cfg_.tasks) {
        if (!filter.isEmpty()
            && !t.name.toLower().contains(filter)
            && !t.description.toLower().contains(filter)) continue;
        TaskStat s = stats_.value(t.name);
        if (t.name == current_running_) s.status = "running";
        auto* it = new QTreeWidgetItem();
        QString icon = "⏸";
        QString rc_text = "—";
        if (s.status == "running") { icon = "🟡"; rc_text = "跑中"; }
        else if (s.status == "ok") { icon = "✓"; rc_text = QString("rc=%1").arg(s.last_rc); }
        else if (s.status == "err") { icon = "✗"; rc_text = QString("rc=%1").arg(s.last_rc); }
        it->setText(0, icon);
        it->setText(1, t.name);
        it->setText(2, t.description);
        it->setText(3, rc_text);
        it->setText(4, s.last_elapsed > 0 ? QString("%1s").arg(s.last_elapsed, 0, 'f', 1) : "—");
        it->setText(5, s.last_time.isEmpty() ? "—" : s.last_time);
        // 2026-09-08 v2: 命令列 (从主窗口 GroupBox 合并过来, 让双击跑)
        it->setText(6, t.cmd);
        // 列 6 字体调小, 灰色显示 (命令是技术细节, 不抢视觉)
        QFont cmd_font = it->font(6);
        cmd_font.setPointSize(cmd_font.pointSize() - 1);
        it->setFont(6, cmd_font);
        it->setForeground(6, QColor("#888"));
        if (s.status == "ok") {
            for (int c = 0; c < 6; ++c) it->setForeground(c, QColor("#6a9955"));
        } else if (s.status == "err") {
            for (int c = 0; c < 6; ++c) it->setForeground(c, QColor("#f48771"));
        } else if (s.status == "running") {
            for (int c = 0; c < 6; ++c) it->setForeground(c, QColor("#dcdcaa"));
            QFont f = it->font(0);
            f.setBold(true);
            it->setFont(0, f);
        }
        task_tree_->addTopLevelItem(it);
        shown++;
    }
    if (info_lbl_) {
        info_lbl_->setText(QString("任务: %1 个 (显示 %2) | 当前跑: %3")
            .arg(static_cast<int>(cfg_.tasks.size())).arg(shown)
            .arg(current_running_.isEmpty() ? "—" : current_running_));
    }
}

// =====================================================================
// 进程刷新 (ps -eo ... | grep -E 'scons|cmake|cloud_main|workspace|godot|...')
// =====================================================================
void AbTaskInspector::refreshProcesses() {
    QString filter = filter_edit_ ? filter_edit_->text().toLower() : QString();
    proc_tree_->clear();

    // 过滤关键字: 项目相关构建/运行命令
    static const QStringList kKeywords = {
        "scons", "cmake", "make", "g++", "gcc", "ld", "ldconfig",
        "cloud_main", "workspace_v7", "libworkspace", "godot", "bvws",
        "qmake", "ninja", "autoconf", "configure",
        // 项目名相关
        QFileInfo(cwd_).fileName(),  // e.g. "godot-ui-standalone-skia"
    };
    // 也过滤跟 cwd 路径匹配的进程
    QString cwd_key = cwd_;
    QString cwd_short = QFileInfo(cwd_).fileName();

    // 跑 ps (一次性, 同步; 数据小)
    QProcess p;
    p.start("ps", QStringList() << "-eo" << "pid,user,etime,pcpu,pmem,comm,args"
                                << "--no-headers");
    if (!p.waitForFinished(2000)) {
        if (info_lbl_) info_lbl_->setText("进程刷新失败 (ps 超时)");
        return;
    }
    QString out = QString::fromUtf8(p.readAllStandardOutput());
    QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    int shown = 0;
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        // 列分割 (按空白, 但 args 可能含空格, 取前 6 列)
        QStringList cols;
        int consumed = 0;
        for (int i = 0; i < 6 && consumed < trimmed.size(); ++i) {
            int start = consumed;
            while (consumed < trimmed.size() && trimmed[consumed] != ' ') consumed++;
            cols << trimmed.mid(start, consumed - start);
            while (consumed < trimmed.size() && trimmed[consumed] == ' ') consumed++;
        }
        QString args = trimmed.mid(consumed);
        if (cols.size() < 6) continue;

        QString comm = cols[5];
        bool related = false;
        for (const QString& kw : kKeywords) {
            if (!kw.isEmpty() && (comm.contains(kw, Qt::CaseInsensitive)
                || args.contains(kw, Qt::CaseInsensitive))) { related = true; break; }
        }
        if (!related && !cwd_key.isEmpty() && args.contains(cwd_key, Qt::CaseInsensitive)) related = true;
        if (!related && !cwd_short.isEmpty() && args.contains(cwd_short, Qt::CaseInsensitive)) related = true;
        if (!related) continue;

        QString lowAll = (comm + " " + args).toLower();
        if (!filter.isEmpty() && !lowAll.contains(filter)) continue;

        auto* it = new QTreeWidgetItem();
        it->setText(0, cols[0]);  // PID
        it->setText(1, cols[1]);  // user
        it->setText(2, cols[2]);  // etime
        it->setText(3, cols[3]);  // pcpu
        it->setText(4, cols[4]);  // pmem
        it->setText(5, comm + " " + args);
        it->setData(0, Qt::UserRole, cols[0].toInt());  // 存 PID 用于右键 kill
        // ab/自己相关进程高亮 (黄)
        if (comm == "ab" || comm == "ab_launcher") {
            for (int c = 0; c < 6; ++c) it->setForeground(c, QColor("#dcdcaa"));
        }
        proc_tree_->addTopLevelItem(it);
        shown++;
    }
    if (info_lbl_) {
        info_lbl_->setText(QString("任务: %1 个 | 进程: %2 个 (项目相关) | 当前跑: %3")
            .arg(static_cast<int>(cfg_.tasks.size())).arg(shown)
            .arg(current_running_.isEmpty() ? "—" : current_running_));
    }
}

void AbTaskInspector::killProcess(int pid) {
    if (pid <= 0) return;
    auto ret = QMessageBox::question(this, "确认",
        QString("杀进程 pid=%1 ?").arg(pid),
        QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    QProcess::execute("kill", QStringList() << "-TERM" << QString::number(pid));
    QTimer::singleShot(800, this, [this, pid]() {
        QProcess::execute("kill", QStringList() << "-KILL" << QString::number(pid));
        refreshProcesses();
    });
}

// =====================================================================
// 任务事件 (MainWindow 调)
// =====================================================================
void AbTaskInspector::onTaskStarted(const QString& task_name) {
    current_running_ = task_name;
    TaskStat& s = stats_[task_name];
    s.status = "running";
    s.run_count++;
    saveHistory();
    refreshTasks();
}

void AbTaskInspector::onTaskRunning(const QString& task_name) {
    if (current_running_ != task_name) {
        current_running_ = task_name;
        refreshTasks();
    }
}

void AbTaskInspector::onTaskFinished(const QString& task_name, int exit_code, double elapsed) {
    current_running_.clear();
    TaskStat& s = stats_[task_name];
    s.status = (exit_code == 0) ? "ok" : "err";
    s.last_rc = exit_code;
    s.last_elapsed = elapsed;
    s.last_time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    if (exit_code != 0) s.err_count++;
    saveHistory();
    refreshTasks();
    refreshProcesses();
}

// =====================================================================
// UI 槽
// =====================================================================
void AbTaskInspector::onRefreshClicked() {
    refreshTasks();
    refreshProcesses();
}

void AbTaskInspector::onAutoRefreshToggled(bool checked) {
    if (checked) auto_timer_.start();
    else auto_timer_.stop();
}

void AbTaskInspector::onFilterChanged(const QString&) {
    refreshTasks();
    refreshProcesses();
}

void AbTaskInspector::onAutoTick() {
    // 任务状态: 不重读历史, 只更新当前跑 (避免闪烁)
    if (!current_running_.isEmpty()) {
        // 高亮当前跑的行
    }
    refreshProcesses();
}

void AbTaskInspector::onItemDoubleClicked(QTreeWidgetItem* it, int /*col*/) {
    QTreeWidget* src = it ? it->treeWidget() : nullptr;
    if (src == task_tree_) {
        // 2026-09-08 v2: 双击任务直接跑 (合并主窗口 GroupBox 的功能)
        //   之前这里只提示用户用 F5, 现在任务列表主窗口已删除, 这里就是唯一的任务入口
        QString name = it->text(1);
        if (name.isEmpty()) return;
        if (info_lbl_) info_lbl_->setText(QString("⚡ 触发跑任务: %1").arg(name));
        emit requestRunTask(name);
    } else if (src == proc_tree_) {
        int pid = it->data(0, Qt::UserRole).toInt();
        QString comm = it->text(5);
        auto ret = QMessageBox::question(this, "进程操作",
            QString("进程: %1\nPID: %2\n\n点 Yes 杀进程 (TERM + KILL), No 取消").arg(comm).arg(pid),
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            killProcess(pid);
        }
    }
}

// =====================================================================
// 2026-09-16 v3: sub-task 处理 (从 MainWindow 通过 runner_ 的信号转过来)
//   目标: sub-task tab 显示当前 task 的所有 sub-task, 实时更新状态
// =====================================================================
bool AbTaskInspector::findTaskSubLists(const QString& task_name,
                                       QStringList& out_cmds,
                                       QStringList& out_descs) const {
    for (const auto& t : cfg_.tasks) {
        if (t.name == task_name) {
            // 优先 sub-task list, 缺省 fallback 到老格式
            if (!t.sub_cmds.isEmpty()) {
                out_cmds  = t.sub_cmds;
                out_descs = t.sub_descs;
                return true;
            }
            if (!t.cmd.isEmpty()) {
                out_cmds  = QStringList{t.cmd};
                out_descs = QStringList{QString()};
                return true;
            }
            return false;
        }
    }
    return false;
}

void AbTaskInspector::switchToSubTaskTab(const QString& task_name) {
    if (!sub_task_panel_ || !tabs_) return;
    // 切到 sub-task tab
    int idx = tabs_->indexOf(sub_task_panel_);
    if (idx >= 0) tabs_->setCurrentIndex(idx);
    if (sub_task_title_) {
        sub_task_title_->setText(QString("🔧 %1 (%2 sub-task%3)")
            .arg(task_name)
            .arg(current_sub_states_.size())
            .arg(current_sub_states_.size() > 1 ? "s" : ""));
        sub_task_title_->setStyleSheet("color: #88ccff; font-size: 12px; font-weight: bold;");
    }
    if (sub_task_prog_) {
        sub_task_prog_->setRange(0, current_sub_states_.size());
        sub_task_prog_->setValue(0);
    }
    if (sub_task_list_) sub_task_list_->clear();
    if (sub_task_info_) sub_task_info_->setText("进度: 0 / " + QString::number(current_sub_states_.size()));
    sub_item_by_idx_.clear();
}

void AbTaskInspector::onTaskSubStarted(const QString& task_name,
                                       int idx, int total,
                                       const QString& desc, const QString& cmd) {
    current_subtask_task_ = task_name;

    // 第一次 sub_started (idx == 1) 时清空 + 预填所有 sub-task
    if (idx == 1) {
        current_sub_states_.clear();
        QStringList cmds, descs;
        if (findTaskSubLists(task_name, cmds, descs)) {
            for (int i = 0; i < cmds.size(); ++i) {
                SubTaskState s;
                s.idx = i + 1;
                s.desc = (i < descs.size()) ? descs[i] : QString();
                s.cmd = cmds[i];
                s.status = "pending";
                current_sub_states_.append(s);
            }
        } else {
            // cfg_ 找不到, 用 on-the-fly 至少建一个 entry (不让 UI 空)
            SubTaskState s;
            s.idx = idx;
            s.desc = desc;
            s.cmd = cmd;
            s.status = "pending";
            current_sub_states_.append(s);
        }
        switchToSubTaskTab(task_name);

        // 填 list
        if (sub_task_list_) {
            for (const auto& s : current_sub_states_) {
                QString label = s.desc.isEmpty()
                                ? QString("📌 [%1/%2]  %3").arg(s.idx).arg(total).arg(s.cmd)
                                : QString("📌 [%1/%2]  %3").arg(s.idx).arg(total).arg(s.desc);
                QListWidgetItem* it = new QListWidgetItem(label, sub_task_list_);
                it->setForeground(QColor("#888"));  // pending = 灰
                // 工具用 Qt::UserRole 存 idx, 后续按 idx 改状态
                it->setData(Qt::UserRole, s.idx);
                sub_item_by_idx_[s.idx] = it;
            }
        }
    }

    // 标记当前 idx 为 running
    if (sub_item_by_idx_.contains(idx)) {
        QListWidgetItem* it = sub_item_by_idx_[idx];
        const SubTaskState& s = current_sub_states_[idx - 1];
        QString label = s.desc.isEmpty()
                        ? QString("⟳ [%1/%2]  %3").arg(idx).arg(total).arg(s.cmd)
                        : QString("⟳ [%1/%2]  %3").arg(idx).arg(total).arg(s.desc);
        it->setText(label);
        it->setForeground(QColor("#88ccff"));  // running = 蓝
        // 高亮 + 滚动
        if (sub_task_list_) {
            sub_task_list_->setCurrentItem(it);
            sub_task_list_->scrollToItem(it);
        }
    }
    if (sub_task_prog_) {
        // 进度条: 已完成 = idx - 1, 当前 = idx (显示部分填充)
        sub_task_prog_->setValue(idx - 1);  // 让当前条还没"完成"
    }
    if (sub_task_info_) {
        sub_task_info_->setText(QString("进度: %1 / %2  ·  当前: %3")
            .arg(idx - 1).arg(total).arg(desc.isEmpty() ? cmd : desc));
    }
}

void AbTaskInspector::onTaskSubFinished(const QString& task_name,
                                        int idx, int total,
                                        int rc, double dt_sec) {
    if (task_name != current_subtask_task_) return;
    if (sub_item_by_idx_.contains(idx)) {
        QListWidgetItem* it = sub_item_by_idx_[idx];
        const SubTaskState& s = current_sub_states_[idx - 1];
        QString ok_mark = (rc == 0) ? "✓" : "✗";
        QString dt_str = QString::number(dt_sec, 'f', 1) + "s";
        QString label = s.desc.isEmpty()
                        ? QString("%1 [%2/%3]  %4  (%5)")
                              .arg(ok_mark).arg(idx).arg(total).arg(s.cmd).arg(dt_str)
                        : QString("%1 [%2/%3]  %4  (%5)")
                              .arg(ok_mark).arg(idx).arg(total).arg(s.desc).arg(dt_str);
        it->setText(label);
        if (rc == 0) {
            it->setForeground(QColor("#6a9955"));  // ok = 绿
        } else {
            it->setForeground(QColor("#f48771"));  // err = 红
        }
    }
    // 更新状态
    if (idx - 1 < current_sub_states_.size()) {
        current_sub_states_[idx - 1].status = (rc == 0) ? "ok" : "err";
        current_sub_states_[idx - 1].dt_sec = dt_sec;
    }
    // 进度条: 已完成
    if (sub_task_prog_) {
        sub_task_prog_->setValue(idx);
    }
    if (sub_task_info_) {
        sub_task_info_->setText(QString("进度: %1 / %2  ·  上一步: %3s (rc=%4)")
            .arg(idx).arg(total).arg(dt_sec, 0, 'f', 1).arg(rc));
    }
}

void AbTaskInspector::onTaskSubFailed(const QString& task_name,
                                      int idx, int total) {
    if (task_name != current_subtask_task_) return;
    if (sub_task_title_) {
        sub_task_title_->setText(QString("✗✗ %1  (第 %3/%4 步失败, 后续不跑)")
            .arg(task_name).arg(idx).arg(total));
        sub_task_title_->setStyleSheet("color: #f48771; font-size: 12px; font-weight: bold;");
    }
    if (sub_task_info_) {
        sub_task_info_->setText(QString("✗ 失败: 第 %1/%2 步 → 后续 sub-task 跳过").arg(idx).arg(total));
    }
}

}  // namespace ab
