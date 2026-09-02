// AbTaskInspector.cpp — 任务 + 进程检查器
// 2026-09-02: 新增
#include "AbTaskInspector.h"

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
    task_tree_->setHeaderLabels({"状态", "任务名", "说明", "上次结果", "耗时", "时间"});
    task_tree_->setRootIsDecorated(false);
    task_tree_->setAlternatingRowColors(true);
    task_tree_->setColumnWidth(0, 50);
    task_tree_->setColumnWidth(1, 140);
    task_tree_->setColumnWidth(2, 200);
    task_tree_->setColumnWidth(3, 80);
    task_tree_->setColumnWidth(4, 70);
    task_tree_->setColumnWidth(5, 130);
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
        QString name = it->text(1);
        // 通知 main window 跑 (通过 signal 接到)
        // 这里直接调 exit(0) 跑会不方便, 改成发信号
        // 但更简单: 弹状态栏
        // 用 QMessageBox 简短提示
        if (info_lbl_) info_lbl_->setText(QString("✗ 双击任务请用工具栏 [▶ 跑选中] (F5), 选中后 F5 即可. 当前选中: %1").arg(name));
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

}  // namespace ab
