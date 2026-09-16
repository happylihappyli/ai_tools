// TaskRunnerWindow.cpp — 独立任务运行器窗口 (仿 ac_task_runner_gui.py)
// 2026-09-16: 新增, Step 6/7.
#include "TaskRunnerWindow.h"
#include "AbTaskRunner.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>  // std::max
#include <QDebug>

namespace ab {

// 2026-09-16: cloud_main 启动路径 (按 .trae/rules/godot_vulkan.md 强制 vulkan+wayland)
//   跟 ac_task_runner_gui.py 一致
static const QString kCloudMainBin =
    "/home/bv/code/godot_ui_linux/godot-ui-standalone-skia/bin/Debug/cloud_main";
static const QString kCloudMainCwd =
    "/home/bv/code/godot_ui_linux/godot-ui-standalone-skia/bin/Debug";

TaskRunnerWindow::TaskRunnerWindow(AbTaskRunner* runner,
                                   const QString& task_name,
                                   const QStringList& sub_cmds,
                                   const QStringList& sub_descs,
                                   QWidget* parent)
    : QMainWindow(parent),
      runner_(runner),
      task_name_(task_name),
      sub_cmds_(sub_cmds),
      sub_descs_(sub_descs) {
    setAttribute(Qt::WA_DeleteOnClose, false);  // 不自动删除, ab 主窗口管生命周期
    setWindowTitle(QString("📊 任务运行器 — %1").arg(task_name));
    resize(1200, 800);
    buildUi();
    wireRunner();
    resetUi(sub_cmds_.size());

    // elapsed 计时器
    elapsed_timer_ = new QTimer(this);
    elapsed_timer_->setInterval(1000);
    connect(elapsed_timer_, &QTimer::timeout, this, &TaskRunnerWindow::onUpdateElapsed);
    elapsed_timer_->start();

    // current dt 计时器 (sub-task 跑时启动, 完成后停)
    current_dt_timer_ = new QTimer(this);
    current_dt_timer_->setInterval(500);
    connect(current_dt_timer_, &QTimer::timeout, this, &TaskRunnerWindow::onUpdateCurrentDt);
}

TaskRunnerWindow::~TaskRunnerWindow() {
    if (runner_) {
        runner_->disconnect(this);  // 防止 runner 引用失效时调到本窗口
    }
}

void TaskRunnerWindow::switchTask(const QString& task_name,
                                  const QStringList& sub_cmds,
                                  const QStringList& sub_descs) {
    task_name_ = task_name;
    sub_cmds_  = sub_cmds;
    sub_descs_ = sub_descs;
    setWindowTitle(QString("📊 任务运行器 — %1").arg(task_name));
    if (lbl_title_) lbl_title_->setText(task_name);
    resetUi(sub_cmds_.size());
}

void TaskRunnerWindow::buildUi() {
    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ============== 顶部 header: 任务名 + status badge + elapsed ==============
    header_ = new QFrame();
    header_->setFrameShape(QFrame::Shape::StyledPanel);
    header_->setStyleSheet(
        "QFrame { background-color: #1a1a1a; border: 1px solid #444; "
        "border-radius: 4px; padding: 6px; }"
    );
    QHBoxLayout* hl = new QHBoxLayout(header_);
    hl->setContentsMargins(8, 4, 8, 4);
    hl->setSpacing(12);

    lbl_title_ = new QLabel(task_name_);
    lbl_title_->setFont(QFont("sans-serif", 14, QFont::Weight::Bold));
    lbl_title_->setStyleSheet("color: #88ccff;");
    hl->addWidget(lbl_title_);

    hl->addStretch();

    lbl_elapsed_ = new QLabel("⏱ 00:00");
    lbl_elapsed_->setFont(QFont("monospace", 11));
    lbl_elapsed_->setStyleSheet("color: #aaa;");
    hl->addWidget(lbl_elapsed_);

    lbl_status_ = new QLabel("⏳ 等待启动");
    lbl_status_->setFont(QFont("sans-serif", 11, QFont::Weight::Bold));
    lbl_status_->setStyleSheet(
        "background-color: #444; color: #fff; padding: 4px 14px; "
        "border-radius: 4px; border: 1px solid #888;"
    );
    hl->addWidget(lbl_status_);

    root->addWidget(header_);

    // ============== 进度条 ==============
    progress_ = new QProgressBar();
    progress_->setRange(0, std::max(sub_cmds_.size(), 1));
    progress_->setValue(0);
    progress_->setFormat("0 / " + QString::number(sub_cmds_.size()) + "  (sub-task)");
    progress_->setFixedHeight(20);
    progress_->setStyleSheet(
        "QProgressBar { border: 1px solid #444; border-radius: 3px; "
        "background-color: #1a1a1a; color: #fff; text-align: center; }"
        "QProgressBar::chunk { background-color: #4682b4; }"
    );
    root->addWidget(progress_);

    // ============== ⚡ 当前执行 区 ==============
    current_box_ = new QFrame();
    current_box_->setFrameShape(QFrame::Shape::StyledPanel);
    current_box_->setStyleSheet(
        "QFrame { background-color: #1a2a3a; border: 2px solid #4682b4; "
        "border-radius: 4px; padding: 4px; }"
    );
    QVBoxLayout* cb_layout = new QVBoxLayout(current_box_);
    cb_layout->setContentsMargins(8, 4, 8, 4);
    cb_layout->setSpacing(2);
    QHBoxLayout* cur_top = new QHBoxLayout();
    lbl_current_title_ = new QLabel("⚡ 当前执行");
    lbl_current_title_->setFont(QFont("sans-serif", 11, QFont::Weight::Bold));
    lbl_current_title_->setStyleSheet("color: #88ccff;");
    cur_top->addWidget(lbl_current_title_);
    lbl_current_idx_ = new QLabel("—");
    lbl_current_idx_->setFont(QFont("monospace", 12, QFont::Weight::Bold));
    lbl_current_idx_->setStyleSheet(
        "background-color: #4682b4; color: #fff; padding: 2px 8px; "
        "border-radius: 3px;"
    );
    cur_top->addWidget(lbl_current_idx_);
    lbl_current_dt_ = new QLabel("⏱ 0s");
    lbl_current_dt_->setFont(QFont("monospace", 10));
    lbl_current_dt_->setStyleSheet("color: #aaa;");
    cur_top->addWidget(lbl_current_dt_);
    cur_top->addStretch();
    cb_layout->addLayout(cur_top);
    txt_current_cmd_ = new QPlainTextEdit();
    txt_current_cmd_->setReadOnly(true);
    txt_current_cmd_->setFont(QFont("monospace", 10));
    txt_current_cmd_->setMaximumHeight(70);
    txt_current_cmd_->setLineWrapMode(QPlainTextEdit::LineWrapMode::WidgetWidth);
    txt_current_cmd_->setStyleSheet(
        "QPlainTextEdit { background-color: #0a1828; color: #ffeeaa; "
        "border: 1px solid #2a4a6a; padding: 4px; }"
    );
    txt_current_cmd_->setPlaceholderText("(等待 sub-task 开始)");
    cb_layout->addWidget(txt_current_cmd_);
    root->addWidget(current_box_);

    // ============== 中间 split: 左 sub-task 列表, 右 实时 log ==============
    splitter_ = new QSplitter(Qt::Orientation::Horizontal);
    splitter_->setSizes({400, 1000});

    // 左
    QWidget* left = new QWidget();
    QVBoxLayout* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    QLabel* lbl_sub_list_title = new QLabel("📋 Sub-task 进度");
    lbl_sub_list_title->setStyleSheet("color: #88ccff; font-weight: bold;");
    ll->addWidget(lbl_sub_list_title);
    sub_list_ = new QListWidget();
    sub_list_->setFont(QFont("monospace", 10));
    sub_list_->setStyleSheet(
        "QListWidget { background-color: #1a1a1a; }"
        "QListWidget::item { padding: 4px; border-bottom: 1px solid #2a2a2a; }"
        "QListWidget::item:selected { background-color: #4682b4; }"
    );
    connect(sub_list_, &QListWidget::itemClicked,
            this, [this](QListWidgetItem*) { onSubItemClicked(); });
    connect(sub_list_, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem*, QListWidgetItem*) { onSubItemClicked(); });
    ll->addWidget(sub_list_);
    splitter_->addWidget(left);

    // 右 (log)
    QWidget* right = new QWidget();
    QVBoxLayout* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* log_header = new QHBoxLayout();
    lbl_log_summary_ = new QLabel("📄 实时 log (stdout, 0 行)");
    lbl_log_summary_->setStyleSheet("color: #88ccff; font-weight: bold;");
    log_header->addWidget(lbl_log_summary_);
    log_header->addStretch();
    rl->addLayout(log_header);
    log_view_ = new QPlainTextEdit();
    log_view_->setReadOnly(true);
    log_view_->setFont(QFont("monospace", 10));
    log_view_->setStyleSheet(
        "QPlainTextEdit { background-color: #0a0a0a; color: #cccccc; "
        "border: 1px solid #333; }"
    );
    log_view_->setMaximumBlockCount(10000);   // 限 1万 行, 防卡 UI
    rl->addWidget(log_view_);
    splitter_->addWidget(right);

    root->addWidget(splitter_, /*stretch=*/1);

    // ============== 底部 详情区 ==============
    detail_box_ = new QGroupBox("📜 选中 sub-task 详情");
    QVBoxLayout* dl = new QVBoxLayout(detail_box_);
    dl->setContentsMargins(6, 4, 6, 4);
    dl->setSpacing(2);
    QHBoxLayout* dt = new QHBoxLayout();
    lbl_detail_meta_ = new QLabel("(未选中 sub-task)");
    lbl_detail_meta_->setFont(QFont("monospace", 10, QFont::Weight::Bold));
    lbl_detail_meta_->setStyleSheet("color: #88ccff;");
    dt->addWidget(lbl_detail_meta_);
    dt->addStretch();
    btn_open_sub_log_ = new QPushButton("📂 打开 sub-task log");
    btn_open_sub_log_->setEnabled(false);
    connect(btn_open_sub_log_, &QPushButton::clicked, this, &TaskRunnerWindow::onOpenSubLog);
    dt->addWidget(btn_open_sub_log_);
    btn_copy_sub_cmd_ = new QPushButton("📋 复制完整命令");
    btn_copy_sub_cmd_->setEnabled(false);
    connect(btn_copy_sub_cmd_, &QPushButton::clicked, this, &TaskRunnerWindow::onCopySubCmd);
    dt->addWidget(btn_copy_sub_cmd_);
    dl->addLayout(dt);
    txt_detail_cmd_ = new QPlainTextEdit();
    txt_detail_cmd_->setReadOnly(true);
    txt_detail_cmd_->setFont(QFont("monospace", 9));
    txt_detail_cmd_->setMinimumHeight(80);
    txt_detail_cmd_->setMaximumHeight(280);
    txt_detail_cmd_->setLineWrapMode(QPlainTextEdit::LineWrapMode::WidgetWidth);
    txt_detail_cmd_->setPlaceholderText("(点击左侧 sub-task 行, 显示完整命令)");
    txt_detail_cmd_->setStyleSheet(
        "QPlainTextEdit { background-color: #0a0a0a; color: #aaffaa; "
        "border: 1px solid #333; padding: 4px; }"
    );
    txt_detail_cmd_->setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOn);
    dl->addWidget(txt_detail_cmd_);
    root->addWidget(detail_box_);

    // ============== 底部按钮行 ==============
    QHBoxLayout* btn_row = new QHBoxLayout();
    btn_stop_ = new QPushButton("⏹ Stop (杀 task)");
    btn_stop_->setStyleSheet(
        "QPushButton { background-color: #aa4444; color: #fff; padding: 6px 14px; "
        "font-weight: bold; border-radius: 3px; }"
        "QPushButton:disabled { background-color: #444; color: #888; }"
    );
    connect(btn_stop_, &QPushButton::clicked, this, &TaskRunnerWindow::onStopClicked);
    btn_row->addWidget(btn_stop_);

    btn_copy_log_ = new QPushButton("📋 复制 log");
    connect(btn_copy_log_, &QPushButton::clicked, this, &TaskRunnerWindow::onCopyLog);
    btn_row->addWidget(btn_copy_log_);

    btn_open_dir_ = new QPushButton("📂 打开 log 目录");
    connect(btn_open_dir_, &QPushButton::clicked, this, &TaskRunnerWindow::onOpenLogDir);
    btn_row->addWidget(btn_open_dir_);

    // 2026-09-16: 启动 cloud_main 按钮 (跟 ac_task_runner_gui.py 一致)
    btn_launch_cloud_ = new QPushButton("🚀 启动 cloud_main");
    btn_launch_cloud_->setStyleSheet(
        "QPushButton { background-color: #2d7d2d; color: #fff; padding: 6px 14px; "
        "font-weight: bold; border-radius: 3px; }"
        "QPushButton:hover { background-color: #3d9d3d; }"
        "QPushButton:disabled { background-color: #444; color: #888; }"
    );
    btn_launch_cloud_->setToolTip(
        "启动 bin/Debug/cloud_main (Vulkan + Wayland)\n"
        "适用于: ac task 跑完后看 3D 区域 + FPS Overlay"
    );
    connect(btn_launch_cloud_, &QPushButton::clicked, this, &TaskRunnerWindow::onLaunchCloudMain);
    btn_row->addWidget(btn_launch_cloud_);
    btn_row->addStretch();

    root->addLayout(btn_row);

    setCentralWidget(central);

    // 状态栏
    QStatusBar* sb = statusBar();
    sb->showMessage("就绪", 2000);

    // 暗主题 (仿 ac_task_runner_gui.py)
    setStyleSheet(
        "QMainWindow, QWidget { background-color: #2b2b2b; color: #ddd; }"
        "QMenuBar { background-color: #1a1a1a; }"
        "QMenuBar::item:selected { background-color: #4682b4; }"
        "QMenu { background-color: #2b2b2b; border: 1px solid #444; }"
        "QMenu::item:selected { background-color: #4682b4; }"
        "QStatusBar { background-color: #1a1a1a; color: #888; }"
        "QPushButton { background-color: #3a3a3a; color: #ddd; padding: 4px 10px; "
                      "border: 1px solid #555; border-radius: 3px; }"
        "QPushButton:hover { background-color: #4a4a4a; }"
    );

    // 菜单
    QMenuBar* mb = menuBar();
    QMenu* mrun = mb->addMenu("运行");
    QAction* a_stop = new QAction("⏹ Stop", this);
    a_stop->setShortcut(QKeySequence("Ctrl+X"));
    connect(a_stop, &QAction::triggered, this, &TaskRunnerWindow::onStopClicked);
    mrun->addAction(a_stop);
    mrun->addSeparator();
    QAction* a_close = new QAction("关闭", this);
    a_close->setShortcut(QKeySequence("Ctrl+Q"));
    connect(a_close, &QAction::triggered, this, &QMainWindow::close);
    mrun->addAction(a_close);

    QMenu* mlog = mb->addMenu("Log");
    QAction* a_copy = new QAction("📋 复制 log", this);
    a_copy->setShortcut(QKeySequence("Ctrl+L"));
    connect(a_copy, &QAction::triggered, this, &TaskRunnerWindow::onCopyLog);
    mlog->addAction(a_copy);
    QAction* a_open = new QAction("📂 打开 log 目录", this);
    a_open->setShortcut(QKeySequence("Ctrl+O"));
    connect(a_open, &QAction::triggered, this, &TaskRunnerWindow::onOpenLogDir);
    mlog->addAction(a_open);
}

void TaskRunnerWindow::wireRunner() {
    if (!runner_) {
        qWarning() << "[TaskRunnerWindow] runner is null";
        return;
    }
    connect(runner_, &AbTaskRunner::output, this, &TaskRunnerWindow::onOutput);
    connect(runner_, &AbTaskRunner::sub_started, this, &TaskRunnerWindow::onSubStarted);
    connect(runner_, &AbTaskRunner::sub_finished, this, &TaskRunnerWindow::onSubFinished);
    connect(runner_, &AbTaskRunner::sub_failed, this, &TaskRunnerWindow::onSubFailed);
    connect(runner_, &AbTaskRunner::finished, this, &TaskRunnerWindow::onTaskFinished);
    connect(runner_, &AbTaskRunner::error, this, &TaskRunnerWindow::onError);
}

void TaskRunnerWindow::resetUi(int total) {
    total_sub_ = total;
    sub_states_.clear();
    selected_sub_idx_ = 0;

    // 清 sub-task 列表 + 预填
    // 同时也改 progress_->setRange 调 max → std::max
    if (sub_list_) {
        sub_list_->clear();
        for (int i = 1; i <= total; ++i) {
            SubState st;
            st.status = "pending";
            sub_states_[i] = st;

            // 优先用 desc
            QString desc = (i - 1 < sub_descs_.size()) ? sub_descs_[i - 1] : QString();
            QString cmd  = (i - 1 < sub_cmds_.size()) ? sub_cmds_[i - 1] : QString();
            QString label = desc.isEmpty()
                            ? QString("  ⏳ [%1/%2] 等待…").arg(i).arg(total)
                            : QString("  ⏳ [%1/%2] %3").arg(i).arg(total).arg(desc);
            QListWidgetItem* it = new QListWidgetItem(label, sub_list_);
            it->setForeground(QColor("#888"));
            it->setData(Qt::UserRole, i);
            if (!desc.isEmpty()) it->setToolTip(desc + "\n$ " + cmd);
            else if (!cmd.isEmpty()) it->setToolTip("$ " + cmd);
        }
    }

    // 重置进度
    if (progress_) {
        progress_->setRange(0, std::max(total, 1));
        progress_->setValue(0);
        progress_->setFormat(QString("0 / %1  (sub-task)").arg(total));
    }

    // 重置 header
    if (lbl_status_) {
        lbl_status_->setText("⏳ 等待启动");
        lbl_status_->setStyleSheet(
            "background-color: #444; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888;"
        );
    }
    if (lbl_elapsed_) lbl_elapsed_->setText("⏱ 00:00");
    task_start_ms_ = QDateTime::currentMSecsSinceEpoch();
    elapsed_timer_->start();

    // 重置当前执行区
    if (lbl_current_idx_) {
        lbl_current_idx_->setText("—");
        lbl_current_idx_->setStyleSheet(
            "background-color: #4682b4; color: #fff; padding: 2px 8px; "
            "border-radius: 3px;"
        );
    }
    if (lbl_current_dt_) lbl_current_dt_->setText("⏱ 0s");
    if (txt_current_cmd_) txt_current_cmd_->clear();

    // 清 log
    if (log_view_) {
        log_view_->clear();
        if (lbl_log_summary_) lbl_log_summary_->setText("📄 实时 log (stdout, 0 行)");
    }

    // 重置详情区
    if (lbl_detail_meta_) lbl_detail_meta_->setText("(未选中 sub-task)");
    if (txt_detail_cmd_) txt_detail_cmd_->clear();
    if (btn_open_sub_log_) btn_open_sub_log_->setEnabled(false);
    if (btn_copy_sub_cmd_) btn_copy_sub_cmd_->setEnabled(false);

    // 按钮可用性
    if (btn_stop_) btn_stop_->setEnabled(true);
}

QString TaskRunnerWindow::logDirForCurrentTask() const {
    // 跟 ac 一致: ~/.cache/ai_tools/task_log/<task_name>/
    QString home = QDir::homePath();
    QString dir = home + "/.cache/ai_tools/task_log/" + task_name_;
    QDir().mkpath(dir);
    return dir;
}

// =====================================================================
// 接 AbTaskRunner signals
// =====================================================================
void TaskRunnerWindow::onOutput(const QString& task_name, const QString& line) {
    if (task_name != task_name_) return;
    if (!log_view_) return;
    // 颜色标记: ▶▶▶ / ✓ / ✗ 头 (仿 ac_task_runner_gui.py)
    QString esc = line;
    esc.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    QString color;
    if (line.startsWith("▶▶▶")) color = "#88ccff";
    else if (line.startsWith("✓")) color = "#88ff88";
    else if (line.startsWith("✗")) color = "#ff8888";
    else if (line.startsWith("🚀") || line.contains("===")) color = "#ffaa44";

    if (!color.isEmpty()) {
        log_view_->appendHtml(QString("<span style=\"color:%1;\">%2</span>").arg(color, esc));
    } else {
        log_view_->appendPlainText(line);
    }
    // 自动滚到底
    QScrollBar* sb = log_view_->verticalScrollBar();
    sb->setValue(sb->maximum());
    // log 摘要
    if (lbl_log_summary_) {
        lbl_log_summary_->setText(QString("📄 实时 log (stdout, %1 行)").arg(log_view_->blockCount()));
    }
}

void TaskRunnerWindow::onSubStarted(const QString& task_name, int idx, int total,
                                    const QString& desc, const QString& cmd) {
    if (task_name != task_name_) return;

    // 第一次 sub_started 时记录 task 开始时间
    if (idx == 1 && task_start_ms_ == 0) {
        task_start_ms_ = QDateTime::currentMSecsSinceEpoch();
    }
    current_sub_start_ms_ = QDateTime::currentMSecsSinceEpoch();
    current_dt_timer_->start();

    // 更新状态数据
    if (!sub_states_.contains(idx)) {
        SubState st; sub_states_[idx] = st;
    }
    sub_states_[idx].status = "running";
    sub_states_[idx].desc = desc;
    sub_states_[idx].cmd = cmd;

    // 更新列表项 (idx - 1 是 0-based index)
    if (sub_list_ && idx - 1 < sub_list_->count()) {
        QListWidgetItem* it = sub_list_->item(idx - 1);
        QString label = desc.isEmpty()
                        ? QString("  ⟳ [%1/%2] %3").arg(idx).arg(total).arg(cmd)
                        : QString("  ⟳ [%1/%2] %3").arg(idx).arg(total).arg(desc);
        it->setText(label);
        it->setForeground(QColor("#88ccff"));
        // tooltip: desc + cmd + log 路径
        QString tip = QString("sub-task [%1/%2]  跑中…\n").arg(idx).arg(total);
        if (!desc.isEmpty()) tip += "\n📌 " + desc;
        if (!cmd.isEmpty())  tip += "\n$ " + cmd;
        it->setToolTip(tip);
        // 高亮 + 滚动
        sub_list_->setCurrentRow(idx - 1);
        sub_list_->scrollToItem(it);
    }

    // 更新当前执行区
    if (lbl_current_idx_) {
        lbl_current_idx_->setText(QString("[%1/%2]").arg(idx).arg(total));
        lbl_current_idx_->setStyleSheet(
            "background-color: #ff8800; color: #fff; padding: 2px 8px; "
            "border-radius: 3px;"
        );
    }
    if (lbl_current_dt_) lbl_current_dt_->setText("⏱ 0s");
    if (txt_current_cmd_) {
        txt_current_cmd_->setPlainText(desc.isEmpty() ? cmd : desc);
    }

    // 更新 status badge
    if (lbl_status_) {
        lbl_status_->setText(QString("⏳ Running (%1/%2)").arg(idx).arg(total));
        lbl_status_->setStyleSheet(
            "background-color: #4488aa; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
        );
    }

    // 详情区: 自动刷新当前选中
    selected_sub_idx_ = idx;
    onSubItemClicked();

    statusBar()->showMessage(
        QString("🚀 sub-task [%1/%2] 开始").arg(idx).arg(total), 3000);
}

void TaskRunnerWindow::onSubFinished(const QString& task_name, int idx, int total,
                                     int rc, double dt_sec) {
    if (task_name != task_name_) return;
    current_dt_timer_->stop();

    if (!sub_states_.contains(idx)) {
        SubState st; sub_states_[idx] = st;
    }
    sub_states_[idx].rc = rc;
    sub_states_[idx].dt = dt_sec;
    sub_states_[idx].status = (rc == 0) ? "ok" : "fail";

    // 更新列表项
    if (sub_list_ && idx - 1 < sub_list_->count()) {
        QListWidgetItem* it = sub_list_->item(idx - 1);
        QString sym = (rc == 0) ? "✓" : "✗";
        QString desc = sub_states_[idx].desc;
        QString cmd  = sub_states_[idx].cmd;
        QString label = desc.isEmpty()
                        ? QString("  %1 [%2/%3] rc=%4 %5s  %6")
                              .arg(sym).arg(idx).arg(total).arg(rc).arg(dt_sec, 0, 'f', 1).arg(cmd)
                        : QString("  %1 [%2/%3] rc=%4 %5s  %6")
                              .arg(sym).arg(idx).arg(total).arg(rc).arg(dt_sec, 0, 'f', 1).arg(desc);
        it->setText(label);
        if (rc == 0) it->setForeground(QColor("#88ff88"));
        else         it->setForeground(QColor("#ff8888"));
        QString tip = QString("sub-task [%1/%2]  rc=%3  %4s\n").arg(idx).arg(total).arg(rc).arg(dt_sec, 0, 'f', 2);
        if (!desc.isEmpty()) tip += "\n📌 " + desc;
        if (!cmd.isEmpty())  tip += "\n$ " + cmd;
        it->setToolTip(tip);
    }

    // 进度条
    if (progress_) {
        int done = 0;
        for (const auto& s : sub_states_) {
            if (s.status == "ok" || s.status == "fail") done++;
        }
        progress_->setValue(done);
        progress_->setFormat(QString("%1 / %2  (sub-task)").arg(done).arg(total));
    }

    // 当前执行区
    if (lbl_current_idx_) {
        lbl_current_idx_->setStyleSheet(
            "background-color: #44aa44; color: #fff; padding: 2px 8px; "
            "border-radius: 3px;"
        );
    }
    if (lbl_current_dt_) lbl_current_dt_->setText(QString("⏱ %1s %2").arg(dt_sec, 0, 'f', 1).arg(rc == 0 ? "✓" : "✗"));

    // 详情区刷新
    if (selected_sub_idx_ == idx) onSubItemClicked();

    statusBar()->showMessage(
        QString("%1 sub-task [%2/%3]  rc=%4  %5s")
            .arg(rc == 0 ? "✓" : "✗").arg(idx).arg(total).arg(rc).arg(dt_sec, 0, 'f', 1),
        5000);
}

void TaskRunnerWindow::onSubFailed(const QString& task_name, int idx, int total) {
    if (task_name != task_name_) return;
    statusBar()->showMessage(
        QString("✗ sub-task [%1/%2] 失败, 后续停止").arg(idx).arg(total), 5000);
}

void TaskRunnerWindow::onTaskFinished(const QString& task_name, int exit_code, double elapsed) {
    if (task_name != task_name_) return;
    elapsed_timer_->stop();
    current_dt_timer_->stop();
    if (btn_stop_) btn_stop_->setEnabled(false);

    int m = int(elapsed) / 60;
    int s = int(elapsed) % 60;
    if (lbl_elapsed_) lbl_elapsed_->setText(QString("⏱ %1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));

    if (exit_code == 0) {
        if (lbl_status_) {
            lbl_status_->setText("✓ Done");
            lbl_status_->setStyleSheet(
                "background-color: #44aa44; color: #fff; padding: 4px 14px; "
                "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
            );
        }
        if (progress_) {
            progress_->setValue(progress_->maximum());
            progress_->setFormat(QString("✓ %1 / %1").arg(progress_->maximum()));
        }
        if (lbl_current_idx_) {
            lbl_current_idx_->setText("[DONE]");
            lbl_current_idx_->setStyleSheet(
                "background-color: #228822; color: #fff; padding: 2px 8px; "
                "border-radius: 3px;"
            );
        }
        statusBar()->showMessage(
            QString("✓ %1 完成 (rc=0)  log: %2/latest.log").arg(task_name_, logDirForCurrentTask()),
            10000);
    } else {
        if (lbl_status_) {
            lbl_status_->setText(QString("✗ Fail (rc=%1)").arg(exit_code));
            lbl_status_->setStyleSheet(
                "background-color: #aa4444; color: #fff; padding: 4px 14px; "
                "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
            );
        }
        statusBar()->showMessage(
            QString("✗ %1 失败 (rc=%2)").arg(task_name_).arg(exit_code),
            10000);
    }
}

void TaskRunnerWindow::onError(const QString& task_name, int err) {
    if (task_name != task_name_) return;
    if (lbl_status_) {
        lbl_status_->setText("✗ 启动失败");
        lbl_status_->setStyleSheet(
            "background-color: #aa4444; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
        );
    }
    if (log_view_) log_view_->appendPlainText(QString("[ERROR] err=%1").arg(err));
}

// =====================================================================
// UI 槽
// =====================================================================
void TaskRunnerWindow::onStopClicked() {
    if (runner_ && runner_->isRunning()) {
        statusBar()->showMessage("⏹ 正在停止 task…", 3000);
        runner_->stop();
    }
}

void TaskRunnerWindow::onCopyLog() {
    if (!log_view_) return;
    QApplication::clipboard()->setText(log_view_->toPlainText());
    statusBar()->showMessage("✓ log 已复制到剪贴板", 3000);
}

void TaskRunnerWindow::onOpenLogDir() {
    QString dir = logDirForCurrentTask();
    QStringList candidates = {
        "xdg-open", "nautilus", "dolphin", "thunar", "pcmanfm"
    };
    for (const QString& c : candidates) {
        QProcess* p = new QProcess(this);
        p->start(c, {dir});
        if (p->waitForStarted(1500)) {
            statusBar()->showMessage(QString("📂 打开 %1").arg(dir), 3000);
            return;
        }
        delete p;
    }
    statusBar()->showMessage(QString("⚠ 找不到文件管理器, log 目录: %1").arg(dir), 5000);
}

void TaskRunnerWindow::onOpenSubLog() {
    if (selected_sub_idx_ <= 0) return;
    QString log_path = sub_states_[selected_sub_idx_].log_path;
    if (log_path.isEmpty()) {
        // 跟 ac 一致: 找 sub_task_<N>_<TS>.log
        QString dir = logDirForCurrentTask();
        QStringList nameFilters;
        nameFilters << QString("%1_*.log").arg(selected_sub_idx_, 2, 10, QChar('0'));
        QDir d(dir);
        QStringList files = d.entryList(nameFilters, QDir::Files, QDir::Time);
        if (files.isEmpty()) {
            statusBar()->showMessage(QString("⚠ 找不到 sub-task %1 的 log").arg(selected_sub_idx_), 5000);
            return;
        }
        log_path = dir + "/" + files.first();
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(log_path));
    statusBar()->showMessage(QString("📂 打开 %1").arg(log_path), 3000);
}

void TaskRunnerWindow::onCopySubCmd() {
    if (selected_sub_idx_ <= 0 || !txt_detail_cmd_) return;
    QApplication::clipboard()->setText(txt_detail_cmd_->toPlainText());
    statusBar()->showMessage("✓ 完整命令已复制到剪贴板", 3000);
}

void TaskRunnerWindow::onLaunchCloudMain() {
    QFileInfo bin(kCloudMainBin);
    if (!bin.isFile()) {
        QMessageBox::warning(this, "启动失败",
            QString("找不到 binary:\n%1\n\n请先跑 ac task 编译一次").arg(kCloudMainBin));
        return;
    }
    QFileInfo lib(kCloudMainCwd + "/libworkspace_v7.so");
    if (!lib.isFile()) {
        QMessageBox::warning(this, "启动失败",
            QString("找不到 libworkspace_v7.so:\n%1\n\n请检查 workspace_v7_lib 同步").arg(lib.absoluteFilePath()));
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("GDK_BACKEND", "wayland");
    env.insert("MOZ_ENABLE_WAYLAND", "1");
    env.insert("QT_QPA_PLATFORM", "wayland");
    env.insert("WAYLAND_DISPLAY", env.value("WAYLAND_DISPLAY", "wayland-0"));
    env.insert("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/nvidia_icd.json");
    env.insert("DISPLAY", env.value("DISPLAY", ":0"));

    QStringList args;
    args << "--rendering-driver" << "vulkan"
         << "--rendering-method" << "forward_plus";

    // Qt5: startDetached(program, arguments, workingDirectory, pid*)
    qint64 pid = 0;
    bool started_ok = QProcess::startDetached(
        kCloudMainBin, args, kCloudMainCwd, &pid);
    if (started_ok && pid > 0) {
        statusBar()->showMessage(QString("🚀 cloud_main 已启动 (pid=%1, Vulkan+Wayland)").arg(pid), 5000);
    } else {
        QMessageBox::warning(this, "启动失败", QString("QProcess::startDetached 返回失败, 启动失败"));
    }
}

void TaskRunnerWindow::onSubItemClicked() {
    if (!sub_list_) return;
    QListWidgetItem* it = sub_list_->currentItem();
    if (!it) {
        if (lbl_detail_meta_) lbl_detail_meta_->setText("(未选中 sub-task)");
        if (txt_detail_cmd_) txt_detail_cmd_->clear();
        if (btn_open_sub_log_) btn_open_sub_log_->setEnabled(false);
        if (btn_copy_sub_cmd_) btn_copy_sub_cmd_->setEnabled(false);
        selected_sub_idx_ = 0;
        return;
    }
    int idx = it->data(Qt::UserRole).toInt();
    selected_sub_idx_ = idx;
    if (!sub_states_.contains(idx)) return;
    const SubState& st = sub_states_[idx];

    QString meta = QString("[%1/%2]  rc=%3  %4s  status=%5")
        .arg(idx).arg(total_sub_).arg(st.rc).arg(st.dt, 0, 'f', 2).arg(st.status);
    if (lbl_detail_meta_) lbl_detail_meta_->setText(meta);

    QString full = QString("$ %1\n").arg(st.cmd);
    if (!st.desc.isEmpty()) full = QString("📌 %1\n").arg(st.desc) + full;
    if (!st.log_path.isEmpty()) full += QString("\n📄 log: %1").arg(st.log_path);
    if (txt_detail_cmd_) txt_detail_cmd_->setPlainText(full);

    if (btn_open_sub_log_) btn_open_sub_log_->setEnabled(true);
    if (btn_copy_sub_cmd_) btn_copy_sub_cmd_->setEnabled(true);
}

void TaskRunnerWindow::onUpdateElapsed() {
    if (task_start_ms_ == 0) return;
    qint64 ms = QDateTime::currentMSecsSinceEpoch() - task_start_ms_;
    int s_total = int(ms / 1000);
    int m = s_total / 60;
    int s = s_total % 60;
    if (lbl_elapsed_) lbl_elapsed_->setText(QString("⏱ %1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
}

void TaskRunnerWindow::onUpdateCurrentDt() {
    if (current_sub_start_ms_ == 0) return;
    qint64 ms = QDateTime::currentMSecsSinceEpoch() - current_sub_start_ms_;
    double dt = ms / 1000.0;
    if (dt < 60) {
        if (lbl_current_dt_) lbl_current_dt_->setText(QString("⏱ %1s").arg(dt, 0, 'f', 1));
    } else {
        int m = int(dt) / 60;
        int s = int(dt) % 60;
        if (lbl_current_dt_) lbl_current_dt_->setText(QString("⏱ %1m%2s").arg(m).arg(s, 2, 10, QChar('0')));
    }
}

}  // namespace ab