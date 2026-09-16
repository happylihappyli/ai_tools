// AbMainWindow.cpp
#include "AbMainWindow.h"
#include "AbTaskRunner.h"
#include "AbLogDock.h"
#include "AbTheme.h"
#include "AbTaskInspector.h"  // 2026-09-02: 任务/进程检查器 (现在通过 InspectorWindow 间接访问)
#include "InspectorWindow.h"  // 2026-09-16 v4: 独立 InspectorWindow (包 AbTaskInspector)
#include "TaskRunnerWindow.h" // 2026-09-16: 独立任务运行器窗口 (仿 ac_task_runner_gui.py)

#include <QVBoxLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QRect>
#include <QHBoxLayout>
#include <QFrame>
#include <QGroupBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QKeySequence>
#include <QInputDialog>
#include <QLineEdit>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QApplication>
#include <QDebug>
#include <QRegularExpression>
// 2026-09-16 v3: 中央面板新增 widget
#include <QPlainTextEdit>
#include <QListWidget>
#include <QSplitter>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QFont>
#include <QDateTime>
#include <QScrollBar>
#include <unistd.h>
#include <cstdlib>
#include <string>

namespace ab {

AbMainWindow::AbMainWindow(const AbConfig& cfg, QWidget* parent)
    : QMainWindow(parent), cfg_(cfg) {
    updateWindowTitle();   // 2026-09-09: 初始窗口标题 = cfg_.title
    // 2026-09-16 v5: 启动时窗口宽度 = 屏幕宽度的 80%, 高度用 cfg (默认 700)
    //   跨屏幕分辨率自动适配 (1080p / 1440p / 4K 都能 80% 填满)
    // v7 (2026-09-16): 默认高度提到 900 (让 sub-task 列表显示 ≥5 行 + 当前执行区 + 详情 + log 都可见)
    //   取消 setMaximumSize 锁, 允许用户拖大/最大化 (用户反馈"高度可以高一些,要可以最大化")
    int win_w = cfg_.window.width()  > 0 ? cfg_.window.width()  : 1000;
    int win_h = cfg_.window.height() > 0 ? cfg_.window.height() : 900;
    if (auto* screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        win_w = int(avail.width() * 0.8);
        // 高度不超过屏幕 95%, 避免小屏幕上超出
        const int max_h = int(avail.height() * 0.95);
        if (win_h > max_h) win_h = max_h;
        // 居中显示 (比默认左上角好看)
        move(avail.x() + (avail.width()  - win_w) / 2,
             avail.y() + (avail.height() - win_h) / 2);
    }
    resize(win_w, win_h);
    // 2026-09-16 v7: 取消 max size 锁 (允许最大化), 仅设最小尺寸防过小
    setMinimumSize(800, 600);

    runner_ = new AbTaskRunner(this);
    wireRunner();

    buildFromConfig();

    log("info", QString("ab 启动 v1.0.0"));
    log("info", QString("项目: %1").arg(cfg_.cwd));
    log("info", QString("任务数: %1").arg(static_cast<int>(cfg_.tasks.size())));
    log("info", QString("主题: %1").arg(cfg_.theme));
    // 2026-09-08 v4: auto 链也 log 一份, 这样 log dock 完整, 不必去状态栏看
    log("info", QString("auto 链: %1")
        .arg(cfg_.auto_chain.isEmpty() ? "(无)" : cfg_.auto_chain.join(" → ")));

    // 检查 cloud_main (如果配置了 run_after_build)
    if (!cfg_.run_after_build.binary_path.isEmpty()) {
        QString bin = findRunBinary();
        if (!bin.isEmpty()) {
            cloud_binary_ = bin;
            enableRunCloudButton(true);
            log("ok", QString("✓ %1 已就绪").arg(QFileInfo(bin).fileName()));
        } else {
            log("warn", QString("未找到 %1, 编译后可启用").arg(cfg_.run_after_build.binary_path));
        }
    }

    // 探测外部工具绝对路径 (避免桌面 GUI PATH 不带 ~/.local/bin)
    ac_binary_     = findTool("ac");
    ar_binary_     = findTool("ar");  // 2026-09-09: AI Run
    spd_say_binary_ = findTool("spd-say");
    const char* ac_no_tts = std::getenv("AC_NO_TTS");
    tts_enabled_   = !(ac_no_tts && std::string(ac_no_tts) == "1");
    if (ac_binary_.isEmpty()) {
        log("warn", "未找到 ac 命令, 菜单 [GitHub Token 管理] 等会失败 (PATH 不全?)");
    } else {
        log("info", QString("ac: %1").arg(ac_binary_));
    }
    if (ar_binary_.isEmpty()) {
        log("warn", QString::fromUtf8("未找到 ar 命令, [AR 运行工具] 按钮会失败 (装 ar_launcher 到 ~/.local/bin?)"));
    } else {
        log("info", QString("ar: %1").arg(ar_binary_));
    }
    if (spd_say_binary_.isEmpty()) {
        log("warn", "未找到 spd-say, TTS 播报不可用");
    } else if (!tts_enabled_) {
        log("info", QString("TTS 已关闭 (AC_NO_TTS=1, spd-say=%1)").arg(spd_say_binary_));
    } else {
        log("info", QString("TTS 已启用: %1").arg(spd_say_binary_));
    }

    // 2026-09-08 v3: 把"任务列表在哪"这种提示信息放到操作日志里, 主窗口不显示
    log("info", "💡 任务列表: 顶部 [任务检查器] → [📋 任务] tab, 双击行直接跑");
    log("info", "💡 操作日志: 底部面板, 编译/任务输出实时滚出 (Ctrl+Shift+L 切换)");

    // auto_start
    if (cfg_.auto_start) {
        QTimer::singleShot(300, this, &AbMainWindow::onRunAuto);
    }
}

AbMainWindow::~AbMainWindow() = default;

void AbMainWindow::wireRunner() {
    connect(runner_, &AbTaskRunner::output,   this, &AbMainWindow::onOutput);
    connect(runner_, &AbTaskRunner::finished, this, &AbMainWindow::onFinished);
    connect(runner_, &AbTaskRunner::error,    this, &AbMainWindow::onError);
    // 2026-09-16 v4: sub-task signals → inspector_window_->inspector() (独立窗口, lazy 创建)
    connect(runner_, &AbTaskRunner::sub_started,
            this, [this](const QString& name, int idx, int total,
                         const QString& desc, const QString& cmd) {
                if (inspector_window_) {
                    if (auto* ins = inspector_window_->inspector()) {
                        ins->onTaskSubStarted(name, idx, total, desc, cmd);
                    }
                }
            });
    connect(runner_, &AbTaskRunner::sub_finished,
            this, [this](const QString& name, int idx, int total,
                         int rc, double dt) {
                if (inspector_window_) {
                    if (auto* ins = inspector_window_->inspector()) {
                        ins->onTaskSubFinished(name, idx, total, rc, dt);
                    }
                }
            });
    connect(runner_, &AbTaskRunner::sub_failed,
            this, [this](const QString& name, int idx, int total) {
                if (inspector_window_) {
                    if (auto* ins = inspector_window_->inspector()) {
                        ins->onTaskSubFailed(name, idx, total);
                    }
                }
            });
}

void AbMainWindow::buildFromConfig() {
    // 2026-09-16 v3: 中央面板 (仿 Python 版 ac_task_runner_gui.py)
    buildCentralPanel();
    buildStatusBar();
    buildBuiltInMenus();   // 2026-09-02: 框架内置通用菜单
    buildBuiltInToolbar(); // 2026-09-02: 框架内置通用工具栏 (跑选中/跑 Auto/停止)
    buildBuiltInButtons();  // 2026-09-02: 框架内置通用按钮 (已合并到工具栏, no-op)
    buildMenus();           // 配置文件追加: "运行" 菜单 (项目特定)
    buildToolbar();         // 配置文件追加: 工具栏 (项目特定, e.g. TTS 按钮)
    buildMainButtons();     // 配置文件追加: 主按钮行 (项目特定, e.g. 自定义任务)
    // 2026-09-16 v4: 移除 inspector_ dock, 改成独立窗口
    //   inspector_window_ 在 onOpenInspector() 里 lazy 创建
    //   桥接 inspector signal (1 参) → runTaskByName (2 参) 移到 lazy 里
}

// 2026-09-02: 框架内置通用菜单 (所有调试程序都需要)
//   - 文件: GitHub Token / 备份 / 退出
//   - 工具: GitHub Token 诊断 / TTS 播报 / 代理测试
//   - 视图: 显示日志 / 切换主题
//   - 帮助: 关于
// ai_build.json 不再需要配这些, 减少冗余
void AbMainWindow::buildBuiltInMenus() {
    QMenuBar* mb = menuBar();

    // --- 文件 (&F) ---
    QMenu* m_file = mb->addMenu("文件(&F)");
    {
        QAction* a = m_file->addAction("GitHub Token 管理...");
        a->setShortcut(QKeySequence("Ctrl+G"));
        a->setData("open_ght");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["open_ght"] = a;
    }
    {
        QAction* a = m_file->addAction("备份项目...");
        a->setShortcut(QKeySequence("Ctrl+B"));
        a->setData("bak");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["bak"] = a;
    }
    m_file->addSeparator();
    {
        QAction* a = m_file->addAction("退出");
        a->setShortcut(QKeySequence("Ctrl+Q"));
        a->setData("quit");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["quit"] = a;
    }

    // --- 工具 (&T) ---
    QMenu* m_tools = mb->addMenu("工具(&T)");
    {
        QAction* a = m_tools->addAction("GitHub Token 诊断");
        a->setShortcut(QKeySequence("Ctrl+D"));
        a->setData("diag");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["diag"] = a;
    }
    {
        QAction* a = m_tools->addAction("代理测试...");
        a->setShortcut(QKeySequence("Ctrl+P"));
        a->setData("proxy_test");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["proxy_test"] = a;
    }
    m_tools->addSeparator();
    {
        QAction* a = m_tools->addAction("TTS 播报...");
        a->setShortcut(QKeySequence("Ctrl+T"));
        a->setData("tts");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["tts"] = a;
    }

    // --- 视图 (&V) ---
    QMenu* m_view = mb->addMenu("视图(&V)");
    {
        QAction* a = m_view->addAction("显示日志面板");
        a->setShortcut(QKeySequence("Ctrl+Shift+L"));
        a->setCheckable(true);
        a->setChecked(cfg_.show_log_dock);
        a->setData("toggle_log");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["toggle_log"] = a;
    }
    {
        // 2026-09-16 v4: 任务检查器改为独立窗口 (主窗口不再 dock)
        //   菜单改为"打开任务检查器" (一次性打开, 跟 TaskRunnerWindow 一样)
        QAction* a = m_view->addAction("📋 打开任务检查器");
        a->setShortcut(QKeySequence("Ctrl+I"));
        a->setData("open_inspector");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["open_inspector"] = a;
    }
    // 2026-09-02: 主题改成 submenu 4 选 1 (单选 QActionGroup)
    {
        QMenu* m_theme = m_view->addMenu("主题(&T)");
        QActionGroup* group = new QActionGroup(this);
        group->setExclusive(true);
        AbTheme::Kind cur = AbTheme::current();
        for (int k = 0; k < AbTheme::NumThemes; ++k) {
            QAction* a = m_theme->addAction(AbTheme::displayName(k));
            a->setCheckable(true);
            a->setChecked(k == cur);
            a->setData(QString("theme:%1").arg(AbTheme::shortName(k)));
            connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
            group->addAction(a);
            actions_[QString("theme:%1").arg(AbTheme::shortName(k))] = a;
        }
    }

    // --- 帮助 (&H) ---
    QMenu* m_help = mb->addMenu("帮助(&H)");
    {
        QAction* a = m_help->addAction("关于 ab...");
        a->setData("about");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["about"] = a;
    }
}

// 2026-09-02: 框架内置通用工具栏 (跑选中/跑 Auto/停止 - 所有 task runner 通用)
void AbMainWindow::buildBuiltInToolbar() {
    QToolBar* tb = addToolBar("主工具栏");
    tb->setObjectName("AbMainToolBar");
    tb->setMovable(true);
    tb->setFloatable(true);

    auto add = [&](const QString& label, const QString& id, const QString& tip, const QString& sc = QString()) {
        QAction* a = tb->addAction(label);
        if (!tip.isEmpty())   a->setToolTip(tip);
        if (!sc.isEmpty())    a->setShortcut(QKeySequence(sc));
        a->setData(id);
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_[id] = a;
    };

    add("▶", "run_selected", "跑选中任务 (F5)", "F5");
    add("⚡", "run_auto",     "跑 Auto 链 (F6)", "F6");
    tb->addSeparator();
    // 2026-09-09: 4 个项目特定动作 (从 buildBuiltInButtons 合并到工具栏)
    if (!cfg_.run_after_build.binary_path.isEmpty()) {
        QString binary_name = QFileInfo(cfg_.run_after_build.binary_path).fileName();
        QString btn_label = cfg_.run_after_build.button_label.isEmpty()
                            ? QString("🚀 启动 %1").arg(binary_name)
                            : cfg_.run_after_build.button_label;
        add("⚡ 编译并启动 (Vulkan)", "build_and_run",
            "编译当前项目 (走 build+deploy task), 成功后自动调 start_cloud_main.sh (Vulkan)", "Ctrl+Shift+R");
        add("⚡ 编译并启动 (OpenGL)", "build_and_run_gl",
            "编译当前项目, 成功后自动调 start_cloud_main.sh --gl (OpenGL 兼容模式)", "Ctrl+Shift+E");
        add(btn_label, "run_cloud",
            "启动已编译的 " + binary_name);
        add("🟢 启动兼容模式", "run_cloud_gl",
            "启动已编译的 " + binary_name + " (OpenGL 兼容模式)");
        add("🎯 AR 运行工具", "open_ar",
            QString::fromUtf8("调 ac ar 打开 AI Run 运行工具 (读 ai_build.json run_panel 段)"));
        tb->addSeparator();
        add("■ 停止", "stop",
            "停止当前 task", "F7");
    }
}

// 2026-09-09 v3: 4 个中间按钮 (编译并启动/启动 cloud_main/AR/停止) 已合并到
//   buildBuiltInToolbar (用户偏好: 工具栏统一管理, 中间不占空间)
//   保留本函数为 no-op, 避免 buildUI 调用方不兼容
void AbMainWindow::buildBuiltInButtons() {
    // 全部按钮已迁到 buildBuiltInToolbar — 这里什么都不做
    (void)cfg_; (void)cloud_binary_; (void)ac_binary_; (void)ar_binary_;
}

// 2026-09-16 v3: 中央面板 (仿 ac_task_runner_gui.py)
//   - 顶部: 任务名 + status badge + elapsed
//   - 进度条: sub-task 进度
//   - ⚡ 当前执行 区
//   - 中间 split: 左 sub-task 列表 + 右 实时 log
//   - 详情区: 选中 sub-task
//   - 底部按钮行: Stop / 复制 log / 打开 log / 启动 cloud_main (Vulkan/GL)
void AbMainWindow::buildCentralPanel() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // 2026-09-16 v7: 用户反馈"底部的按钮可以放到工具栏" — 把 Stop / Copy log / 打开 log /
    //   启动 cloud_main (Vulkan/GL) 5 个按钮合并到顶部 QToolBar, 节省底部高度
    //   让 sub-task 列表可以显示更多行
    panel_toolbar_ = new QToolBar("操作", this);
    panel_toolbar_->setMovable(false);
    panel_toolbar_->setIconSize(QSize(16, 16));
    panel_toolbar_->setStyleSheet(
        "QToolBar { background-color: #2a2a2a; border-bottom: 1px solid #444; spacing: 4px; padding: 2px; }"
        "QToolButton { padding: 4px 8px; border-radius: 3px; }"
        "QToolButton:hover { background-color: #3a3a3a; }");

    panel_btn_stop_ = new QPushButton("⏹ Stop");
    panel_btn_stop_->setStyleSheet(
        "QPushButton { background-color: #aa4444; color: #fff; padding: 4px 10px; "
        "font-weight: bold; border-radius: 3px; }"
        "QPushButton:disabled { background-color: #444; color: #888; }");
    connect(panel_btn_stop_, &QPushButton::clicked, this, &AbMainWindow::onPanelStopClicked);
    panel_toolbar_->addWidget(panel_btn_stop_);

    panel_toolbar_->addSeparator();

    panel_btn_copy_log_ = new QPushButton("📋 复制 log");
    connect(panel_btn_copy_log_, &QPushButton::clicked, this, &AbMainWindow::onPanelCopyLog);
    panel_toolbar_->addWidget(panel_btn_copy_log_);

    panel_btn_open_dir_ = new QPushButton("📂 打开 log 目录");
    connect(panel_btn_open_dir_, &QPushButton::clicked, this, &AbMainWindow::onPanelOpenLogDir);
    panel_toolbar_->addWidget(panel_btn_open_dir_);

    panel_toolbar_->addSeparator();

    panel_btn_launch_cloud_ = new QPushButton("🚀 启动 cloud_main (Vulkan)");
    panel_btn_launch_cloud_->setStyleSheet(
        "QPushButton { background-color: #2d7d2d; color: #fff; padding: 4px 10px; "
        "font-weight: bold; border-radius: 3px; }"
        "QPushButton:hover { background-color: #3d9d3d; }");
    connect(panel_btn_launch_cloud_, &QPushButton::clicked, this, [this]() { onPanelLaunchCloud(false); });
    panel_toolbar_->addWidget(panel_btn_launch_cloud_);

    panel_btn_launch_cloud_gl_ = new QPushButton("🟢 启动 cloud_main (GL)");
    panel_btn_launch_cloud_gl_->setStyleSheet(
        "QPushButton { background-color: #aa7d2d; color: #fff; padding: 4px 10px; "
        "font-weight: bold; border-radius: 3px; }"
        "QPushButton:hover { background-color: #c8902d; }");
    connect(panel_btn_launch_cloud_gl_, &QPushButton::clicked, this, [this]() { onPanelLaunchCloud(true); });
    panel_toolbar_->addWidget(panel_btn_launch_cloud_gl_);

    // 2026-09-16 v7: QToolBar 加到主窗口顶部 (header 上方)
    addToolBar(Qt::TopToolBarArea, panel_toolbar_);

    // ====== 顶部 header: 任务名 + status badge + elapsed ======
    QFrame* header_ = new QFrame();
    header_->setFrameShape(QFrame::Shape::StyledPanel);
    header_->setStyleSheet(
        "QFrame { background-color: #1a1a1a; border: 1px solid #444; "
        "border-radius: 4px; padding: 6px; }");
    QHBoxLayout* hl = new QHBoxLayout(header_);
    hl->setContentsMargins(8, 4, 8, 4);
    hl->setSpacing(12);

    panel_lbl_title_ = new QLabel("⏸ 暂无任务");
    panel_lbl_title_->setFont(QFont("sans-serif", 14, QFont::Weight::Bold));
    panel_lbl_title_->setStyleSheet("color: #88ccff;");
    hl->addWidget(panel_lbl_title_);

    hl->addStretch();

    panel_lbl_elapsed_ = new QLabel("⏱ 00:00");
    panel_lbl_elapsed_->setFont(QFont("monospace", 11));
    panel_lbl_elapsed_->setStyleSheet("color: #aaa;");
    hl->addWidget(panel_lbl_elapsed_);

    panel_lbl_status_ = new QLabel("⏳ 等待启动");
    panel_lbl_status_->setFont(QFont("sans-serif", 11, QFont::Weight::Bold));
    panel_lbl_status_->setStyleSheet(
        "background-color: #444; color: #fff; padding: 4px 14px; "
        "border-radius: 4px; border: 1px solid #888;");
    hl->addWidget(panel_lbl_status_);

    root->addWidget(header_);

    // ====== 进度条 ======
    panel_progress_ = new QProgressBar();
    panel_progress_->setRange(0, 1);
    panel_progress_->setValue(0);
    panel_progress_->setFormat("0 / 0  (sub-task)");
    panel_progress_->setFixedHeight(20);
    panel_progress_->setStyleSheet(
        "QProgressBar { border: 1px solid #444; border-radius: 3px; "
        "background-color: #1a1a1a; color: #fff; text-align: center; }"
        "QProgressBar::chunk { background-color: #4682b4; }");
    root->addWidget(panel_progress_);

    // ====== ⚡ 当前执行 区 ======
    panel_current_box_ = new QFrame();
    panel_current_box_->setFrameShape(QFrame::Shape::StyledPanel);
    panel_current_box_->setStyleSheet(
        "QFrame { background-color: #1a2a3a; border: 2px solid #4682b4; "
        "border-radius: 4px; padding: 4px; }");
    QVBoxLayout* cb_layout = new QVBoxLayout(panel_current_box_);
    cb_layout->setContentsMargins(8, 4, 8, 4);
    cb_layout->setSpacing(2);
    QHBoxLayout* cur_top = new QHBoxLayout();
    QLabel* cur_title = new QLabel("⚡ 当前执行");
    cur_title->setFont(QFont("sans-serif", 11, QFont::Weight::Bold));
    cur_title->setStyleSheet("color: #88ccff;");
    cur_top->addWidget(cur_title);
    panel_lbl_current_idx_ = new QLabel("—");
    panel_lbl_current_idx_->setFont(QFont("monospace", 12, QFont::Weight::Bold));
    panel_lbl_current_idx_->setStyleSheet(
        "background-color: #4682b4; color: #fff; padding: 2px 8px; "
        "border-radius: 3px;");
    cur_top->addWidget(panel_lbl_current_idx_);
    panel_lbl_current_dt_ = new QLabel("⏱ 0s");
    panel_lbl_current_dt_->setFont(QFont("monospace", 10));
    panel_lbl_current_dt_->setStyleSheet("color: #aaa;");
    cur_top->addWidget(panel_lbl_current_dt_);
    cur_top->addStretch();
    cb_layout->addLayout(cur_top);
    panel_txt_current_cmd_ = new QPlainTextEdit();
    panel_txt_current_cmd_->setReadOnly(true);
    panel_txt_current_cmd_->setFont(QFont("monospace", 10));
    panel_txt_current_cmd_->setMaximumHeight(70);
    panel_txt_current_cmd_->setLineWrapMode(QPlainTextEdit::LineWrapMode::WidgetWidth);
    panel_txt_current_cmd_->setStyleSheet(
        "QPlainTextEdit { background-color: #0a1828; color: #ffeeaa; "
        "border: 1px solid #2a4a6a; padding: 4px; }");
    panel_txt_current_cmd_->setPlaceholderText("(等待 sub-task 开始)");
    cb_layout->addWidget(panel_txt_current_cmd_);
    root->addWidget(panel_current_box_);

    // ====== 中间 split: 左 sub-task 列表 + 右 实时 log ======
    panel_splitter_ = new QSplitter(Qt::Orientation::Horizontal);
    panel_splitter_->setSizes({400, 1000});

    QWidget* left = new QWidget();
    QVBoxLayout* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    QLabel* lbl_left_title = new QLabel("📋 Sub-task 进度");
    lbl_left_title->setStyleSheet("color: #88ccff; font-weight: bold;");
    ll->addWidget(lbl_left_title);
    panel_sub_list_ = new QListWidget();
    panel_sub_list_->setFont(QFont("monospace", 10));
    // 2026-09-16 v7: 用户反馈"中间任务列表至少要显示 5 个任务"
    //   minHeight = 5 行 * 28px + padding ≈ 160px, 让 sub-task 列表默认就够 5 行可见
    //   (窗口可拉高显示更多行)
    panel_sub_list_->setMinimumHeight(160);
    panel_sub_list_->setStyleSheet(
        "QListWidget { background-color: #1a1a1a; }"
        "QListWidget::item { padding: 4px; border-bottom: 1px solid #2a2a2a; }"
        "QListWidget::item:selected { background-color: #4682b4; }");
    connect(panel_sub_list_, &QListWidget::itemClicked,
            this, [this](QListWidgetItem*) { onPanelTaskListClicked(); });
    connect(panel_sub_list_, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem*, QListWidgetItem*) { onPanelTaskListClicked(); });
    ll->addWidget(panel_sub_list_);
    panel_splitter_->addWidget(left);

    QWidget* right = new QWidget();
    QVBoxLayout* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* log_header = new QHBoxLayout();
    panel_lbl_log_summary_ = new QLabel("📄 实时 log (stdout, 0 行)");
    panel_lbl_log_summary_->setStyleSheet("color: #88ccff; font-weight: bold;");
    log_header->addWidget(panel_lbl_log_summary_);
    log_header->addStretch();
    rl->addLayout(log_header);
    panel_log_view_ = new QPlainTextEdit();
    panel_log_view_->setReadOnly(true);
    panel_log_view_->setFont(QFont("monospace", 10));
    panel_log_view_->setStyleSheet(
        "QPlainTextEdit { background-color: #0a0a0a; color: #cccccc; "
        "border: 1px solid #333; }");
    panel_log_view_->setMaximumBlockCount(10000);
    rl->addWidget(panel_log_view_);
    panel_splitter_->addWidget(right);

    root->addWidget(panel_splitter_, /*stretch=*/1);

    // ====== 详情区 ======
    panel_detail_box_ = new QGroupBox("📜 选中 sub-task 详情");
    QVBoxLayout* dl = new QVBoxLayout(panel_detail_box_);
    dl->setContentsMargins(6, 4, 6, 4);
    dl->setSpacing(2);
    QHBoxLayout* dt = new QHBoxLayout();
    panel_lbl_detail_meta_ = new QLabel("(未选中 sub-task)");
    panel_lbl_detail_meta_->setFont(QFont("monospace", 10, QFont::Weight::Bold));
    panel_lbl_detail_meta_->setStyleSheet("color: #88ccff;");
    dt->addWidget(panel_lbl_detail_meta_);
    dt->addStretch();
    panel_btn_open_sub_log_ = new QPushButton("📂 打开 sub-task log");
    panel_btn_open_sub_log_->setEnabled(false);
    connect(panel_btn_open_sub_log_, &QPushButton::clicked, this, [this]() {
        // 复用 TaskRunnerWindow 的逻辑 (提取到 helper)
        if (panel_selected_sub_idx_ <= 0) return;
        QString dir = panelLogDirForCurrentTask();
        QStringList nameFilters;
        nameFilters << QString("%1_*.log").arg(panel_selected_sub_idx_, 2, 10, QChar('0'));
        QDir d(dir);
        QStringList files = d.entryList(nameFilters, QDir::Files, QDir::Time);
        if (files.isEmpty()) {
            log("warn", QString("⚠ 找不到 sub-task %1 的 log").arg(panel_selected_sub_idx_));
            return;
        }
        QString log_path = dir + "/" + files.first();
        QDesktopServices::openUrl(QUrl::fromLocalFile(log_path));
        log("ok", QString("📂 打开 %1").arg(log_path));
    });
    dt->addWidget(panel_btn_open_sub_log_);
    panel_btn_copy_sub_cmd_ = new QPushButton("📋 复制完整命令");
    panel_btn_copy_sub_cmd_->setEnabled(false);
    connect(panel_btn_copy_sub_cmd_, &QPushButton::clicked, this, [this]() {
        if (panel_selected_sub_idx_ <= 0 || !panel_txt_detail_cmd_) return;
        QApplication::clipboard()->setText(panel_txt_detail_cmd_->toPlainText());
        log("ok", "✓ 完整命令已复制到剪贴板");
    });
    dt->addWidget(panel_btn_copy_sub_cmd_);
    dl->addLayout(dt);
    panel_txt_detail_cmd_ = new QPlainTextEdit();
    panel_txt_detail_cmd_->setReadOnly(true);
    panel_txt_detail_cmd_->setFont(QFont("monospace", 9));
    panel_txt_detail_cmd_->setMinimumHeight(80);
    panel_txt_detail_cmd_->setMaximumHeight(280);
    panel_txt_detail_cmd_->setLineWrapMode(QPlainTextEdit::LineWrapMode::WidgetWidth);
    panel_txt_detail_cmd_->setPlaceholderText("(点击左侧 sub-task 行, 显示完整命令)");
    panel_txt_detail_cmd_->setStyleSheet(
        "QPlainTextEdit { background-color: #0a0a0a; color: #aaffaa; "
        "border: 1px solid #333; padding: 4px; }");
    panel_txt_detail_cmd_->setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOn);
    dl->addWidget(panel_txt_detail_cmd_);
    root->addWidget(panel_detail_box_);

    // 2026-09-16 v7: 底部按钮行已移至顶部 QToolBar (panel_toolbar_), 这里只留 stretch
    //   让详情区不会把 log 区挤掉
    root->addStretch();

    // ====== 计时器 ======
    panel_elapsed_timer_ = new QTimer(this);
    panel_elapsed_timer_->setInterval(1000);
    connect(panel_elapsed_timer_, &QTimer::timeout, this, &AbMainWindow::onPanelUpdateElapsed);
    panel_current_dt_timer_ = new QTimer(this);
    panel_current_dt_timer_->setInterval(500);
    connect(panel_current_dt_timer_, &QTimer::timeout, this, &AbMainWindow::onPanelUpdateCurrentDt);

    // 暗主题
    setStyleSheet(
        "QMainWindow, QWidget { background-color: #2b2b2b; color: #ddd; }"
        "QGroupBox { border: 1px solid #444; margin-top: 8px; padding-top: 6px; }"
        "QPushButton { background-color: #3a3a3a; color: #ddd; padding: 4px 10px; "
                      "border: 1px solid #555; border-radius: 3px; }"
        "QPushButton:hover { background-color: #4a4a4a; }");

    // 2026-09-16 v3: log dock 已移除, no-op (log 在中央面板)
}

void AbMainWindow::buildStatusBar() {
    // 2026-09-08 v4: 状态栏承载所有持久信息 (项目路径 + auto 链 + 进度条 + 动态 log 状态 + Qt 主题)
    //   主窗口中间区域完全清空, 只剩 stretch
    statusbar_ = new QStatusBar(this);
    setStatusBar(statusbar_);

    // 1) 最左: 项目路径 + auto 链 (持久显示, 不被 log 覆盖)
    QString auto_text = cfg_.auto_chain.isEmpty() ? "(无)" : cfg_.auto_chain.join(" → ");
    sb_proj_ = new QLabel(QString("📁 %1  |  auto: %2")
                          .arg(cfg_.cwd, auto_text));
    sb_proj_->setStyleSheet("color: #888; font-size: 11px;");
    statusbar_->addWidget(sb_proj_, 1);  // 拉伸占 1 份

    // 2) 中: 动态 log 状态 (✗/✓ msg, 被 log() 覆盖)
    sb_left_ = new QLabel("就绪");
    statusbar_->addWidget(sb_left_, 1);  // 拉伸占 1 份

    // 2.5) 2026-09-16 fix71: 渲染模式 label (展示默认 Vulkan / OpenGL 兼容模式差异)
    //   默认 "渲染模式: —" 灰, 启动 cloud_main (Vulkan) → "渲染模式: Vulkan (Forward+)" 蓝,
    //   启动兼容模式 → "渲染模式: OpenGL3 (Compatibility)" 橙. 让用户一眼看到当前是哪种渲染.
    sb_render_ = new QLabel("渲染模式: —");
    sb_render_->setStyleSheet("color: #555; font-size: 12px; padding: 0 8px; font-weight: bold;");
    statusbar_->addPermanentWidget(sb_render_);

    // 3) 右: Qt/主题 (进度条/进度文字 2026-09-16 v3 移到了中央面板顶部 header)
    sb_right_ = new QLabel(QString("Qt=%1 | 主题=%2")
                           .arg(qApp ? "Qt5/6" : "?", cfg_.theme));
    statusbar_->addPermanentWidget(sb_right_);
}

QAction* AbMainWindow::createActionForButton(const AbButtonDef& b, QWidget* parent) {
    QAction* a = new QAction(b.label.isEmpty() ? b.id : b.label, parent);
    a->setToolTip(b.tooltip);
    if (!b.shortcut.isEmpty()) a->setShortcut(QKeySequence(b.shortcut));
    if (b.checkable) a->setCheckable(true);
    if (!b.enabled)  a->setEnabled(false);
    a->setData(b.id);
    connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
    return a;
}

void AbMainWindow::buildMenus() {
    if (cfg_.menus.empty()) return;
    QMenuBar* mb = menuBar();
    for (const auto& md : cfg_.menus) {
        QMenu* m = mb->addMenu(md.name);
        for (const auto& mi : md.items) {
            if (mi.type == ab::AbMenuItem::Separator) {
                m->addSeparator();
                continue;
            }
            QAction* a = new QAction(mi.label, this);
            if (!mi.shortcut.isEmpty()) a->setShortcut(QKeySequence(mi.shortcut));
            if (!mi.tooltip.isEmpty()) a->setToolTip(mi.tooltip);
            if (mi.checkable) a->setCheckable(true);
            a->setData(mi.id);
            connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
            m->addAction(a);
            actions_[mi.id] = a;
        }
    }
}

void AbMainWindow::buildToolbar() {
    if (cfg_.toolbar.empty()) return;
    QToolBar* tb = addToolBar("主工具栏");
    tb->setObjectName("AbMainToolBar");
    tb->setMovable(true);
    tb->setFloatable(true);
    for (const auto& b : cfg_.toolbar) {
        QAction* a = createActionForButton(b, this);
        tb->addAction(a);
        actions_[b.id] = a;
    }
    // 2026-09-16: 加内置"📊 任务运行器"按钮 (仿 ac_task_runner_gui.py)
    //   不依赖 ai_build.json, 始终存在
    tb->addSeparator();
    QAction* a_runner = new QAction("📊 任务运行器", this);
    a_runner->setToolTip("打开独立任务运行器窗口 (仿 ac_task_runner_gui.py)\n"
                        "左侧 sub-task 列表 + 右侧实时 log + ⚡ 当前执行 + 详情区\n"
                        "底部按钮: Stop / 复制 log / 打开 log / 启动 cloud_main");
    a_runner->setData("open_task_runner");
    a_runner->setShortcut(QKeySequence("Ctrl+R"));
    tb->addAction(a_runner);
    actions_["open_task_runner"] = a_runner;
    connect(a_runner, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
}

void AbMainWindow::buildMainButtons() {
    if (cfg_.buttons.empty()) return;
    // 在中心 widget 的进度条下面加按钮行
    QWidget* cw = centralWidget();
    if (!cw) return;
    QVBoxLayout* vl = qobject_cast<QVBoxLayout*>(cw->layout());
    if (!vl) return;

    QHBoxLayout* row = new QHBoxLayout();
    for (const auto& b : cfg_.buttons) {
        QPushButton* btn = new QPushButton(b.label, this);
        if (!b.tooltip.isEmpty()) btn->setToolTip(b.tooltip);
        if (!b.color.isEmpty())   btn->setProperty("role", b.color);
        if (!b.enabled)           btn->setEnabled(false);
        btn->setProperty("abId", b.id);
        connect(btn, &QPushButton::clicked, this, &AbMainWindow::onActionTriggered);
        row->addWidget(btn);
        buttons_[b.id] = btn;
    }
    row->addStretch(1);
    vl->addLayout(row);
}

void AbMainWindow::onActionTriggered() {
    QObject* s = sender();
    QString id;
    if (auto* a = qobject_cast<QAction*>(s)) {
        id = a->data().toString();
    } else if (auto* b = qobject_cast<QPushButton*>(s)) {
        id = b->property("abId").toString();
    }
    if (id.isEmpty()) return;
    log("debug", QString("触发动作: %1").arg(id));

    // 特殊 id 处理
    if (id == "run_selected") { onRunSelectedTask(); return; }
    if (id == "run_auto")     { onRunAuto(); return; }
    if (id == "stop")         { onStop(); return; }
    if (id == "run_cloud")    { onRunCloud(); return; }
    if (id == "run_cloud_gl") { onRunCloudGL(); return; }
    if (id == "build_and_run"){ onBuildAndRun(); return; }
    if (id == "build_and_run_gl") { onBuildAndRunGL(); return; }
    if (id == "open_ar") { onOpenAR(); return; }
    if (id == "toggle_theme") { onToggleTheme(); return; }
    // 2026-09-02: theme:dark/light/solarized/nord
    if (id.startsWith("theme:")) {
        QString name = id.mid(6);  // "dark" / "light" / "solarized" / "nord"
        AbTheme::Kind k = AbTheme::parse(name);
        AbTheme::apply(static_cast<int>(k));
        cfg_.theme = name;  // 同步到配置
        if (sb_right_) sb_right_->setText(QString("Qt=%1 | 主题=%2").arg(qApp ? "Qt5/6" : "?", name));
        log("ok", QString("主题切换: %1").arg(AbTheme::displayName(static_cast<int>(k))));
        return;
    }
    if (id == "toggle_log")   {
        // 2026-09-16 v3: log dock 已移除, no-op
        return;
    }
    if (id == "open_inspector") { onOpenInspector(); return; }
    if (id == "open_task_runner") { onOpenTaskRunner(); return; }
    if (id == "about")        { onAbout(); return; }
    if (id == "quit")         { onQuit(); return; }
    if (id == "open_ght")     {
        // 跨进程: 调 ac ght (用探测到的绝对路径, 避免桌面 GUI PATH 不全)
        if (ac_binary_.isEmpty()) {
            log("err", "✗ ac 未找到, 没法打开 Token 管理 (请装 ai_tools 并确认 ~/.local/bin 在 PATH)");
            return;
        }
        log("info", QString("→ 调 %1 ght --no-check").arg(ac_binary_));
        qint64 pid = 0;
        if (QProcess::startDetached(ac_binary_, QStringList() << "ght" << "--no-check", QDir::homePath(), &pid)) {
            log("ok", QString("✓ Token 管理已启动, pid=%1").arg(pid));
        } else {
            log("err", "✗ 启动 ac ght 失败");
        }
        return;
    }
    if (id == "tts") {
        bool ok = false;
        QString text = QInputDialog::getText(
            this,
            "TTS 播报",
            "请输入要播报的内容:",
            QLineEdit::Normal,
            "ab 工具链就绪",
            &ok
        );
        if (!ok || text.trimmed().isEmpty()) {
            log("info", "TTS 已取消");
            return;
        }
        log("info", QString("→ TTS: %1").arg(text));
        speakTextAsync(text, true);
        return;
    }
    if (id == "bak") {
        if (ac_binary_.isEmpty()) {
            log("err", "✗ ac 未找到, 备份功能不可用");
            return;
        }
        log("info", QString("→ 调 %1 bak %2").arg(ac_binary_).arg(cfg_.cwd));
        QProcess::startDetached(ac_binary_, QStringList() << "bak" << cfg_.cwd);
        return;
    }
    if (id == "proxy_test") {
        // 调 ac ght-cli --proxy-test, 检测 GitHub API 代理是否通
        if (ac_binary_.isEmpty()) {
            log("err", "✗ ac 未找到, 没法跑代理测试");
            return;
        }
        log("info", QString("→ 调 %1 ght-cli --proxy-test").arg(ac_binary_));
        QProcess::startDetached(ac_binary_, QStringList() << "ght-cli" << "--proxy-test", QDir::homePath());
        return;
    }
    if (id == "diag") { runTaskByName("diag"); return; }
    if (id == "view-log") { runTaskByName("view-log"); return; }
    if (id == "build+deploy") { runTaskByName("build+deploy"); return; }

    // 通用: 找 button/toolbar/menu 定义, 按 task / cmd 跑
    for (const auto& b : cfg_.toolbar) {
        if (b.id == id) {
            if (!b.task.isEmpty()) runTaskByName(b.task);
            else if (!b.cmd.isEmpty()) runCmd(b.cmd, id);
            return;
        }
    }
    for (const auto& b : cfg_.buttons) {
        if (b.id == id) {
            if (!b.task.isEmpty()) runTaskByName(b.task);
            else if (!b.cmd.isEmpty()) runCmd(b.cmd, id);
            return;
        }
    }
    // 2026-09-02: menus 也支持 task/cmd 字段
    for (const auto& md : cfg_.menus) {
        for (const auto& mi : md.items) {
            if (mi.id == id) {
                if (!mi.task.isEmpty()) runTaskByName(mi.task);
                else if (!mi.cmd.isEmpty()) runCmd(mi.cmd, id);
                else log("warn", QString("菜单项 %1 没有 task/cmd").arg(id));
                return;
            }
        }
    }
    log("warn", QString("未定义的动作: %1").arg(id));
}

void AbMainWindow::onRunSelectedTask() {
    // 2026-09-16 v4: "跑选中" 改去 InspectorWindow 任务 tab 找
    if (!inspector_window_) {
        log("warn", "任务检查器窗口未打开, 先按 Ctrl+I 打开 [任务检查器] 窗口");
        return;
    }
    auto* ins = inspector_window_->inspector();
    if (!ins) {
        log("warn", "任务检查器未初始化");
        return;
    }
    auto* it = ins->selectedTaskItem();
    if (!it) {
        log("warn", "没选中任务 (在 [任务检查器] 窗口 → [📋 任务] tab 选中一行, 再按 F5)");
        return;
    }
    QString name = it->text(1);  // 列 1 = 任务名
    runTaskByName(name);
}

void AbMainWindow::onInspectorRunTask(const QString& task_name) {
    // 2026-09-08 v2: 桥接 inspector requestRunTask signal → runTaskByName
    //   双击任务 tab 行触发, on_done = nullptr (非 auto 链)
    runTaskByName(task_name);
}

void AbMainWindow::onRunAuto() {
    if (cfg_.auto_chain.isEmpty()) {
        log("warn", "auto 链为空 (在 ai_build.json 的 auto 字段定义)");
        return;
    }
    log("task", QString("跑 auto 链: %1").arg(cfg_.auto_chain.join(" → ")));
    auto_queue_ = cfg_.auto_chain;
    auto_index_ = 0;
    runNextInAuto();
}

void AbMainWindow::runNextInAuto() {
    if (auto_index_ >= auto_queue_.size()) {
        log("ok", "auto 链全部完成 ✓");
        speakTextAsync("全部完成");
        return;
    }
    QString name = auto_queue_[auto_index_++];
    log("info", QString("[auto %1/%2] %3").arg(auto_index_).arg(auto_queue_.size()).arg(name));
    runTaskByName(name, [this]() { runNextInAuto(); });
}

QString AbMainWindow::resolveTaskCmd(const QString& task_name,
                                    QStringList* out_subs,
                                    QStringList* out_descs) const {
    for (const auto& t : cfg_.tasks) {
        if (t.name == task_name) {
            if (out_subs) *out_subs = t.sub_cmds;
            if (out_descs) *out_descs = t.sub_descs;
            return t.cmd;
        }
    }
    return QString();
}

// 2026-09-09: 窗口标题动态显示当前命令
//   空参数: 恢复 cfg_.title
//   非空:   "<cfg_.title> — ▶ <task_name> (<cmd>)"
//   例: 跑 view-log task 时标题变为 "godot_ui_linux — ab — ▶ view-log (tail -f /tmp/cloud_main.log)"
void AbMainWindow::updateWindowTitle(const QString& current_cmd) {
    current_cmd_ = current_cmd;
    if (current_cmd.isEmpty()) {
        setWindowTitle(cfg_.title);
    } else {
        // cmd 太长截断到 60 字符 (标题栏宽度有限)
        QString short_cmd = current_cmd;
        if (short_cmd.size() > 60) {
            short_cmd = short_cmd.left(57) + "...";
        }
        setWindowTitle(QString("%1 — ▶ %2 (%3)")
                       .arg(cfg_.title, current_task_, short_cmd));
    }
}

void AbMainWindow::runTaskByName(const QString& name, std::function<void()> on_done) {
    if (runner_->isRunning()) {
        log("warn", "已有 task 在跑, 请先停止");
        return;
    }
    // 2026-09-16: 查 task 是 sub-task list 模式还是单 cmd 模式
    QStringList sub_cmds, sub_descs;
    QString single_cmd;
    bool is_sub_mode = false;
    for (const auto& t : cfg_.tasks) {
        if (t.name == name) {
            if (!t.sub_cmds.isEmpty()) {
                sub_cmds   = t.sub_cmds;
                sub_descs  = t.sub_descs;
                is_sub_mode = true;
            } else {
                single_cmd = t.cmd;
            }
            break;
        }
    }
    if (!is_sub_mode && single_cmd.isEmpty()) {
        log("err", QString("未知 task: %1").arg(name));
        if (on_done) on_done();
        return;
    }

    log("task", QString("▶ 跑 task [%1]: %2 (mode=%3)")
        .arg(name, is_sub_mode ? QString("sub-task list (%1 步)").arg(sub_cmds.size()) : "single cmd",
             is_sub_mode ? "sub" : "single"));
    current_task_ = name;
    panel_task_name_ = name;
    // 标题: sub-task 模式显示 "task_name (N sub-tasks)"
    if (is_sub_mode) {
        updateWindowTitle(QString("(%1 sub-tasks)").arg(sub_cmds.size()));
    } else {
        updateWindowTitle(single_cmd);
    }
    current_on_done_ = on_done;
    // 2026-09-16 v3: 重置中央面板 UI (顶部 header + 进度条 + sub-task 列表 + 当前执行区)
    panelResetUi(is_sub_mode ? sub_cmds.size() : 1);
    if (is_sub_mode) {
        // 预填所有 sub-task 列表项
        for (int i = 0; i < sub_cmds.size(); ++i) {
            QString desc = (i < sub_descs.size()) ? sub_descs[i] : QString();
            panelAppendSubTaskItem(i + 1, sub_cmds.size(), desc, sub_cmds[i]);
        }
    } else {
        panelAppendSubTaskItem(1, 1, QString(), single_cmd);
    }
    if (inspector_window_) {
        if (auto* ins = inspector_window_->inspector()) ins->onTaskStarted(name);
    }
    if (auto a = actions_.value("run_selected")) a->setEnabled(false);
    if (auto a = actions_.value("run_auto"))    a->setEnabled(false);
    if (auto a = actions_.value("stop"))        a->setEnabled(true);
    if (auto b = buttons_.value("run_selected")) b->setEnabled(false);
    if (auto b = buttons_.value("run_auto"))    b->setEnabled(false);
    if (auto b = buttons_.value("stop"))        b->setEnabled(true);
    if (is_sub_mode) {
        runner_->runSubTasks(name, sub_cmds, sub_descs, cfg_.cwd);
    } else {
        runner_->run(name, single_cmd, cfg_.cwd);
    }
}

void AbMainWindow::runCmd(const QString& cmd, const QString& task_name) {
    if (runner_->isRunning()) {
        log("warn", "已有 task 在跑");
        return;
    }
    log("task", QString("▶ 跑 cmd [%1]: %2").arg(task_name, cmd));
    current_task_ = task_name;
    current_cmd_  = cmd;
    panel_task_name_ = task_name;
    updateWindowTitle(cmd);   // 2026-09-09: 窗口标题显示当前命令
    current_on_done_ = nullptr;
    // 2026-09-16 v3: 中央面板 reset (单 cmd 模式)
    panelResetUi(1);
    panelAppendSubTaskItem(1, 1, QString(), cmd);
    runner_->run(task_name, cmd, cfg_.cwd);
}

void AbMainWindow::onStop() {
    if (runner_->isRunning()) {
        log("warn", "停止当前 task");
        runner_->stop();
    } else {
        log("info", "没有 task 在跑");
    }
}

void AbMainWindow::onOutput(const QString& task_name, const QString& line) {
    if (line.isEmpty()) return;
    QString ll = line.toLower();
    QString level = "info";
    if (ll.contains("error") || ll.contains("fatal") || ll.contains("failed")) level = "err";
    else if (ll.contains("warning")) level = "warn";
    else if (line.contains("✓") || ll.contains("[ok]")) level = "ok";

    // 2026-09-16 v3: 写入中央面板右侧 log (主路径)
    if (panel_log_view_) {
        // 颜色标记 (▶▶▶ / ✓ / ✗ / 🚀)
        QString esc = line;
        esc.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
        QString color;
        if (line.startsWith("▶▶▶")) color = "#88ccff";
        else if (line.startsWith("✓")) color = "#88ff88";
        else if (line.startsWith("✗")) color = "#ff8888";
        else if (line.startsWith("🚀") || line.contains("===")) color = "#ffaa44";
        if (!color.isEmpty()) {
            panel_log_view_->appendHtml(QString("<span style=\"color:%1;\">%2</span>").arg(color, esc));
        } else {
            panel_log_view_->appendPlainText(line);
        }
        // 自动滚到底
        if (auto* sb = panel_log_view_->verticalScrollBar()) {
            sb->setValue(sb->maximum());
        }
        // 更新 log 摘要
        if (panel_lbl_log_summary_) {
            panel_lbl_log_summary_->setText(
                QString("📄 实时 log (stdout, %1 行)").arg(panel_log_view_->blockCount()));
        }
    }

    // 兼容: 也写到 sb_left_ 状态栏 (log_dock_ 已移除, 不写 dock)
    if (sb_left_) {
        sb_left_->setText(QString("[%1] %2").arg(task_name, line.left(120)));
    }
}

void AbMainWindow::onFinished(const QString& task_name, int exit_code, double elapsed) {
    log(exit_code == 0 ? "ok" : "err",
        QString("[%1] 退出码 %2  耗时 %3s").arg(task_name).arg(exit_code).arg(elapsed, 0, 'f', 1));

    // 2026-09-16 v3: 更新中央面板顶部状态 (status badge + elapsed 停 + 进度条满)
    if (panel_lbl_status_) {
        if (exit_code == 0) {
            panel_lbl_status_->setText("✓ Done");
            panel_lbl_status_->setStyleSheet(
                "background-color: #44aa44; color: #fff; padding: 4px 14px; "
                "border-radius: 4px; border: 1px solid #888; font-weight: bold;");
            if (panel_progress_) {
                panel_progress_->setValue(panel_progress_->maximum());
                panel_progress_->setFormat(QString("✓ %1 / %1").arg(panel_progress_->maximum()));
            }
        } else {
            panel_lbl_status_->setText(QString("✗ Fail (rc=%1)").arg(exit_code));
            panel_lbl_status_->setStyleSheet(
                "background-color: #aa4444; color: #fff; padding: 4px 14px; "
                "border-radius: 4px; border: 1px solid #888; font-weight: bold;");
        }
    }
    if (panel_elapsed_timer_) panel_elapsed_timer_->stop();
    if (panel_current_dt_timer_) panel_current_dt_timer_->stop();
    if (panel_lbl_elapsed_) {
        int m = int(elapsed) / 60;
        int s = int(elapsed) % 60;
        panel_lbl_elapsed_->setText(QString("⏱ %1:%2")
            .arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
    }
    if (panel_btn_stop_) panel_btn_stop_->setEnabled(false);

    if (inspector_window_) {
        if (auto* ins = inspector_window_->inspector()) ins->onTaskFinished(task_name, exit_code, elapsed);
    }
    if (auto a = actions_.value("run_selected")) a->setEnabled(true);
    if (auto a = actions_.value("run_auto"))    a->setEnabled(true);
    if (auto a = actions_.value("stop"))        a->setEnabled(false);
    if (auto b = buttons_.value("run_selected")) b->setEnabled(true);
    if (auto b = buttons_.value("run_auto"))    b->setEnabled(true);
    if (auto b = buttons_.value("stop"))        b->setEnabled(false);
    speakTaskFinished(task_name, exit_code);
    updateWindowTitle();   // 2026-09-09: 恢复窗口标题 (清掉当前命令)

    // 编译类 task 成功 → 找 cloud_main
    if (exit_code == 0 && (task_name == "build+deploy" || task_name == "build-only")) {
        if (!cfg_.run_after_build.binary_path.isEmpty()) {
            QString bin = findRunBinary();
            if (!bin.isEmpty()) {
                cloud_binary_ = bin;
                enableRunCloudButton(true);
                log("ok", QString("✓ %1 已就绪").arg(QFileInfo(bin).fileName()));
                // auto_run 模式: 立即跑
                if (cfg_.run_after_build.auto_run) {
                    QTimer::singleShot(500, this, &AbMainWindow::onRunCloud);
                }
            }
        }
    }

    if (current_on_done_) {
        auto cb = current_on_done_;
        current_on_done_ = nullptr;
        cb();
    }
}

void AbMainWindow::speakTextAsync(const QString& text, bool log_when_disabled) {
    if (text.trimmed().isEmpty()) return;
    if (spd_say_binary_.isEmpty()) {
        if (log_when_disabled) log("warn", "TTS 不可用: 未找到 spd-say");
        return;
    }
    if (!tts_enabled_) {
        if (log_when_disabled) log("info", QString("TTS 已关闭, 跳过播报: %1").arg(text));
        return;
    }
    if (!QProcess::startDetached(spd_say_binary_, QStringList() << "-l" << "zh" << text)) {
        if (log_when_disabled) log("warn", QString("TTS 启动失败: %1").arg(text));
    }
}

void AbMainWindow::speakTaskFinished(const QString& task_name, int exit_code) const {
    if (spd_say_binary_.isEmpty() || !tts_enabled_) return;
    QString text = (exit_code == 0)
        ? QString("任务 %1 成功").arg(task_name)
        : QString("任务 %1 失败").arg(task_name);
    QProcess::startDetached(spd_say_binary_, QStringList() << "-l" << "zh" << text);
}

void AbMainWindow::onError(const QString& task_name, int err) {
    log("err", QString("[%1] QProcess 错误码 %2").arg(task_name).arg(err));
    updateWindowTitle();   // 2026-09-09: 恢复窗口标题
}

QString AbMainWindow::findRunBinary() const {
    if (cfg_.run_after_build.binary_path.isEmpty()) return QString();
    // binary_path 可能是 "bin/Debug/cloud_main" 或绝对路径
    QFileInfo fi(cfg_.run_after_build.binary_path);
    if (fi.isAbsolute() && fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    QString rel = fi.fileName();
    for (const char* sub : {"Debug", "Release"}) {
        QString p = cfg_.cwd + "/bin/" + sub + "/" + rel;
        QFileInfo fi2(p);
        if (fi2.exists() && fi2.isExecutable()) return fi2.absoluteFilePath();
    }
    return QString();
}

QString AbMainWindow::findTool(const QString& name) const {
    // which-like: 找 name 的绝对路径. 找不到返回 ''.
    // 1. PATH 探测
    QByteArray path_env = qgetenv("PATH");
    if (!path_env.isEmpty()) {
        const QStringList dirs = QString::fromLocal8Bit(path_env).split(':', Qt::SkipEmptyParts);
        for (const QString& d : dirs) {
            QString cand = d + '/' + name;
            QFileInfo fi(cand);
            if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
        }
    }
    // 2. 常见 PATH 兜底 (桌面 GUI 经常没有 ~/.local/bin)
    const QString home = QDir::homePath();
    const QStringList fallbacks = {
        home + "/.local/bin/" + name,
        "/usr/local/bin/" + name,
        "/usr/bin/" + name,
        "/bin/" + name,
    };
    for (const QString& cand : fallbacks) {
        QFileInfo fi(cand);
        if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    }
    return QString();
}

void AbMainWindow::enableRunCloudButton(bool en) {
    if (auto a = actions_.value("run_cloud")) a->setEnabled(en);
    if (auto b = buttons_.value("run_cloud")) b->setEnabled(en);
    if (auto a = actions_.value("run_cloud_gl")) a->setEnabled(en);
    if (auto b = buttons_.value("run_cloud_gl")) b->setEnabled(en);
}

void AbMainWindow::onRunCloud() {
    if (cloud_binary_.isEmpty()) {
        cloud_binary_ = findRunBinary();
        if (cloud_binary_.isEmpty()) {
            QMessageBox::warning(this, "未找到",
                QString("%1 不存在, 请先跑 build+deploy 编译").arg(cfg_.run_after_build.binary_path));
            return;
        }
    }
    log("task", QString("🚀 启动 %1 (后台)").arg(QFileInfo(cloud_binary_).fileName()));
    QStringList args = cfg_.run_after_build.args;
    // cloud_main 是长跑 GUI 程序, 用 startDetached 后台跑, 不阻塞 ab GUI.
    // 早期版本用 p->start + waitForFinished(-1) 会卡住 GUI, 而且 cloud_main 退出时
    // 才记一条 rc, 中间用户看不到任何 stderr. 这里改成 detached, 立刻 rc=0 表示启动成功.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // 2026-09-02 FIX v9.0: 跟 ui.py cmd_launch_cloud_main 完全一致启动链
    //   之前 ab 在 onRunCloud 里硬塞 VK_ICD_FILENAMES=nvidia_icd.json + SDL_VIDEODRIVER=wayland,
    //   ui.py 完全没塞这两个. 同样的 binary + lib + tscn, 两边行为却不一样, 真因就是
    //   ab 多塞了 env, 跟 godot_vulkan.md 规则的"不要硬塞, 走系统默认"违背.
    //   修法:
    //     - 删 VK_ICD_FILENAMES 默认值 (交给系统 default ICD)
    //     - 删 SDL_VIDEODRIVER 默认值 (走 $DISPLAY/$WAYLAND_DISPLAY)
    //     - env 完全从 cfg_.run_after_build.env 读, 不加料
    //   ai_build.json 现在跟 ui.py 一样只设 6 个 env: BVWS_EMBED / BVWS_FORCE_PROJECT /
    //   RENDER_MODE / ROS_DISTRO / LD_LIBRARY_PATH (含 $VAR 展开) / SDL_AUDIODRIVER.
    for (auto it = cfg_.run_after_build.env.constBegin();
         it != cfg_.run_after_build.env.constEnd(); ++it) {
        QString val = it.value();
        // $VAR 展开: "$LD_LIBRARY_PATH:/extra" → "<current>:extra"
        // 支持多次出现同一个 $VAR
        static QRegularExpression reVar(R"(\$([A-Za-z_][A-Za-z0-9_]*))");
        QRegularExpressionMatchIterator mi = reVar.globalMatch(val);
        int offset = 0;
        QString expanded;
        while (mi.hasNext()) {
            auto m = mi.next();
            expanded += val.mid(offset, m.capturedStart() - offset);
            QString varName = m.captured(1);
            expanded += env.value(varName);  // systemEnvironment 或之前 set 的
            offset = m.capturedEnd();
        }
        expanded += val.mid(offset);
        env.insert(it.key(), expanded);
    }
    // Qt5 QProcess::startDetached 没有 (program, args, env, cwd, pid) 5参 overload,
    // 只能继承父进程 env. 用 qputenv 把 env 全量塞到父进程, fork 时子进程继承.
    // 注意: 这会污染 ab 自身进程的 env, 启动 cloud_main 后 ab 也有这些 env.
    // 副作用: 不影响 ab GUI 功能 (已经启动完了).
    log("info", QString("  env: 注入 %1 个变量 (从 cfg_.run_after_build.env)").arg(env.keys().size()));
    for (const QString& k : env.keys()) {
        qputenv(k.toUtf8().constData(), env.value(k).toUtf8());
    }
    // 2026-09-02: run_after_build.cwd 优先于 cfg_.cwd
    //   ui.py 启 cloud_main 用 bin/Debug 作 cwd (dlopen ./libworkspace_v7.so 时相对路径解析正确).
    //   之前 ab GUI 用 cfg_.cwd = 项目根, 导致 cloud_main 找不到 sidecar, 3D 区域空白.
    QString child_cwd = cfg_.run_after_build.cwd.isEmpty() ? cfg_.cwd : cfg_.run_after_build.cwd;
    log("info", QString("  cwd: %1").arg(child_cwd));
    qint64 pid = 0;
    
    // 2026-09-16 fix: Vulkan 模式也优先通过 start_cloud_main.sh 启动 (确保 LD_LIBRARY_PATH 注入)
    QString sh_path = "/home/bv/code/godot_ui_linux/start_cloud_main.sh";
    if (QFile::exists(sh_path)) {
        log("info", QString("  走 start_cloud_main.sh 路径"));
        QStringList sh_args;
        sh_args << sh_path;
        if (QProcess::startDetached("/bin/bash", sh_args, child_cwd, &pid)) {
            log("ok", QString("✓ cloud_main 已通过 .sh 启动 (Vulkan), pid=%1").arg(pid));
            log("info", QString("  stdout/stderr 进自己的终端/日志文件"));
            speakTextAsync(QString("启动 %1").arg(QFileInfo(cloud_binary_).fileName()));
            return;
        }
    }

    bool ok = QProcess::startDetached(cloud_binary_, args, child_cwd, &pid);
    if (ok) {
        log("ok", QString("✓ %1 已在后台启动 (Vulkan), pid=%2").arg(QFileInfo(cloud_binary_).fileName()).arg(pid));
        log("info", QString("  stdout/stderr 直接进自己的终端/日志文件, 不进 ab 日志 dock"));
        log("info", QString("  停止: kill %1  或  pkill -f %2").arg(pid).arg(QFileInfo(cloud_binary_).fileName()));
        // 2026-09-16 fix71: 状态栏显示渲染模式
        if (sb_render_) {
            sb_render_->setText("渲染模式: Vulkan (Forward+)");
            sb_render_->setStyleSheet("color: #2980b9; font-size: 12px; padding: 0 8px; font-weight: bold;");
        }
        speakTextAsync(QString("启动 %1").arg(QFileInfo(cloud_binary_).fileName()));
    } else {
        log("err", QString("✗ 启动失败: %1").arg(cloud_binary_));
    }
}

void AbMainWindow::onRunCloudGL() {
    if (cloud_binary_.isEmpty()) {
        cloud_binary_ = findRunBinary();
        if (cloud_binary_.isEmpty()) {
            QMessageBox::warning(this, "未找到",
                QString("%1 不存在, 请先跑 build+deploy 编译").arg(cfg_.run_after_build.binary_path));
            return;
        }
    }
    log("task", QString("🚀 启动 %1 (OpenGL 兼容模式)").arg(QFileInfo(cloud_binary_).fileName()));
    
    // 给 args 追加 --gl
    QStringList args = cfg_.run_after_build.args;
    args << "--gl";

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = cfg_.run_after_build.env.constBegin();
         it != cfg_.run_after_build.env.constEnd(); ++it) {
        QString val = it.value();
        QString expanded;
        int offset = 0;
        QRegularExpression re("\\$([A-Za-z_][A-Za-z0-9_]*)");
        QRegularExpressionMatchIterator i = re.globalMatch(val);
        while (i.hasNext()) {
            QRegularExpressionMatch m = i.next();
            expanded += val.mid(offset, m.capturedStart() - offset);
            QString varName = m.captured(1);
            expanded += env.value(varName);  
            offset = m.capturedEnd();
        }
        expanded += val.mid(offset);
        env.insert(it.key(), expanded);
    }
    
    log("info", QString("  env: 注入 %1 个变量").arg(env.keys().size()));
    for (const QString& k : env.keys()) {
        qputenv(k.toUtf8().constData(), env.value(k).toUtf8());
    }
    
    QString child_cwd = cfg_.run_after_build.cwd.isEmpty() ? cfg_.cwd : cfg_.run_after_build.cwd;
    log("info", QString("  cwd: %1").arg(child_cwd));
    qint64 pid = 0;
    // 2026-09-16 fix53: 兼容模式优先通过 start_cloud_main.sh --gl 启动 (有完整 env 注入),
    //   fallback 才走 cfg_.run_after_build 直接启动 cloud_main
    QString sh_path = "/home/bv/code/godot_ui_linux/start_cloud_main.sh";
    if (QFile::exists(sh_path)) {
        log("info", QString("  走 start_cloud_main.sh --gl 路径"));
        QStringList sh_args;
        sh_args << sh_path << "--gl";
        if (QProcess::startDetached("/bin/bash", sh_args, child_cwd, &pid)) {
            log("ok", QString("✓ cloud_main 已通过 .sh 启动 (OpenGL), pid=%1").arg(pid));
            // 2026-09-16 fix71: 状态栏显示渲染模式
            if (sb_render_) {
                sb_render_->setText("渲染模式: OpenGL3 (Compatibility)");
                sb_render_->setStyleSheet("color: #e67e22; font-size: 12px; padding: 0 8px; font-weight: bold;");
            }
            speakTextAsync(QString("启动兼容模式"));
            return;
        }
    }
    // fallback: 直接启 binary
    bool ok = QProcess::startDetached(cloud_binary_, args, child_cwd, &pid);
    if (ok) {
        log("ok", QString("✓ %1 已在后台启动 (OpenGL fallback), pid=%2").arg(QFileInfo(cloud_binary_).fileName()).arg(pid));
        speakTextAsync(QString("启动兼容模式"));
    } else {
        log("err", QString("✗ 启动失败: %1").arg(cloud_binary_));
    }
}

void AbMainWindow::onBuildAndRun() {
    log("task", "⚡ 编译并启动");
    QString t = !resolveTaskCmd("build+deploy").isEmpty() ? "build+deploy" : "build-only";
    if (t.isEmpty()) {
        QMessageBox::warning(this, "未配置", "ai_build.json 缺 build+deploy 任务");
        return;
    }
    cfg_.run_after_build.auto_run = true;
    runTaskByName(t);
}

void AbMainWindow::onBuildAndRunGL() {
    log("task", "⚡ 编译并启动 (OpenGL)");
    QString t = !resolveTaskCmd("build+deploy").isEmpty() ? "build+deploy" : "build-only";
    if (t.isEmpty()) {
        QMessageBox::warning(this, "未配置", "ai_build.json 缺 build+deploy 任务");
        return;
    }
    cfg_.run_after_build.auto_run = true;
    if (!cfg_.run_after_build.args.contains("--gl")) {
        cfg_.run_after_build.args << "--gl";
    }
    runTaskByName(t);
}

void AbMainWindow::onOpenAR() {
    log("task", "🎯 启动 AR 运行工具 (ac ar)");
    if (ac_binary_.isEmpty()) {
        log("err", QString::fromUtf8("✗ ac 未找到"));
        return;
    }
    QString config_arg = cfg_.cwd + "/ai_build.json";
    qint64 pid = 0;
    QStringList args;
    args << "ar" << "--config" << config_arg;
    if (QProcess::startDetached(ac_binary_, args, cfg_.cwd, &pid)) {
        log("ok", QString("✓ AR 工具已启动, pid=%1").arg(pid));
    } else {
        log("err", "启动 AR 工具失败");
    }
}

void AbMainWindow::onToggleTheme() {
    // dark → light, light → dark
    auto cur = AbTheme::current();
    AbTheme::Kind nxt = (cur == AbTheme::Dark) ? AbTheme::Light : AbTheme::Dark;
    AbTheme::apply(static_cast<int>(nxt));
    log("ok", QString("主题切换: %1").arg(nxt == AbTheme::Dark ? "暗色" : "浅色"));
}

void AbMainWindow::onToggleLogDock(bool checked) {
    // 2026-09-16 v3: log dock 已移除, 此项 no-op (保持兼容, log 现在在中央面板)
    Q_UNUSED(checked);
}

// 2026-09-16: 打开独立任务运行器窗口 (仿 ac_task_runner_gui.py)
void AbMainWindow::onOpenTaskRunner() {
    if (task_runner_win_) {
        // 已存在, 只是激活 + 切到前台
        if (task_runner_win_->isMinimized()) task_runner_win_->showNormal();
        task_runner_win_->raise();
        task_runner_win_->activateWindow();
        return;
    }
    // 决定用哪个 task 显示:
    //   1. 当前正在跑的任务 (current_task_)
    //   2. cfg_.tasks 第一个
    //   3. "build+deploy" (兜底, 最常用)
    QString task_name = current_task_;
    QStringList sub_cmds, sub_descs;
    auto find_task_lists = [&](const QString& nm) {
        for (const auto& t : cfg_.tasks) {
            if (t.name == nm) {
                if (!t.sub_cmds.isEmpty()) {
                    sub_cmds  = t.sub_cmds;
                    sub_descs = t.sub_descs;
                } else if (!t.cmd.isEmpty()) {
                    sub_cmds  = QStringList{t.cmd};
                    sub_descs = QStringList{QString()};
                }
                return true;
            }
        }
        return false;
    };
    bool found = !task_name.isEmpty() && find_task_lists(task_name);
    if (!found && !cfg_.tasks.empty()) {
        task_name = cfg_.tasks.front().name;
        found = find_task_lists(task_name);
    }
    if (!found) {
        task_name = "build+deploy";
        sub_cmds  = QStringList{"echo \"(未配置 ai_build.json tasks, 这是 demo)\""};
        sub_descs = QStringList{QString("demo: 等待 ac --task %1").arg(task_name)};
    }

    task_runner_win_ = new TaskRunnerWindow(
        runner_, task_name, sub_cmds, sub_descs, this);
    // 关闭时清指针 (避免悬挂)
    connect(task_runner_win_, &QObject::destroyed, this, [this]() {
        task_runner_win_ = nullptr;
    });
    task_runner_win_->show();
    log("ok", QString("📊 任务运行器已打开 — %2").arg(task_name));
}

// 2026-09-16 v4: 打开独立 InspectorWindow (任务检查器窗口)
//   - 如果已开, 只是激活
//   - 第一次开, lazy 创建 + 桥接 signal (inspector 双击任务 → runTaskByName)
//   - inspector_window_ 关闭时不退出 app (setAttribute WA_DeleteOnClose=false)
void AbMainWindow::onOpenInspector() {
    if (inspector_window_) {
        if (inspector_window_->isMinimized()) inspector_window_->showNormal();
        inspector_window_->raise();
        inspector_window_->activateWindow();
        return;
    }
    inspector_window_ = new InspectorWindow(runner_, cfg_, cfg_.cwd, this);
    // 桥接 inspector requestRunTask → runTaskByName
    connect(inspector_window_, &InspectorWindow::requestRunTask,
            this, &AbMainWindow::onInspectorRunTask);
    // 关闭时清指针 (避免悬挂)
    connect(inspector_window_, &QObject::destroyed, this, [this]() {
        inspector_window_ = nullptr;
    });
    inspector_window_->show();
    log("ok", "📋 任务检查器已打开 (独立窗口 — 📋 任务 / ⚙️ 进程 / 🔧 sub-task)");
}

void AbMainWindow::onAbout() {
    QMessageBox::information(this, "关于 ab",
        QString("ab — AI Build 编译/调试 GUI (C++ Qt5/6)\n"
                "版本: 1.0.0\n"
                "项目: %1\n"
                "任务: %2 个\n"
                "主题: %3\n\n"
                "ab 是通用的 AI 辅助构建工具,\n"
                "通过 ai_build.json 配置文件驱动 UI.\n"
                "想改按钮/菜单, 改 ai_build.json 的 ui 段即可.")
        .arg(cfg_.cwd)
        .arg(static_cast<int>(cfg_.tasks.size()))
        .arg(cfg_.theme));
}

void AbMainWindow::onQuit() {
    close();
}

void AbMainWindow::log(const QString& level, const QString& msg) {
    // 2026-09-16 v3: log dock 已移除, 仅写到状态栏
    // 同步状态栏
    if (level == "err") {
        if (sb_left_) {
            sb_left_->setText(QString("✗ %1").arg(msg));
            sb_left_->setProperty("level", "err");
            sb_left_->style()->unpolish(sb_left_);
            sb_left_->style()->polish(sb_left_);
        }
    } else if (level == "ok") {
        if (sb_left_) {
            sb_left_->setText(QString("✓ %1").arg(msg));
            sb_left_->setProperty("level", "ok");
            sb_left_->style()->unpolish(sb_left_);
            sb_left_->style()->polish(sb_left_);
        }
    }
}

void AbMainWindow::reloadConfig() {
    // TODO: 重读 ai_build.json, 重建 UI
}

// =====================================================================
// 2026-09-16 v3: 中央面板 sub-task UI 实现 (仿 ac_task_runner_gui.py)
// =====================================================================
bool AbMainWindow::panelFindTaskLists(const QString& name, QStringList& cmds, QStringList& descs) const {
    for (const auto& t : cfg_.tasks) {
        if (t.name == name) {
            if (!t.sub_cmds.isEmpty()) {
                cmds  = t.sub_cmds;
                descs = t.sub_descs;
                return true;
            }
            if (!t.cmd.isEmpty()) {
                cmds  = QStringList{t.cmd};
                descs = QStringList{QString()};
                return true;
            }
            return false;
        }
    }
    return false;
}

void AbMainWindow::panelResetUi(int total) {
    panel_total_sub_ = std::max(total, 1);
    panel_selected_sub_idx_ = 0;
    panel_sub_states_.clear();
    panel_task_start_ms_ = QDateTime::currentMSecsSinceEpoch();
    panel_current_sub_start_ms_ = 0;
    panel_task_name_ = current_task_;

    // 清 sub-task 列表
    if (panel_sub_list_) panel_sub_list_->clear();

    // 进度条
    if (panel_progress_) {
        panel_progress_->setRange(0, panel_total_sub_);
        panel_progress_->setValue(0);
        panel_progress_->setFormat(QString("0 / %1  (sub-task)").arg(panel_total_sub_));
    }

    // 顶部 header
    if (panel_lbl_title_) {
        panel_lbl_title_->setText(panel_task_name_.isEmpty() ? "⏸ 暂无任务" : panel_task_name_);
        panel_lbl_title_->setStyleSheet("color: #88ccff; font-size: 14px; font-weight: bold;");
    }
    if (panel_lbl_status_) {
        panel_lbl_status_->setText("⏳ 等待启动");
        panel_lbl_status_->setStyleSheet(
            "background-color: #444; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888; font-weight: bold;");
    }
    if (panel_lbl_elapsed_) panel_lbl_elapsed_->setText("⏱ 00:00");
    panel_elapsed_timer_->start();

    // 当前执行区
    if (panel_lbl_current_idx_) {
        panel_lbl_current_idx_->setText("—");
        panel_lbl_current_idx_->setStyleSheet(
            "background-color: #4682b4; color: #fff; padding: 2px 8px; "
            "border-radius: 3px; font-weight: bold;");
    }
    if (panel_lbl_current_dt_) panel_lbl_current_dt_->setText("⏱ 0s");
    if (panel_txt_current_cmd_) panel_txt_current_cmd_->clear();

    // log 区不清空 (保留上次输出), 用户可手动清
    // 摘要更新
    if (panel_lbl_log_summary_ && panel_log_view_) {
        panel_lbl_log_summary_->setText(
            QString("📄 实时 log (stdout, %1 行)").arg(panel_log_view_->blockCount()));
    }

    // 详情区
    if (panel_lbl_detail_meta_) panel_lbl_detail_meta_->setText("(未选中 sub-task)");
    if (panel_txt_detail_cmd_) panel_txt_detail_cmd_->clear();
    if (panel_btn_open_sub_log_) panel_btn_open_sub_log_->setEnabled(false);
    if (panel_btn_copy_sub_cmd_) panel_btn_copy_sub_cmd_->setEnabled(false);

    // 按钮可用性
    if (panel_btn_stop_) panel_btn_stop_->setEnabled(true);
}

void AbMainWindow::panelAppendSubTaskItem(int idx, int total, const QString& desc, const QString& cmd) {
    if (!panel_sub_list_) return;
    PanelSubState st;
    st.status = "pending";
    st.cmd = cmd;
    st.desc = desc;
    panel_sub_states_[idx] = st;

    QString label = desc.isEmpty()
                    ? QString("  ⏳ [%1/%2] 等待…").arg(idx).arg(total)
                    : QString("  ⏳ [%1/%2] %3").arg(idx).arg(total).arg(desc);
    QListWidgetItem* it = new QListWidgetItem(label, panel_sub_list_);
    it->setForeground(QColor("#888"));
    it->setData(Qt::UserRole, idx);
    if (!desc.isEmpty()) it->setToolTip(desc + "\n$ " + cmd);
    else if (!cmd.isEmpty()) it->setToolTip("$ " + cmd);
}

void AbMainWindow::panelUpdateRunningItem(int idx, int total) {
    if (!panel_sub_list_) return;
    panel_current_sub_start_ms_ = QDateTime::currentMSecsSinceEpoch();
    panel_current_dt_timer_->start();

    if (panel_sub_states_.contains(idx)) {
        panel_sub_states_[idx].status = "running";
    }
    if (idx - 1 < panel_sub_list_->count()) {
        QListWidgetItem* it = panel_sub_list_->item(idx - 1);
        QString desc = panel_sub_states_.value(idx).desc;
        QString cmd  = panel_sub_states_.value(idx).cmd;
        QString l = desc.isEmpty()
                    ? QString("  ⟳ [%1/%2] %3").arg(idx).arg(total).arg(cmd)
                    : QString("  ⟳ [%1/%2] %3").arg(idx).arg(total).arg(desc);
        it->setText(l);
        it->setForeground(QColor("#88ccff"));
        QString tip = QString("sub-task [%1/%2]  跑中…\n").arg(idx).arg(total);
        if (!desc.isEmpty()) tip += "\n📌 " + desc;
        if (!cmd.isEmpty())  tip += "\n$ " + cmd;
        it->setToolTip(tip);
        panel_sub_list_->setCurrentRow(idx - 1);
        panel_sub_list_->scrollToItem(it);
    }
}

void AbMainWindow::panelUpdateFinishedItem(int idx, int total, int rc, double dt) {
    if (!panel_sub_list_) return;
    panel_current_dt_timer_->stop();
    if (panel_sub_states_.contains(idx)) {
        panel_sub_states_[idx].rc = rc;
        panel_sub_states_[idx].dt = dt;
        panel_sub_states_[idx].status = (rc == 0) ? "ok" : "fail";
    }
    if (idx - 1 < panel_sub_list_->count()) {
        QListWidgetItem* it = panel_sub_list_->item(idx - 1);
        QString sym = (rc == 0) ? "✓" : "✗";
        QString desc = panel_sub_states_.value(idx).desc;
        QString cmd  = panel_sub_states_.value(idx).cmd;
        QString l = desc.isEmpty()
                    ? QString("  %1 [%2/%3] rc=%4 %5s  %6")
                          .arg(sym).arg(idx).arg(total).arg(rc).arg(dt, 0, 'f', 1).arg(cmd)
                    : QString("  %1 [%2/%3] rc=%4 %5s  %6")
                          .arg(sym).arg(idx).arg(total).arg(rc).arg(dt, 0, 'f', 1).arg(desc);
        it->setText(l);
        if (rc == 0) it->setForeground(QColor("#88ff88"));
        else         it->setForeground(QColor("#ff8888"));
        QString tip = QString("sub-task [%1/%2]  rc=%3  %4s\n").arg(idx).arg(total).arg(rc).arg(dt, 0, 'f', 2);
        if (!desc.isEmpty()) tip += "\n📌 " + desc;
        if (!cmd.isEmpty())  tip += "\n$ " + cmd;
        it->setToolTip(tip);
    }

    // 进度条: done 数
    if (panel_progress_) {
        int done = 0;
        for (const auto& s : panel_sub_states_) {
            if (s.status == "ok" || s.status == "fail") done++;
        }
        panel_progress_->setValue(done);
        panel_progress_->setFormat(QString("%1 / %2  (sub-task)").arg(done).arg(total));
    }

    // 当前执行区
    if (panel_lbl_current_idx_) {
        panel_lbl_current_idx_->setStyleSheet(
            "background-color: #44aa44; color: #fff; padding: 2px 8px; "
            "border-radius: 3px; font-weight: bold;");
    }
    if (panel_lbl_current_dt_) {
        panel_lbl_current_dt_->setText(QString("⏱ %1s %2").arg(dt, 0, 'f', 1).arg(rc == 0 ? "✓" : "✗"));
    }
}

QString AbMainWindow::panelLogDirForCurrentTask() const {
    QString home = QDir::homePath();
    QString dir = home + "/.cache/ai_tools/task_log/" + panel_task_name_;
    QDir().mkpath(dir);
    return dir;
}

// ---- 接 runner signals ----
void AbMainWindow::onSubStartedPanel(const QString& task_name, int idx, int total,
                                     const QString& desc, const QString& cmd) {
    if (task_name != current_task_) return;
    panelUpdateRunningItem(idx, total);

    // 当前执行区文本
    if (panel_txt_current_cmd_) {
        panel_txt_current_cmd_->setPlainText(desc.isEmpty() ? cmd : desc);
    }
    if (panel_lbl_current_idx_) {
        panel_lbl_current_idx_->setText(QString("[%1/%2]").arg(idx).arg(total));
        panel_lbl_current_idx_->setStyleSheet(
            "background-color: #ff8800; color: #fff; padding: 2px 8px; "
            "border-radius: 3px; font-weight: bold;");
    }
    // 顶部 status badge
    if (panel_lbl_status_) {
        panel_lbl_status_->setText(QString("⏳ Running (%1/%2)").arg(idx).arg(total));
        panel_lbl_status_->setStyleSheet(
            "background-color: #4488aa; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888; font-weight: bold;");
    }
    panel_selected_sub_idx_ = idx;
    onPanelTaskListClicked();
}

void AbMainWindow::onSubFinishedPanel(const QString& task_name, int idx, int total,
                                      int rc, double dt_sec) {
    if (task_name != current_task_) return;
    panelUpdateFinishedItem(idx, total, rc, dt_sec);
    if (panel_selected_sub_idx_ == idx) onPanelTaskListClicked();
}

void AbMainWindow::onSubFailedPanel(const QString& task_name, int idx, int total) {
    if (task_name != current_task_) return;
    if (panel_lbl_status_) {
        panel_lbl_status_->setText(QString("✗ Failed at %1/%2").arg(idx).arg(total));
        panel_lbl_status_->setStyleSheet(
            "background-color: #aa4444; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888; font-weight: bold;");
    }
}

// ---- 中央面板 UI 槽 ----
void AbMainWindow::onPanelTaskListClicked() {
    if (!panel_sub_list_) return;
    QListWidgetItem* it = panel_sub_list_->currentItem();
    if (!it) {
        if (panel_lbl_detail_meta_) panel_lbl_detail_meta_->setText("(未选中 sub-task)");
        if (panel_txt_detail_cmd_) panel_txt_detail_cmd_->clear();
        if (panel_btn_open_sub_log_) panel_btn_open_sub_log_->setEnabled(false);
        if (panel_btn_copy_sub_cmd_) panel_btn_copy_sub_cmd_->setEnabled(false);
        panel_selected_sub_idx_ = 0;
        return;
    }
    int idx = it->data(Qt::UserRole).toInt();
    panel_selected_sub_idx_ = idx;
    if (!panel_sub_states_.contains(idx)) return;
    const PanelSubState& st = panel_sub_states_[idx];

    QString meta = QString("[%1/%2]  rc=%3  %4s  status=%5")
        .arg(idx).arg(panel_total_sub_).arg(st.rc).arg(st.dt, 0, 'f', 2).arg(st.status);
    if (panel_lbl_detail_meta_) panel_lbl_detail_meta_->setText(meta);

    QString full = QString("$ %1\n").arg(st.cmd);
    if (!st.desc.isEmpty()) full = QString("📌 %1\n").arg(st.desc) + full;
    if (panel_txt_detail_cmd_) panel_txt_detail_cmd_->setPlainText(full);
    if (panel_btn_open_sub_log_) panel_btn_open_sub_log_->setEnabled(true);
    if (panel_btn_copy_sub_cmd_) panel_btn_copy_sub_cmd_->setEnabled(true);
}

void AbMainWindow::onPanelUpdateElapsed() {
    if (!panel_task_start_ms_ || !panel_lbl_elapsed_) return;
    qint64 ms = QDateTime::currentMSecsSinceEpoch() - panel_task_start_ms_;
    int s_total = int(ms / 1000);
    int m = s_total / 60;
    int s = s_total % 60;
    panel_lbl_elapsed_->setText(QString("⏱ %1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
}

void AbMainWindow::onPanelUpdateCurrentDt() {
    if (!panel_current_sub_start_ms_ || !panel_lbl_current_dt_) return;
    qint64 ms = QDateTime::currentMSecsSinceEpoch() - panel_current_sub_start_ms_;
    double dt = ms / 1000.0;
    if (dt < 60) {
        panel_lbl_current_dt_->setText(QString("⏱ %1s").arg(dt, 0, 'f', 1));
    } else {
        int m = int(dt) / 60;
        int s = int(dt) % 60;
        panel_lbl_current_dt_->setText(QString("⏱ %1m%2s").arg(m).arg(s, 2, 10, QChar('0')));
    }
}

void AbMainWindow::onPanelStopClicked() {
    if (runner_ && runner_->isRunning()) {
        log("info", "⏹ 正在停止 task");
        runner_->stop();
    }
}

void AbMainWindow::onPanelCopyLog() {
    if (!panel_log_view_) return;
    QApplication::clipboard()->setText(panel_log_view_->toPlainText());
    log("ok", "✓ log 已复制到剪贴板");
}

void AbMainWindow::onPanelOpenLogDir() {
    QString dir = panelLogDirForCurrentTask();
    QStringList candidates = { "xdg-open", "nautilus", "dolphin", "thunar", "pcmanfm" };
    for (const QString& c : candidates) {
        QProcess* p = new QProcess(this);
        p->start(c, {dir});
        if (p->waitForStarted(1500)) {
            log("ok", QString("📂 打开 %1").arg(dir));
            return;
        }
        delete p;
    }
    log("warn", QString("⚠ 找不到文件管理器, log 目录: %1").arg(dir));
}

void AbMainWindow::onPanelLaunchCloud(bool use_gl) {
    static const QString kBin  = "/home/bv/code/godot_ui_linux/godot-ui-standalone-skia/bin/Debug/cloud_main";
    static const QString kCwd  = "/home/bv/code/godot_ui_linux/godot-ui-standalone-skia/bin/Debug";
    if (!QFileInfo(kBin).isFile()) {
        QMessageBox::warning(this, "启动失败",
            QString("找不到 binary:\n%1\n\n请先跑 ac task 编译一次").arg(kBin));
        return;
    }
    if (!QFileInfo(kCwd + "/libworkspace_v7.so").isFile()) {
        QMessageBox::warning(this, "启动失败",
            QString("找不到 libworkspace_v7.so, 请先编译 workspace_v7_lib"));
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
    args << "--rendering-driver" << (use_gl ? "opengl3" : "vulkan")
         << "--rendering-method" << (use_gl ? "gl_compatibility" : "forward_plus");

    qint64 pid = 0;
    bool ok = QProcess::startDetached(kBin, args, kCwd, &pid);
    if (ok && pid > 0) {
        log("ok", QString("🚀 cloud_main 已启动 (pid=%1, %2)")
            .arg(pid).arg(use_gl ? "OpenGL3 兼容模式" : "Vulkan+Wayland"));
        if (sb_render_) {
            sb_render_->setText(use_gl ? "渲染模式: OpenGL3" : "渲染模式: Vulkan (Forward+)");
            sb_render_->setStyleSheet(
                QString("color: %1; font-size: 12px; padding: 0 8px; font-weight: bold;")
                    .arg(use_gl ? "#ffaa44" : "#88ccff"));
        }
    } else {
        QMessageBox::warning(this, "启动失败", "QProcess::startDetached 返回失败");
    }
}

}  // namespace ab
