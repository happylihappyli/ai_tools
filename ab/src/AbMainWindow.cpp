// AbMainWindow.cpp
#include "AbMainWindow.h"
#include "AbTaskRunner.h"
#include "AbLogDock.h"
#include "AbTheme.h"
#include "AbLang.h"      // 2026-10-26: 语言管理 + T() 翻译 + log 词汇翻译
#include "AbTaskInspector.h"  // 2026-09-02: 任务/进程检查器 (现在通过 InspectorWindow 间接访问)
#include "InspectorWindow.h"  // 2026-09-16 v4: 独立 InspectorWindow (包 AbTaskInspector)
#include "TaskRunnerWindow.h" // 2026-09-16: 独立任务运行器窗口 (仿 ac_task_runner_gui.py)

#include <cstdio>   // 2026-10-19: fopen / fprintf for auto-exit-marker
#include <map>
#include <QSpinBox>     // 2026-10-19: auto-exit 倒计时秒数配置
#include <QSettings>    // 2026-10-19: 持久化 auto_exit_wait_sec (跨项目)
#include <algorithm>   // 2026-10-19: std::clamp
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
#include <QToolButton>
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

// 2026-10-26: UI 字符串统一走 AbLang::T (双向翻译表, zh 默认 / en 可切, 视图→语言)
static QString T(const char* s) { return AbLang::T(s); }

AbMainWindow::AbMainWindow(const AbConfig& cfg, const QString& configPath, QWidget* parent)
    : QMainWindow(parent),
      cfg_(cfg),
      // 2026-09-19: ai_build.json 路径 — 用于 reload + QFileSystemWatcher
      //   默认: cfg_.cwd + "/ai_build.json" (跟 main.cpp:1426 找 configPath 路径一致)
      config_path_(configPath.isEmpty() ? (cfg_.cwd + "/ai_build.json") : configPath) {
    // 2026-10-19 user-configurable-auto-exit-wait: QSettings 持久化的 user-pref 覆盖 cfg.
    //   用户原话 "用户可以设置, 保存". 顺序: ai_build.json → QSettings user-pref (最后覆盖).
    {
        QSettings s("ai_tools", "ab");
        int sec = s.value("auto_exit_wait_sec", -1).toInt();
        if (sec >= 0) cfg_.auto_exit_wait_sec = std::clamp(sec, 0, 60);
    }
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

    log("info", T("ab started v1.0.0"));
    log("info", T("Project: %1").arg(cfg_.cwd));
    log("info", T("Tasks: %1").arg(static_cast<int>(cfg_.tasks.size())));
    log("info", T("Theme: %1").arg(cfg_.theme));
    // 2026-10-26 i18n: 显示当前语言 (视图 → 语言 可切, 存 ~/.config/ai_tools/lang.json)
    log("info", T("Language: %1").arg(AbLang::displayName(static_cast<int>(AbLang::current()))));
    // 2026-09-08 v4: auto 链也 log 一份, 这样 log dock 完整, 不必去状态栏看
    log("info", T("Auto chain: %1")
        .arg(cfg_.auto_chain.isEmpty() ? T("(none)") : cfg_.auto_chain.join(" → ")));

    // 检查 cloud_main (如果配置了 run_after_build)
    if (!cfg_.run_after_build.binary_path.isEmpty()) {
        QString bin = findRunBinary();
        if (!bin.isEmpty()) {
            cloud_binary_ = bin;
            enableRunCloudButton(true);
            log("ok", T("✓ %1 ready").arg(QFileInfo(bin).fileName()));
        } else {
            log("warn", T("%1 not found, enabled after build").arg(cfg_.run_after_build.binary_path));
        }
    }

    // 探测外部工具绝对路径 (避免桌面 GUI PATH 不带 ~/.local/bin)
    ac_binary_     = findTool("ac");
    ar_binary_     = findTool("ar");  // 2026-09-09: AI Run
    spd_say_binary_ = findTool("spd-say");
    const char* ac_no_tts = std::getenv("AC_NO_TTS");
    tts_enabled_   = !(ac_no_tts && std::string(ac_no_tts) == "1");
    if (ac_binary_.isEmpty()) {
        log("warn", T("✗ ac not found, cannot open Token Manager (install ai_tools and check ~/.local/bin in PATH)"));
    } else {
        log("info", QString("ac: %1").arg(ac_binary_));
    }
    if (ar_binary_.isEmpty()) {
        log("warn", QString::fromUtf8("未找到 ar 命令, [AR 运行工具] 按钮会失败 (装 ar_launcher 到 ~/.local/bin?)"));
    } else {
        log("info", QString("ar: %1").arg(ar_binary_));
    }
    if (spd_say_binary_.isEmpty()) {
        log("warn", T("TTS unavailable: spd-say not found"));
    } else if (!tts_enabled_) {
        log("info", QString("TTS 已关闭 (AC_NO_TTS=1, spd-say=%1)").arg(spd_say_binary_));
    } else {
        log("info", T("TTS enabled: %1").arg(spd_say_binary_));
    }

    // 2026-09-08 v3: 把"任务列表在哪"这种提示信息放到操作日志里, 主窗口不显示
    log("info", "💡 任务列表: 顶部 [任务检查器] → [📋 任务] tab, 双击行直接跑");
    log("info", "💡 操作日志: 底部面板, 编译/任务输出实时滚出 (Ctrl+Shift+L 切换)");

    // 2026-10-19 startup-prompt: 默认不弹 — 把 prompt 挂到顶部 header 🔔 按钮 (用户点开看).
    //   ai_build.json 设 ui.prompt_modal=true 才弹模态 (强制用户看完).
    showStartupPrompts();

    // auto_start
    // 2026-09-16 v7: 强制 cerr log, 确认 QTimer 真触发 + cfg_.auto_start 值
    std::fprintf(stderr, "[ab] auto_start=%s, auto_chain=[%s], schedule QTimer 300ms → onRunAuto\n",
                 cfg_.auto_start ? "true" : "false",
                 cfg_.auto_chain.join(", ").toUtf8().constData());
    std::fflush(stderr);
    if (cfg_.auto_start && !cfg_.auto_chain.isEmpty()) {
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
    // 2026-09-16 v9: bug 修复 - onSubStartedPanel / onSubFinishedPanel / onSubFailedPanel
    //   之前是孤儿函数, 从没 connect 到 runner_->sub_* signal, 导致中央面板 sub-task 列表
    //   不刷新状态 (跑完了还显示静态 4 行), 用户反馈"为什么半自动运行" 实际是 UI 没反馈
    //   现在 connect 后, 中央面板会实时高亮当前 sub-task + 进度条 + ✓/✗ 状态
    connect(runner_, &AbTaskRunner::sub_started,
            this, &AbMainWindow::onSubStartedPanel);
    connect(runner_, &AbTaskRunner::sub_finished,
            this, &AbMainWindow::onSubFinishedPanel);
    connect(runner_, &AbTaskRunner::sub_failed,
            this, &AbMainWindow::onSubFailedPanel);
    // 2026-10-19 ab-error-log: 失败时 ab-error-*.log 写到 /tmp, 给 agent 看. 显示路径
    //   在 log panel + stderr (sandbox 里 agent tail /tmp/cloud_editor.log 也能看到).
    connect(runner_, &AbTaskRunner::error_log_written,
            this, [this](const QString& task_name, const QString& log_path, int exit_code) {
                log("error", T("[%1] rc=%2 error log: %3").arg(task_name).arg(exit_code).arg(log_path));
                fprintf(stderr, "[ab-error-log] task=%s rc=%d file=%s\n",
                    task_name.toUtf8().constData(), exit_code, log_path.toUtf8().constData());
                fflush(stderr);
            });
    // 2026-10-15: 单 sub-task 内编译文件数实时进度 (scons / gcc / ninja)
    connect(runner_, &AbTaskRunner::sub_progress,
            this, &AbMainWindow::onSubProgress);
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

    // 2026-09-19: ai_build.json 热重载 — 监听文件 mtime 变化自动 reload
    //   不需要重启 ab 就能反映 ai_build.json 修改 (省去 pkill+ab 流程)
    if (!config_path_.isEmpty() && QFileInfo::exists(config_path_)) {
        config_watcher_ = new QFileSystemWatcher(this);
        config_watcher_->addPath(config_path_);
        connect(config_watcher_, &QFileSystemWatcher::fileChanged,
                this, [this](const QString &path) {
            // fileChanged 触发时旧 inode 已释放, watcher 自动失去监听.
            // 100ms 后重新 addPath 让下次写再次触发 (覆盖 vi/sed/Edit 多次写的场景)
            log("ok", T("[auto-reload] ai_build.json changed, re-reading..."));
            QTimer::singleShot(100, this, [this, path]() {
                reloadConfig();
                if (config_watcher_ && !config_watcher_->files().contains(path)) {
                    config_watcher_->addPath(path);
                }
            });
        });
    }
    // F5 快捷键手动 reload (auto-reload 出问题时强制刷新)
    QAction* reload_act = new QAction(T("Reload ai_build.json"), this);
    reload_act->setShortcut(QKeySequence("F5"));
    reload_act->setToolTip(T("Reload ai_build.json + rebuild toolbar/menus/buttons (no ab restart needed)"));
    addAction(reload_act);  // 加到主窗口, 全局快捷键生效
    connect(reload_act, &QAction::triggered, this, [this]() {
        log("ok", T("[F5] re-reading ai_build.json..."));
        reloadConfig();
    });
}

// 2026-10-19 startup-prompt (用户反馈):
//   默认不弹窗 — 把 prompt 挂到顶部 header 🔔 按钮 (用户点开看).
//   ai_build.json 设 ui.prompt_modal=true 才弹模态 (强制用户看完).
//   "如果是自动的, 一般也不用提示, 每个子任务都有说明" — auto 链跑起来后
//   🔧 sub-task tab 实时显示, 不需要默认 prompt 弹窗.
void AbMainWindow::showStartupPrompts(bool allow_modal) {
    if (cfg_.startup_prompts.isEmpty()) {
        if (panel_btn_prompts_) panel_btn_prompts_->setVisible(false);
        return;
    }
    int font_pt = cfg_.startup_prompt_font_pt > 0 ? cfg_.startup_prompt_font_pt : 28;
    // 2026-10-19 startup-prompt (用户原话: '默认不要显示大字体窗口, 除非提醒用户要操作'):
    //   默认 modal=false — 不弹模态, 把 prompt 加到顶部 header 🔔 按钮的 menu (用户点开才看).
    //   modal=true 才弹模态 QMessageBox (大字体).
    //   用户提示: '如果是自动的, 一般也不用提示, 每个子任务都有说明' — 用户的 auto 链跑
    //   起来后, 🔧 sub-task tab 已经实时显示, 不需要默认 prompt 弹窗.
    if (cfg_.startup_prompts.isEmpty()) {
        if (panel_btn_prompts_) panel_btn_prompts_->setVisible(false);
        return;
    }
    if (cfg_.startup_prompt_modal && allow_modal) {
        // 强制模态: 弹大字体 QMessageBox 串行, 用户必须 OK 一路点完
        for (int i = 0; i < cfg_.startup_prompts.size(); ++i) {
            const QString& text = cfg_.startup_prompts[i];
            QString title = T("⚡ Startup Tip (%1/%2)")
                .arg(i + 1).arg(cfg_.startup_prompts.size());
            QMessageBox box(this);
            box.setWindowTitle(title);
            box.setText(text);
            box.setIcon(QMessageBox::Information);
            QFont f = box.font();
            f.setPointSize(font_pt);
            f.setBold(true);
            box.setFont(f);
            box.setMinimumSize(600, 200);
            box.adjustSize();
            box.exec();
        }
        log("ok", QString("[startup-prompt modal] 显示 %1 段大字体提示 (用户已点 OK)")
            .arg(cfg_.startup_prompts.size()));
        // modal 完后按钮隐藏 (用户已读完)
        if (panel_btn_prompts_) panel_btn_prompts_->setVisible(false);
    } else {
        // 默认非模态: 把每段 prompt 加到顶部 🔔 按钮的 menu (用户点开才看)
        //   按钮显示 🔔 + (N) 提示有几段 prompt
        if (!panel_btn_prompts_) return;
        QMenu* m = panel_btn_prompts_->menu();
        m->clear();
        for (int i = 0; i < cfg_.startup_prompts.size(); ++i) {
            const QString& text = cfg_.startup_prompts[i];
            // 用第一行作为菜单项标题 + 后面行作为 tooltip (完整 prompt)
            QStringList lines = text.split('\n');
            QString title = lines.isEmpty() ? text : lines.first().left(60);
            QString rest  = lines.size() > 1 ? lines.mid(1).join('\n') : QString();
            QAction* a = m->addAction(QString("%1. %2").arg(i + 1).arg(title));
            a->setToolTip(rest.isEmpty() ? text : rest);
            // 不弹窗 — 鼠标 hover 上看 tooltip; 想看完整用菜单项或 QMessageBox
            connect(a, &QAction::triggered, this, [this, text, title]() {
                QMessageBox::information(this, title, text);
            });
        }
        panel_btn_prompts_->setText(QString("🔔 (%1)").arg(cfg_.startup_prompts.size()));
        panel_btn_prompts_->setToolTip(T("Startup tips: %1 sections (click 🔔 to view)").arg(cfg_.startup_prompts.size()));
        panel_btn_prompts_->setVisible(true);
        log("info", QString("[startup-prompt] %1 段提示已挂到 🔔 按钮 (默认非模态, 点开看)")
            .arg(cfg_.startup_prompts.size()));
    }
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
    QMenu* m_file = mb->addMenu(T("File (&F)"));
    {
        QAction* a = m_file->addAction(T("GitHub Token Manager..."));
        a->setShortcut(QKeySequence("Ctrl+G"));
        a->setData("open_ght");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["open_ght"] = a;
    }
    {
        QAction* a = m_file->addAction(T("Backup Project..."));
        a->setShortcut(QKeySequence("Ctrl+B"));
        a->setData("bak");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["bak"] = a;
    }
    m_file->addSeparator();
    {
        QAction* a = m_file->addAction(T("Quit"));
        a->setShortcut(QKeySequence("Ctrl+Q"));
        a->setData("quit");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["quit"] = a;
    }

    // --- 工具 (&T) ---
    QMenu* m_tools = mb->addMenu(T("Tools (&T)"));
    {
        QAction* a = m_tools->addAction(T("GitHub Token Diagnostics"));
        a->setShortcut(QKeySequence("Ctrl+D"));
        a->setData("diag");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["diag"] = a;
    }
    {
        QAction* a = m_tools->addAction(T("Proxy Test..."));
        a->setShortcut(QKeySequence("Ctrl+P"));
        a->setData("proxy_test");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["proxy_test"] = a;
    }
    m_tools->addSeparator();
    {
        QAction* a = m_tools->addAction(T("TTS Speak..."));
        a->setShortcut(QKeySequence("Ctrl+T"));
        a->setData("tts");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["tts"] = a;
    }

    // --- 日志 (&L) ---
    // 2026-09-20: 把 "复制 log" / "打开 log 目录" 从工具栏移到菜单 —
    //   这两个是次要操作, 不需要默认占用工具栏.
    //   Ctrl+L = 复制 log (一键复制当前 panel log 到剪贴板)
    //   Ctrl+Shift+O = 打开 log 目录 (用系统文件管理器)
    QMenu* m_log = mb->addMenu(T("Log (&L)"));
    {
        QAction* a = m_log->addAction(T("📋 Copy Log"));
        a->setShortcut(QKeySequence("Ctrl+L"));
        a->setData("copy_log");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["copy_log"] = a;
    }
    {
        QAction* a = m_log->addAction(T("📂 Open Log Directory"));
        a->setShortcut(QKeySequence("Ctrl+Shift+O"));
        a->setData("open_log_dir");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["open_log_dir"] = a;
    }

    // --- 视图 (&V) ---
    QMenu* m_view = mb->addMenu(T("View (&V)"));
    {
        QAction* a = m_view->addAction(T("Show Log Panel"));
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
        QAction* a = m_view->addAction(T("📋 Open Task Inspector"));
        a->setShortcut(QKeySequence("Ctrl+I"));
        a->setData("open_inspector");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["open_inspector"] = a;
    }
    // 2026-09-02: 主题改成 submenu 4 选 1 (单选 QActionGroup)
    {
        QMenu* m_theme = m_view->addMenu(T("Theme (&T)"));
        QActionGroup* group = new QActionGroup(this);
        group->setExclusive(true);
        AbTheme::Kind cur = AbTheme::current();
        for (int k = 0; k < AbTheme::NumThemes; ++k) {
            QAction* a = m_theme->addAction(T(AbTheme::displayName(k).toUtf8().constData()));
            a->setCheckable(true);
            a->setChecked(k == cur);
            a->setData(QString("theme:%1").arg(AbTheme::shortName(k)));
            connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
            group->addAction(a);
            actions_[QString("theme:%1").arg(AbTheme::shortName(k))] = a;
        }
    }
    // 2026-10-26 i18n: 语言子菜单 (中文/English 单选, 自动保存 ~/.config/ai_tools/lang.json)
    //   切换后 rebuildUi() 整体重建菜单/工具栏/面板, 立即生效
    {
        QMenu* m_lang = m_view->addMenu(T("Language (&L)"));
        QActionGroup* lang_group = new QActionGroup(this);
        lang_group->setExclusive(true);
        AbLang::Kind cur_lang = AbLang::current();
        for (int k = 0; k < AbLang::NumLangs; ++k) {
            QAction* a = m_lang->addAction(AbLang::displayName(k));
            a->setCheckable(true);
            a->setChecked(k == cur_lang);
            a->setData(QString("lang:%1").arg(AbLang::shortName(k)));
            connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
            lang_group->addAction(a);
            actions_[QString("lang:%1").arg(AbLang::shortName(k))] = a;
        }
    }

    // --- 帮助 (&H) ---
    QMenu* m_help = mb->addMenu(T("Help (&H)"));
    {
        QAction* a = m_help->addAction(T("About ab..."));
        a->setData("about");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["about"] = a;
    }
}

// 2026-09-02: 框架内置通用工具栏 (跑选中/跑 Auto/停止 - 所有 task runner 通用)
void AbMainWindow::buildBuiltInToolbar() {
    QToolBar* tb = addToolBar(T("Main Toolbar"));
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

    // 2026-09-18: disable_built_in_toolbar=true → 完全跳过默认按钮, 只显 ui.toolbar 自定义按钮
    if (!cfg_.disable_built_in_toolbar) {
        add("▶", "run_selected", T("Run selected task (F5)"), "F5");
        add("⚡", "run_auto",     T("Run Auto chain (F6)"), "F6");
        tb->addSeparator();
    }

    // 2026-09-09: 项目特定动作 (从 buildBuiltInButtons 合并到工具栏)
    // 2026-09-18: disable_built_in_toolbar=true → 也跳过 run_after_build 5 个按钮
    if (!cfg_.disable_built_in_toolbar && !cfg_.run_after_build.binary_path.isEmpty()) {
        QString binary_name = QFileInfo(cfg_.run_after_build.binary_path).fileName();
        // 默认按钮文案 "🚀 启动" 走翻译表; ai_build.json 自定义的 label 原样使用
        QString btn_label = cfg_.run_after_build.button_label;
        if (btn_label.isEmpty() || btn_label == QString::fromUtf8("🚀 启动")) {
            btn_label = T("🚀 Run");
        }
        add(T("⚡ Build && Run (Vulkan)"), "build_and_run",
            T("Build project (build+deploy task), then start start_cloud_main.sh (Vulkan)"), "Ctrl+Shift+R");
        add(T("⚡ Build && Run (OpenGL)"), "build_and_run_gl",
            T("Build project, then start start_cloud_main.sh --gl (OpenGL compat)"), "Ctrl+Shift+E");
        add(btn_label, "run_cloud",
            T("Run compiled %1").arg(binary_name));
        add(T("🟢 Run Compat Mode"), "run_cloud_gl",
            T("Run compiled %1 (OpenGL compat mode)").arg(binary_name));
        add(T("🎯 AR Runner"), "open_ar",
            T("Open AI Run tool via ac ar (reads ai_build.json run_panel)"));
        tb->addSeparator();
        add(T("■ Stop"), "stop",
            T("Stop current task"), "F7");
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

    // 2026-09-16 v7: 把 Stop 按钮放到顶部 QToolBar, 节省底部高度, 让 sub-task 列表可以显示更多行
    // 2026-09-20: 进一步精简工具栏 — 只保留 Stop (核心控制), 复制 log / 打开 log 目录
    //   移到菜单的 "日志(&L)" 项, 默认工具栏不再提供这些次要操作.
    panel_toolbar_ = new QToolBar(T("Actions"), this);
    panel_toolbar_->setMovable(false);
    panel_toolbar_->setIconSize(QSize(16, 16));
    // 2026-10-19 fix-theme: 让主题 QSS 接管 toolbar 背景 (不硬编码)

    panel_btn_stop_ = new QPushButton(T("⏹ Stop"));
    // Stop 按钮用 role=danger (QSS 已配 QPushButton[role="danger"])
    panel_btn_stop_->setProperty("role", "danger");
    connect(panel_btn_stop_, &QPushButton::clicked, this, &AbMainWindow::onPanelStopClicked);
    panel_toolbar_->addWidget(panel_btn_stop_);

    // 2026-09-16 v7: QToolBar 加到主窗口顶部 (header 上方)
    addToolBar(Qt::TopToolBarArea, panel_toolbar_);

    // ====== 顶部 header: 任务名 + status badge + elapsed ======
    // 2026-10-19 fix-theme: 不要硬编码 background-color, 让主题 QSS 接管
    //   之前硬编码 #1a1a1a 等, 导致 light/solarized/nord 主题切过来时这块还是黑底
    QFrame* header_ = new QFrame();
    header_->setFrameShape(QFrame::Shape::StyledPanel);
    // 仅保留边框/圆角/padding, 背景由主题决定
    header_->setStyleSheet(
        "QFrame { border: 1px solid palette(midlight); "
        "border-radius: 4px; padding: 6px; }");
    QHBoxLayout* hl = new QHBoxLayout(header_);
    hl->setContentsMargins(8, 4, 8, 4);
    hl->setSpacing(12);

    panel_lbl_title_ = new QLabel(T("⏸ No Task"));
    panel_lbl_title_->setFont(QFont("sans-serif", 14, QFont::Weight::Bold));
    // 标题色跟随主题 (蓝色强调), 但 QSS 全局有 QLabel, 这里只覆盖需要强调的
    hl->addWidget(panel_lbl_title_);

    hl->addStretch();

    panel_lbl_elapsed_ = new QLabel("⏱ 00:00");
    panel_lbl_elapsed_->setFont(QFont("monospace", 11));
    hl->addWidget(panel_lbl_elapsed_);

    panel_lbl_status_ = new QLabel(T("⏳ Waiting"));
    panel_lbl_status_->setFont(QFont("sans-serif", 11, QFont::Weight::Bold));
    // status badge 用 dynamicProperty [level] 让主题 QSS 接管 (ok/err/warn/info 已在 QSS)
    panel_lbl_status_->setProperty("level", "info");
    hl->addWidget(panel_lbl_status_);

    // 2026-10-19 startup-prompt-button: 默认不弹模态窗口, 而是在主窗口顶部 header
    //   加一个 🔔 提示按钮 (用户原话: '默认不要显示, 顶部工具栏附近, 哪里空放哪里').
    //   用户点按钮展开 prompt list (非模态). 要强制模态, ai_build.json 设 ui.prompt_modal=true.
    panel_btn_prompts_ = new QToolButton();
    panel_btn_prompts_->setText("🔔");
    panel_btn_prompts_->setToolTip(T("Startup tips (click to view)"));
    panel_btn_prompts_->setVisible(false);  // 默认隐藏, 有 prompt 才显示
    panel_btn_prompts_->setPopupMode(QToolButton::ToolButtonPopupMode::InstantPopup);
    panel_btn_prompts_->setMenu(new QMenu(panel_btn_prompts_));  // 占位, 后填
    hl->addWidget(panel_btn_prompts_);

    // 2026-10-19 auto-exit-countdown-gui: 倒计时 cancel 按钮 + wait sec spinbox.
    //   用户原话 "用户可以设置, 保存, 默认都是3秒, 而且有倒计时". 这里给 GUI 配置:
    //     [auto-exit 倒计时] [___3___秒] [✕ 取消退出 (3s)]
    //   spinbox 改 cfg_.auto_exit_wait_sec (QSettings 持久化); cancel 按钮设 cancel_auto_exit_=true
    //   timer 检查到就停 quit, 不退.
    QLabel* lbl_wait = new QLabel(T("auto-exit countdown:"));
    lbl_wait->setFont(QFont("sans", 9));
    hl->addWidget(lbl_wait);
    auto_exit_wait_spin_ = new QSpinBox();
    auto_exit_wait_spin_->setRange(0, 60);
    auto_exit_wait_spin_->setSuffix(T(" sec"));
    auto_exit_wait_spin_->setValue(cfg_.auto_exit_wait_sec);
    auto_exit_wait_spin_->setToolTip(T("Wait N seconds before auto-exit after auto chain finishes. 0 = exit immediately. Range [0, 60]"));
    hl->addWidget(auto_exit_wait_spin_);
    auto_exit_cancel_btn_ = new QPushButton(T("✕ Cancel Exit"));
    auto_exit_cancel_btn_->setToolTip(T("Click to cancel the auto-exit countdown (keep GUI open)"));
    auto_exit_cancel_btn_->setVisible(false);
    hl->addWidget(auto_exit_cancel_btn_);
    connect(auto_exit_wait_spin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
                cfg_.auto_exit_wait_sec = std::clamp(v, 0, 60);
                // 写回 ai_build.json (顶层 ui.auto_exit_wait_sec), 这样下次 reload 还在.
                if (!config_path_.isEmpty()) {
                    AbJson::patchUiAutoExitWait(config_path_.toStdString(), cfg_.auto_exit_wait_sec);
                }
                // QSettings 也存一份 (用户跨项目用)
                QSettings s("ai_tools", "ab");
                s.setValue("auto_exit_wait_sec", cfg_.auto_exit_wait_sec);
                log("info", T("auto-exit wait set to %1 sec (saved to ai_build.json)").arg(cfg_.auto_exit_wait_sec));
            });
    connect(auto_exit_cancel_btn_, &QPushButton::clicked,
            this, [this]() {
                cancel_auto_exit_ = true;
                log("info", T("✕ user clicked cancel-exit (next timer tick won't quit)"));
            });

    // 2026-10-15: 编译进度 label (scons 编译 N 个文件)
    //   显示 "🔨 编译 12 个 (foo.cpp)" — 200ms 节流, 不抢 status badge
    panel_lbl_compile_progress_ = new QLabel("");
    panel_lbl_compile_progress_->setFont(QFont("monospace", 10));
    panel_lbl_compile_progress_->setProperty("level", "warn");
    panel_lbl_compile_progress_->setVisible(false);
    hl->addWidget(panel_lbl_compile_progress_);

    root->addWidget(header_);

    // ====== 进度条 ======
    panel_progress_ = new QProgressBar();
    panel_progress_->setRange(0, 1);
    panel_progress_->setValue(0);
    panel_progress_->setFormat(T("0 / %1  (sub-task)").arg(0));
    panel_progress_->setFixedHeight(20);
    // 让主题 QSS 接管 (QProgressBar 已在 QSS)
    root->addWidget(panel_progress_);

    // ====== ⚡ 当前执行 区 ======
    panel_current_box_ = new QFrame();
    panel_current_box_->setFrameShape(QFrame::Shape::StyledPanel);
    // 仅保留边框, 不硬编码背景 (让主题决定)
    panel_current_box_->setStyleSheet(
        "QFrame { border: 2px solid palette(highlight); "
        "border-radius: 4px; padding: 4px; }");
    QVBoxLayout* cb_layout = new QVBoxLayout(panel_current_box_);
    cb_layout->setContentsMargins(8, 4, 8, 4);
    cb_layout->setSpacing(2);
    QHBoxLayout* cur_top = new QHBoxLayout();
    QLabel* cur_title = new QLabel(T("⚡ Current"));
    cur_title->setFont(QFont("sans-serif", 11, QFont::Weight::Bold));
    cur_top->addWidget(cur_title);
    panel_lbl_current_idx_ = new QLabel("—");
    panel_lbl_current_idx_->setFont(QFont("monospace", 12, QFont::Weight::Bold));
    panel_lbl_current_idx_->setProperty("level", "info");
    cur_top->addWidget(panel_lbl_current_idx_);
    panel_lbl_current_dt_ = new QLabel("⏱ 0s");
    panel_lbl_current_dt_->setFont(QFont("monospace", 10));
    cur_top->addWidget(panel_lbl_current_dt_);
    cur_top->addStretch();
    cb_layout->addLayout(cur_top);
    panel_txt_current_cmd_ = new QPlainTextEdit();
    panel_txt_current_cmd_->setReadOnly(true);
    panel_txt_current_cmd_->setFont(QFont("monospace", 10));
    panel_txt_current_cmd_->setMaximumHeight(70);
    panel_txt_current_cmd_->setLineWrapMode(QPlainTextEdit::LineWrapMode::WidgetWidth);
    // 跟随主题 (QSS 已配 QPlainTextEdit)
    panel_txt_current_cmd_->setPlaceholderText(T("(waiting for sub-task to start)"));
    cb_layout->addWidget(panel_txt_current_cmd_);
    // 注: panel_current_box_ 加入 QTabWidget (panel_task_tab_) 在下方, 不直接 addWidget 到 root

    // ====== 中间 split: 左 sub-task 列表 + 右 实时 log ======
    // 2026-10-19 fix-splitter-ratio: 用户要求"左边占1/4, 右边实时日志3/4" → 左:右 = 1:3
    // 问题: setSizes({1, 3}) 用相对权重是官方推荐做法, 但实际效果:
    //   - 构造时 splitter handle 还没拿到父 layout 给的尺寸, setSizes 第一次会被忽略
    //   - setStretchFactor 是 "权重" 而不是 "比例", 受 sizeHint 影响大
    //   - 子 widget 有自己的 sizeHint, QListWidget 偏好较宽, QPlainTextEdit 偏好较宽, 平衡时两边差不多
    // 修复: 在 resizeEvent 里**强制** setSizes({W/4, 3W/4}) 重新分配, 加 QTimer::singleShot(0) 在事件循环首轮补一次
    panel_splitter_ = new QSplitter(Qt::Orientation::Horizontal);
    panel_splitter_->setChildrenCollapsible(false);
    // 给左/右子 widget 设置不同的 sizeHint, 让 splitter 倾向右更大
    panel_splitter_->setHandleWidth(4);
    panel_splitter_->setStretchFactor(0, 1);   // 左 (1/4)
    panel_splitter_->setStretchFactor(1, 3);   // 右 (3/4)

    QWidget* left = new QWidget();
    left->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    // 关键: 给左 widget 偏小的最小宽度, 防止 QListWidget 默认 sizeHint 偏大抢空间
    left->setMinimumWidth(120);
    QVBoxLayout* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    QLabel* lbl_left_title = new QLabel(T("📋 Sub-task Progress"));
    ll->addWidget(lbl_left_title);
    panel_sub_list_ = new QListWidget();
    panel_sub_list_->setFont(QFont("monospace", 10));
    // 2026-09-16 v7: 用户反馈"中间任务列表至少要显示 5 个任务"
    //   minHeight = 5 行 * 28px + padding ≈ 160px, 让 sub-task 列表默认就够 5 行可见
    //   (窗口可拉高显示更多行)
    panel_sub_list_->setMinimumHeight(160);
    // 跟随主题 QSS (QListWidget 已在 QSS)
    connect(panel_sub_list_, &QListWidget::itemClicked,
            this, [this](QListWidgetItem*) { onPanelTaskListClicked(); });
    connect(panel_sub_list_, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem*, QListWidgetItem*) { onPanelTaskListClicked(); });
    ll->addWidget(panel_sub_list_);
    panel_splitter_->addWidget(left);

    QWidget* right = new QWidget();
    right->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 关键: 给右 widget 设置较强 Expanding + minimum, 让 splitter 倾向让它占大头
    right->setMinimumWidth(300);
    QVBoxLayout* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* log_header = new QHBoxLayout();
    panel_lbl_log_summary_ = new QLabel(T("📄 Live Log (%1 lines)").arg(0));
    log_header->addWidget(panel_lbl_log_summary_);
    log_header->addStretch();
    rl->addLayout(log_header);
    panel_log_view_ = new QPlainTextEdit();
    panel_log_view_->setReadOnly(true);
    panel_log_view_->setFont(QFont("monospace", 10));
    // 2026-10-19 fix-theme: 不要硬编码深色背景, 让主题 QSS 接管
    //   之前 #0a0a0a 黑色背景 + #cccccc 浅灰字, light 主题切过来 log 区也是黑底黑字反差乱
    panel_log_view_->setMaximumBlockCount(10000);
    // 让 log 区域可以无限缩放 (splitter 给它更多空间)
    panel_log_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rl->addWidget(panel_log_view_);
    panel_splitter_->addWidget(right);

    // 延迟到事件循环首次布局后再设比例 — setSizes 在 widget 还没确定尺寸时不生效
    QTimer::singleShot(0, this, [this]() {
        if (panel_splitter_) {
            panel_splitter_->setSizes({1, 3});
        }
    });

    root->addWidget(panel_splitter_, /*stretch=*/1);

    // ====== 详情区 ======
    // 2026-10-19: 详情面板默认隐藏 (用户点击 sub-task 才显示), 加关闭按钮
    panel_detail_box_ = new QGroupBox(T("📜 Sub-task Details"));
    QVBoxLayout* dl = new QVBoxLayout(panel_detail_box_);
    dl->setContentsMargins(6, 4, 6, 4);
    dl->setSpacing(2);
    QHBoxLayout* dt = new QHBoxLayout();
    panel_lbl_detail_meta_ = new QLabel(T("(no sub-task selected)"));
    panel_lbl_detail_meta_->setFont(QFont("monospace", 10, QFont::Weight::Bold));
    dt->addWidget(panel_lbl_detail_meta_);
    dt->addStretch();
    panel_btn_open_sub_log_ = new QPushButton(T("📂 Open sub-task log"));
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
            log("warn", T("⚠ No log found for sub-task %1").arg(panel_selected_sub_idx_));
            return;
        }
        QString log_path = dir + "/" + files.first();
        QDesktopServices::openUrl(QUrl::fromLocalFile(log_path));
        log("ok", T("📂 Opened %1").arg(log_path));
    });
    dt->addWidget(panel_btn_open_sub_log_);
    panel_btn_copy_sub_cmd_ = new QPushButton(T("📋 Copy Full Command"));
    panel_btn_copy_sub_cmd_->setEnabled(false);
    connect(panel_btn_copy_sub_cmd_, &QPushButton::clicked, this, [this]() {
        if (panel_selected_sub_idx_ <= 0 || !panel_txt_detail_cmd_) return;
        QApplication::clipboard()->setText(panel_txt_detail_cmd_->toPlainText());
        log("ok", T("✓ full command copied to clipboard"));
    });
    dt->addWidget(panel_btn_copy_sub_cmd_);
    // 2026-10-19: 关闭按钮 — 隐藏整个详情面板
    panel_btn_close_detail_ = new QPushButton("✕");
    panel_btn_close_detail_->setFixedSize(24, 24);
    panel_btn_close_detail_->setToolTip(T("Close details panel (click a sub-task row to reopen)"));
    connect(panel_btn_close_detail_, &QPushButton::clicked, this, [this]() {
        if (panel_detail_box_) panel_detail_box_->setVisible(false);
    });
    dt->addWidget(panel_btn_close_detail_);
    dl->addLayout(dt);
    panel_txt_detail_cmd_ = new QPlainTextEdit();
    panel_txt_detail_cmd_->setReadOnly(true);
    panel_txt_detail_cmd_->setFont(QFont("monospace", 9));
    panel_txt_detail_cmd_->setMinimumHeight(80);
    panel_txt_detail_cmd_->setMaximumHeight(280);
    panel_txt_detail_cmd_->setLineWrapMode(QPlainTextEdit::LineWrapMode::WidgetWidth);
    panel_txt_detail_cmd_->setPlaceholderText(T("(click a sub-task row on the left to see the full command)"));
    // 跟随主题 QSS
    panel_txt_detail_cmd_->setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOn);
    dl->addWidget(panel_txt_detail_cmd_);
    // 2026-10-19: 默认隐藏 — 用户点击 sub-task 才显示
    panel_detail_box_->setVisible(false);
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

    // 2026-10-19 fix-theme: 移除硬编码的暗主题 QSS, 让 AbTheme 全局 QSS 接管
    //   之前这行强制覆盖 mainwindow 的全局样式, 主题切换不生效
    // setStyleSheet(...暗主题...) 已删除

    // 2026-09-16 v3: log dock 已移除, no-op (log 在中央面板)
}

// 2026-10-19: 窗口缩放后强制保持 1:3 splitter 比例 (左 1/4, 右 3/4)
//  Qt QSplitter 的 setSizes 在 splitter 构造后第一次布局前不生效 (widget 还没拿到父 layout 分配的尺寸)
//  resizeEvent 在窗口每次变化时被触发, 我们重新按比例分配像素值
//  这保证: 1) 启动后第一次显示就是 1:3  2) 拉大/缩小窗口保持 1:3
void AbMainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (!panel_splitter_) return;
    int total_w = panel_splitter_->width();
    if (total_w <= 0) return;
    int left_w  = total_w / 4;       // 1/4
    int right_w = total_w - left_w;  // 3/4
    panel_splitter_->setSizes({left_w, right_w});
}

void AbMainWindow::buildStatusBar() {
    // 2026-09-08 v4: 状态栏承载所有持久信息 (项目路径 + auto 链 + 进度条 + 动态 log 状态 + Qt 主题)
    //   主窗口中间区域完全清空, 只剩 stretch
    statusbar_ = new QStatusBar(this);
    setStatusBar(statusbar_);

    // 1) 最左: 项目路径 + auto 链 (持久显示, 不被 log 覆盖)
    QString auto_text = cfg_.auto_chain.isEmpty() ? T("(none)") : cfg_.auto_chain.join(" → ");
    sb_proj_ = new QLabel(QString("📁 %1  |  auto: %2")
                          .arg(cfg_.cwd, auto_text));
    // 2026-10-19 fix-theme: 让主题 QSS 接管状态栏颜色
    statusbar_->addWidget(sb_proj_, 1);  // 拉伸占 1 份

    // 2) 中: 动态 log 状态 (✗/✓ msg, 被 log() 覆盖)
    sb_left_ = new QLabel(T("Ready"));
    statusbar_->addWidget(sb_left_, 1);  // 拉伸占 1 份

    // 2.5) 2026-09-16 fix71: 渲染模式 label (展示默认 Vulkan / OpenGL 兼容模式差异)
    //   默认 "渲染模式: —" 灰, 启动 cloud_main (Vulkan) → "渲染模式: Vulkan (Forward+)" 蓝,
    //   启动兼容模式 → "渲染模式: OpenGL3 (Compatibility)" 橙. 让用户一眼看到当前是哪种渲染.
    sb_render_ = new QLabel(T("Renderer: —"));
    sb_render_->setProperty("level", "info");  // 走 QSS 默认蓝色 (跟之前 #2980b9 类似)
    statusbar_->addPermanentWidget(sb_render_);

    // 3) 右: Qt/主题 (进度条/进度文字 2026-09-16 v3 移到了中央面板顶部 header)
    sb_right_ = new QLabel(T("Qt=%1 | Theme=%2")
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
    // 2026-09-20: 按 row 字段分组, 每行一个 QToolBar (Qt QToolBar 不自动 wrap)
    //   ai_build.json button 可选 "row": 0/1/2, 同 row 一行, 缺省 0
    std::map<int, std::vector<AbButtonDef>> by_row;
    int max_row = 0;
    for (const auto& b : cfg_.toolbar) {
        by_row[b.row].push_back(b);
        if (b.row > max_row) max_row = b.row;
    }
    QToolBar* last_tb = nullptr;
    for (int r = 0; r <= max_row; ++r) {
        auto it = by_row.find(r);
        if (it == by_row.end()) continue;
        QToolBar* tb = addToolBar(QString("主工具栏 row=%1").arg(r));
        tb->setObjectName(QString("AbMainToolBar_row%1").arg(r));
        tb->setMovable(true);
        tb->setFloatable(true);
        for (const auto& b : it->second) {
            QAction* a = createActionForButton(b, this);
            tb->addAction(a);
            actions_[b.id] = a;
        }
        // 第一行末尾加"📊 任务运行器"内置按钮
        if (r == 0) {
            tb->addSeparator();
            QAction* a_runner = new QAction(T("📊 Task Runner"), this);
            a_runner->setToolTip(T("Open Task Runner window (sub-task list + live log + current + details)"));
            a_runner->setData("open_task_runner");
            a_runner->setShortcut(QKeySequence("Ctrl+R"));
            tb->addAction(a_runner);
            actions_["open_task_runner"] = a_runner;
            connect(a_runner, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        }
        last_tb = tb;
    }
    (void)last_tb;
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
    log("debug", T("Triggered action: %1").arg(id));

    // 2026-10-26 i18n: 语言切换 (视图 → 语言). 保存 lang.json + 整体重建 UI 立即生效.
    if (id.startsWith("lang:")) {
        AbLang::Kind k = AbLang::parse(id.mid(5));
        AbLang::apply(static_cast<int>(k));
        rebuildUi();
        log("ok", T("Language switched: %1 (saved)").arg(AbLang::displayName(static_cast<int>(k))));
        return;
    }

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
        if (sb_right_) sb_right_->setText(T("Qt=%1 | Theme=%2").arg(qApp ? "Qt5/6" : "?", name));
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
            log("err", T("✗ ac not found, cannot open Token Manager (install ai_tools and check ~/.local/bin in PATH)"));
            return;
        }
        log("info", QString("→ %1 ght --no-check").arg(ac_binary_));
        qint64 pid = 0;
        if (QProcess::startDetached(ac_binary_, QStringList() << "ght" << "--no-check", QDir::homePath(), &pid)) {
            log("ok", T("✓ Token manager started, pid=%1").arg(pid));
        } else {
            log("err", T("✗ failed to start ac ght"));
        }
        return;
    }
    if (id == "tts") {
        bool ok = false;
        QString text = QInputDialog::getText(
            this,
            T("TTS Speak"),
            T("Text to speak:"),
            QLineEdit::Normal,
            T("ab toolchain ready"),
            &ok
        );
        if (!ok || text.trimmed().isEmpty()) {
            log("info", T("TTS cancelled"));
            return;
        }
        log("info", QString("→ TTS: %1").arg(text));
        speakTextAsync(text, true);
        return;
    }
    if (id == "bak") {
        if (ac_binary_.isEmpty()) {
            log("err", T("✗ ac not found, backup unavailable"));
            return;
        }
        log("info", QString("→ %1 bak %2").arg(ac_binary_).arg(cfg_.cwd));
        QProcess::startDetached(ac_binary_, QStringList() << "bak" << cfg_.cwd);
        return;
    }
    if (id == "proxy_test") {
        // 调 ac ght-cli --proxy-test, 检测 GitHub API 代理是否通
        if (ac_binary_.isEmpty()) {
            log("err", T("✗ ac not found, cannot run proxy test"));
            return;
        }
        log("info", QString("→ %1 ght-cli --proxy-test").arg(ac_binary_));
        QProcess::startDetached(ac_binary_, QStringList() << "ght-cli" << "--proxy-test", QDir::homePath());
        return;
    }
    if (id == "diag") { runTaskByName("diag"); return; }
    if (id == "view-log") { runTaskByName("view-log"); return; }
    if (id == "build+deploy") { runTaskByName("build+deploy"); return; }
    // 2026-09-20: 复制 log / 打开 log 目录 (从工具栏移到菜单)
    if (id == "copy_log")    { onPanelCopyLog(); return; }
    if (id == "open_log_dir"){ onPanelOpenLogDir(); return; }

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
                else log("warn", T("Menu item %1 has no task/cmd").arg(id));
                return;
            }
        }
    }
    log("warn", T("Undefined action: %1").arg(id));
}

// 2026-10-26 i18n: 语言切换后整体重建 UI
//   思路: 所有界面文案都在 build*() 里经 AbLang::T() 生成, 切语言时把
//   菜单/工具栏/中央面板/状态栏全部拆掉重 build 一遍, 立即全量生效.
//   注意:
//     - config_watcher_ / F5 reload action 是一次性的, 不重建 (不会重复挂)
//     - 副窗口 (任务检查器/任务运行器) 直接关掉, 用户重开时按新语言创建
//     - 正在跑的 task 不受影响: runner_ signals 接的是 this, 重建后新 widget 继续接 log
void AbMainWindow::rebuildUi() {
    // 0) 面板计时器先停再删 (buildCentralPanel 会新建; 防止 tick 到旧 widget)
    if (panel_elapsed_timer_)    { panel_elapsed_timer_->stop();    panel_elapsed_timer_->deleteLater();    panel_elapsed_timer_ = nullptr; }
    if (panel_current_dt_timer_) { panel_current_dt_timer_->stop(); panel_current_dt_timer_->deleteLater(); panel_current_dt_timer_ = nullptr; }
    // auto-exit 倒计时 timer: 语言切换视为用户接管, 停掉 (不再自动退)
    if (auto_exit_countdown_timer_) { auto_exit_countdown_timer_->stop(); auto_exit_countdown_timer_->deleteLater(); auto_exit_countdown_timer_ = nullptr; }

    // 1) 关副窗口 (deleteLater; destroyed signal 会把指针清 nullptr)
    if (task_runner_win_)  { task_runner_win_->deleteLater();  task_runner_win_ = nullptr; }
    if (inspector_window_) { inspector_window_->deleteLater(); inspector_window_ = nullptr; }

    // 2) actions_/buttons_ 清空 (QAction 宿主: menu/toolbar/this 混合, 统一 deleteLater 安全 —
    //    QObject 析构会取消未处理的 DeferredDelete, 不会 double free)
    for (auto it = actions_.begin(); it != actions_.end(); ++it) it.value()->deleteLater();
    actions_.clear();
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) it.value()->deleteLater();
    buttons_.clear();

    // 3) 菜单栏清空 (QMenuBar 上的 QMenu 是其 child, deleteLater 释放)
    menuBar()->clear();
    const auto menus = menuBar()->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
    for (QMenu* m : menus) m->deleteLater();

    // 4) 工具栏全删 (内置主工具栏 + 中央面板操作工具栏 + ui.toolbar 自定义行)
    const auto toolbars = findChildren<QToolBar*>();
    for (QToolBar* tb : toolbars) { removeToolBar(tb); tb->deleteLater(); }

    // 5) 状态栏替换前删旧的
    if (QStatusBar* old_sb = statusBar()) { setStatusBar(nullptr); old_sb->deleteLater(); }

    // 6) 中央 widget 删 (takeCentralWidget 移交所有权)
    if (QWidget* cw = takeCentralWidget()) cw->deleteLater();

    // 7) 重建 (跟构造时的顺序一致)
    buildCentralPanel();
    buildStatusBar();
    buildBuiltInMenus();
    buildBuiltInToolbar();
    buildBuiltInButtons();
    buildMenus();
    buildToolbar();
    buildMainButtons();

    // 8) 按钮可用性跟 runner 状态对齐 (正在跑 task 时禁止再启动)
    const bool running = runner_ && runner_->isRunning();
    if (auto a = actions_.value("run_selected")) a->setEnabled(!running);
    if (auto a = actions_.value("run_auto"))     a->setEnabled(!running);
    if (auto a = actions_.value("stop"))         a->setEnabled(running);
    if (panel_btn_stop_) panel_btn_stop_->setEnabled(running);

    // 9) 标题 + 🔔 启动提示按钮恢复 (allow_modal=false: 只回填按钮, 不重新弹模态框)
    updateWindowTitle();
    showStartupPrompts(/*allow_modal=*/false);
}

void AbMainWindow::onRunSelectedTask() {
    // 2026-09-16 v4: "跑选中" 改去 InspectorWindow 任务 tab 找
    if (!inspector_window_) {
        log("warn", T("Task Inspector window not open, press Ctrl+I first"));
        return;
    }
    auto* ins = inspector_window_->inspector();
    if (!ins) {
        log("warn", T("Task Inspector window not open, press Ctrl+I first"));
        return;
    }
    auto* it = ins->selectedTaskItem();
    if (!it) {
        log("warn", T("No task selected (select a row in Task Inspector → 📋 Tasks, then F5)"));
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
        log("warn", T("Auto chain is empty (define in ai_build.json auto field)"));
        return;
    }
    log("task", T("Run auto chain: %1").arg(cfg_.auto_chain.join(" → ")));
    auto_queue_ = cfg_.auto_chain;
    auto_index_ = 0;
    runNextInAuto();
}

void AbMainWindow::runNextInAuto() {
    // 2026-10-26 fix-chain-fail-stop: 上一个 task 失败 → 中断 auto 链 (跳过剩余).
    //   之前无条件继续: build 编译失败 (rc=2) 后 strip/launch-editor 照跑 — 把旧 binary
    //   又启动一遍, 最后一个 task rc=0 → ab 报 "全部完成", 用户以为编译成功
    //   (排查 "隐藏标签栏第二次加载仍显示" 时发现的真凶).
    if (last_task_exit_code_ != 0 && auto_index_ < auto_queue_.size()) {
        const QString failed = (auto_index_ > 0 && auto_index_ - 1 < auto_queue_.size())
                ? auto_queue_[auto_index_ - 1] : QString("?");
        log("err", T("⚠ auto chain aborted: task %1 failed (rc=%2), skipped %3 remaining task(s)")
                .arg(failed).arg(last_task_exit_code_).arg(auto_queue_.size() - auto_index_));
        auto_index_ = auto_queue_.size(); // 跳到末尾 → 走下方收尾 (TTS 失败 + auto-exit)
    }
    if (auto_index_ >= auto_queue_.size()) {
        // 2026-10-19 auto-exit-always: 之前只有 all_ok 才退, 用户报告 '有错误没自动退出'.
        //   改: --auto-exit 时**无论成功失败都退出**, 用退出码区分 (0=全成功, 非0=有失败).
        //   TTS 仍然播报成功/失败, GUI log 显示完整错误 (跟 ab-error-*.log 一起).
        //   这样 agent (sandbox) 跑完能立刻拿到 $? 决定下一步, 不需要 wait-for-user.
        bool all_ok = (last_task_exit_code_ == 0);
        log(all_ok ? "ok" : "warn",
            T("Auto chain finished (all_ok=%1 last_rc=%2 auto_exit=%3)")
            .arg(all_ok ? "true" : "false")
            .arg(last_task_exit_code_)
            .arg(cfg_.auto_exit ? "true" : "false"));
        speakTextAsync(all_ok ? "全部完成" : QString("失败 %1").arg(last_task_exit_code_));
        if (cfg_.auto_exit) {
            // 2026-10-19 user-configurable-auto-exit-wait: 用户原话 "有错误，没有错误，等待几秒，
            //   用户可以设置，保存，默认都是3秒, 而且有倒计时".
            //   改: 用 cfg_.auto_exit_wait_sec (默认 3), 范围 [0, 60], 0 = 立即退.
            //   显示倒计时 QTimer (每秒 -1) 让用户能 cancel auto-exit (点 "✕ 取消" 按钮).
            int final_rc = all_ok ? 0 : (last_task_exit_code_ != 0 ? last_task_exit_code_ : 1);
            int wait_sec = std::clamp(cfg_.auto_exit_wait_sec, 0, 60);
            int wait_ms  = wait_sec * 1000;
            // 2026-10-19 auto-exit-marker: 写退出码 marker 给 agent 看. 先写 (即使 ab crash 也拿到).
            QString marker = QString("/tmp/ab-auto-exit-%1.rc").arg(QCoreApplication::applicationPid());
            FILE* fp = fopen(marker.toUtf8().constData(), "w");
            if (fp) {
                fprintf(fp, "%d\n", final_rc);
                fclose(fp);
            }
            fprintf(stderr, "[AbMainWindow] auto_exit marker: %s = %d (wait %ds)\n",
                marker.toUtf8().constData(), final_rc, wait_sec);
            fflush(stderr);

            // 2026-10-19 countdown: 启动 QTimer 倒计时显示到 GUI + stderr. 用户能 cancel.
            cancel_auto_exit_ = false;  // 用户点 ✕ 时设 true, timer 检查到就不 quit
            if (auto_exit_countdown_timer_) delete auto_exit_countdown_timer_;
            auto_exit_countdown_timer_ = new QTimer(this);
            auto_exit_countdown_timer_->setInterval(1000);
            int remaining = wait_sec;
            QString countdown_msg = all_ok
                ? T("✓ auto chain succeeded, exiting in %1 s (click ✕ to cancel)").arg(remaining)
                : T("⚠ auto chain failed (rc=%1), exiting in %2 s — copy: cat /tmp/ab-error-*.log (click ✕ to cancel)")
                      .arg(final_rc).arg(remaining);
            log(all_ok ? "ok" : "warn", countdown_msg);
            if (auto_exit_cancel_btn_) {
                auto_exit_cancel_btn_->setVisible(true);
                auto_exit_cancel_btn_->setText(T("✕ Cancel Exit (%1s)").arg(remaining));
            }
            connect(auto_exit_countdown_timer_, &QTimer::timeout, this, [this, all_ok, final_rc, wait_ms]() {
                if (cancel_auto_exit_) {
                    log("info", T("✕ user cancelled auto-exit"));
                    if (auto_exit_cancel_btn_) auto_exit_cancel_btn_->setVisible(false);
                    if (auto_exit_countdown_timer_) { auto_exit_countdown_timer_->stop(); }
                    return;
                }
                remaining_counter_--;
                if (remaining_counter_ < 0) {
                    if (auto_exit_countdown_timer_) auto_exit_countdown_timer_->stop();
                    if (auto_exit_cancel_btn_) auto_exit_cancel_btn_->setVisible(false);
                    qApp->quit();
                    return;
                }
                if (auto_exit_cancel_btn_) {
                    auto_exit_cancel_btn_->setText(T("✕ Cancel Exit (%1s)").arg(remaining_counter_));
                }
                // 2026-10-26 fix: 成功分支只有 %1, 失败分支 %1+%2 — 分开 arg (之前合并写法
                //   在成功分支会报 "Argument missing" 警告)
                if (all_ok) {
                    log("ok", T("✓ success, exiting in %1 s (click ✕ to cancel)").arg(remaining_counter_));
                } else {
                    log("warn", T("⚠ failed (rc=%1), exiting in %2 s — copy cat /tmp/ab-error-*.log (click ✕ to cancel)")
                        .arg(final_rc).arg(remaining_counter_));
                }
            });
            auto_exit_countdown_timer_->start();
            // 立即触发一次让剩余时间从 wait_sec 开始显示
            remaining_counter_ = wait_sec;
        }
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
        log("warn", T("A task is already running, stop it first"));
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
        log("err", T("Unknown task: %1").arg(name));
        if (on_done) on_done();
        return;
    }

    log("task", T("▶ run task [%1]: %2 (mode=%3)")
        .arg(name, is_sub_mode ? T("sub-task list (%1 steps)").arg(sub_cmds.size()) : "single cmd",
             is_sub_mode ? "sub" : "single"));
    current_task_ = name;
    panel_task_name_ = name;
    // 标题: sub-task 模式显示 "task_name (N sub-tasks)"
    if (is_sub_mode) {
        updateWindowTitle(T("(%1 sub-tasks)").arg(sub_cmds.size()));
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
        log("warn", T("A task is already running, stop it first"));
        return;
    }
    log("task", T("▶ run cmd [%1]: %2").arg(task_name, cmd));
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
        log("warn", T("Stopping current task"));
        runner_->stop();
    } else {
        log("info", T("No task running"));
    }
}

// 2026-10-26 opt-perf: /tmp/ab-session.log 常驻句柄 (log() / onOutput 共用).
//   之前 onOutput 每行 fopen/fprintf/fclose — 一次编译几千行输出 = 几千组 open/close syscall.
static FILE* abSessionLogFp() {
    static FILE* s_fp = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        s_fp = std::fopen("/tmp/ab-session.log", "a");
        if (s_fp) {
            std::fprintf(s_fp, "\n===== ab session start %s =====\n",
                         QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData());
            std::fflush(s_fp);
        }
    }
    return s_fp;
}

void AbMainWindow::onOutput(const QString& task_name, const QString& line) {
    if (line.isEmpty()) return;
    QString ll = line.toLower();
    QString level = "info";
    if (ll.contains("error") || ll.contains("fatal") || ll.contains("failed")) level = "err";
    else if (ll.contains("warning")) level = "warn";
    else if (line.contains("✓") || ll.contains("[ok]")) level = "ok";

    // 2026-10-26 i18n: 右侧实时 log 显示层做"部分词汇翻译" (仅 zh 模式):
    //   "Compiling editor/editor_node.cpp ..." → "编译 editor/editor_node.cpp ..."
    //   "WARNING: msgfmt execution failed"     → "警告: msgfmt execution failed"
    //   文件名/路径/技术细节不动. /tmp/ab-session.log + stderr 仍写原文 (agent/管道拿原文).
    const QString display = AbLang::translateLogLine(line);

    // 2026-09-16 v10: 同步写 /tmp/ab-session.log (跟 log() 一样的 fallback, 跟 panel_log_view_ 互不干扰)
    //   用户反馈"实时 log 没输出" - 实际 panel_log_view_ 有内容, 但 /tmp/ab-session.log 里看不到
    //   sub-task cmd 输出 / ✓/✗ 标记 / ▶▶▶ desc 都走 onOutput, 现在也写到文件
    //   2026-10-26 opt-perf: 改用 abSessionLogFp() 常驻句柄, 不再每行 fopen/fclose
    if (FILE* fp = abSessionLogFp()) {
        const QByteArray lv_ba = level.toUtf8();
        const QByteArray tn_ba = task_name.toUtf8();
        const QByteArray ln_ba = line.toUtf8();
        std::fprintf(fp, "[%s] [%s] %s\n", lv_ba.constData(), tn_ba.constData(), ln_ba.constData());
        std::fflush(fp);
    }

    // 2026-09-16 v3: 写入中央面板右侧 log (主路径)
    if (panel_log_view_) {
        // 2026-10-15: 解析 ANSI 颜色 (scons 等会用 ESC[xxm 染色) + 符号标记
        //   ▶▶▶ / ✓ / ✗ / 🚀 加颜色; ESC 序列翻译成 <span style="color:...">
        appendColoredLine(panel_log_view_, display);
        // 自动滚到底
        if (auto* sb = panel_log_view_->verticalScrollBar()) {
            sb->setValue(sb->maximum());
        }
        // 更新 log 摘要
        if (panel_lbl_log_summary_) {
            panel_lbl_log_summary_->setText(
                T("📄 Live Log (%1 lines)").arg(panel_log_view_->blockCount()));
        }
    }

    // 兼容: 也写到 sb_left_ 状态栏 (log_dock_ 已移除, 不写 dock)
    if (sb_left_) {
        sb_left_->setText(QString("[%1] %2").arg(task_name, display.left(120)));
    }
}

void AbMainWindow::onFinished(const QString& task_name, int exit_code, double elapsed) {
    log(exit_code == 0 ? "ok" : "err",
        T("[%1] exit code %2  elapsed %3s").arg(task_name).arg(exit_code).arg(elapsed, 0, 'f', 1));
    last_task_exit_code_ = exit_code;  // 2026-10-09 add-auto-exit: 给 runNextInAuto 检查

    // 2026-10-15: task 结束 → 隐藏编译进度 label (避免残留)
    if (panel_lbl_compile_progress_) {
        panel_lbl_compile_progress_->setVisible(false);
    }

    // 2026-09-16 v3: 更新中央面板顶部状态 (status badge + elapsed 停 + 进度条满)
    if (panel_lbl_status_) {
        if (exit_code == 0) {
            panel_lbl_status_->setText(T("✓ Done"));
            // 2026-10-19 fix-theme: 用 dynamicProperty level 让 QSS 接管, 不硬编码绿
            panel_lbl_status_->setProperty("level", "ok");
            panel_lbl_status_->style()->unpolish(panel_lbl_status_);
            panel_lbl_status_->style()->polish(panel_lbl_status_);
            if (panel_progress_) {
                panel_progress_->setValue(panel_progress_->maximum());
                panel_progress_->setFormat(QString("✓ %1 / %1").arg(panel_progress_->maximum()));
            }
        } else {
            panel_lbl_status_->setText(T("✗ Fail (rc=%1)").arg(exit_code));
            panel_lbl_status_->setProperty("level", "err");
            panel_lbl_status_->style()->unpolish(panel_lbl_status_);
            panel_lbl_status_->style()->polish(panel_lbl_status_);
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
    log("err", T("[%1] QProcess error %2").arg(task_name).arg(err));
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
            QMessageBox::warning(this, T("Not Found"),
                T("%1 does not exist, run build+deploy first").arg(cfg_.run_after_build.binary_path));
            return;
        }
    }
    log("task", T("🚀 Launch %1 (background)").arg(QFileInfo(cloud_binary_).fileName()));
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
            sb_render_->setText(T("Renderer: Vulkan (Forward+)"));
            // 2026-10-19 fix-theme: 用 level=info 让 QSS 接管蓝色
            sb_render_->setProperty("level", "info");
            sb_render_->style()->unpolish(sb_render_);
            sb_render_->style()->polish(sb_render_);
        }
        speakTextAsync(QString("启动 %1").arg(QFileInfo(cloud_binary_).fileName()));
    } else {
        log("err", T("✗ Launch failed: %1").arg(cloud_binary_));
    }
}

void AbMainWindow::onRunCloudGL() {
    if (cloud_binary_.isEmpty()) {
        cloud_binary_ = findRunBinary();
        if (cloud_binary_.isEmpty()) {
            QMessageBox::warning(this, T("Not Found"),
                T("%1 does not exist, run build+deploy first").arg(cfg_.run_after_build.binary_path));
            return;
        }
    }
    log("task", T("🚀 Launch %1 (OpenGL compat mode)").arg(QFileInfo(cloud_binary_).fileName()));
    
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
                sb_render_->setText(T("Renderer: OpenGL3 (Compatibility)"));
                // 2026-10-19 fix-theme: 用 level=warn 让 QSS 接管橙色
                sb_render_->setProperty("level", "warn");
                sb_render_->style()->unpolish(sb_render_);
                sb_render_->style()->polish(sb_render_);
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
        log("err", T("✗ Launch failed: %1").arg(cloud_binary_));
    }
}

void AbMainWindow::onBuildAndRun() {
    log("task", "⚡ 编译并启动");
    QString t = !resolveTaskCmd("build+deploy").isEmpty() ? "build+deploy" : "build-only";
    if (t.isEmpty()) {
        QMessageBox::warning(this, T("Not Configured"), T("ai_build.json has no build+deploy task"));
        return;
    }
    cfg_.run_after_build.auto_run = true;
    runTaskByName(t);
}

void AbMainWindow::onBuildAndRunGL() {
    log("task", "⚡ 编译并启动 (OpenGL)");
    QString t = !resolveTaskCmd("build+deploy").isEmpty() ? "build+deploy" : "build-only";
    if (t.isEmpty()) {
        QMessageBox::warning(this, T("Not Configured"), T("ai_build.json has no build+deploy task"));
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
    log("ok", T("📊 Task Runner opened — %1").arg(task_name));
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
    log("ok", T("📋 Task Inspector opened (separate window — 📋 Tasks / ⚙️ Processes / 🔧 sub-task)"));
}

void AbMainWindow::onAbout() {
    QMessageBox::information(this, T("About ab"),
        T("ab — AI Build GUI (C++ Qt5/6)\nVersion: 1.0.0\nProject: %1\nTasks: %2\nTheme: %3\n\nab is a generic AI-assisted build tool,\ndriven by ai_build.json.\nTo change buttons/menus, edit the ui section of ai_build.json.")
        .arg(cfg_.cwd)
        .arg(static_cast<int>(cfg_.tasks.size()))
        .arg(cfg_.theme));
}

void AbMainWindow::onQuit() {
    close();
}

void AbMainWindow::log(const QString& level, const QString& msg) {
    // 2026-09-16 v7: 三路同步写, 确保 GUI/offscreen/headless 用户都能看到完整日志
    //   1) stderr: 终端跑 `ab` 时直接看 (带 fflush, SIGTERM 也不丢)
    //   2) 状态栏: GUI 模式下右下角状态条
    //   3) 文件 /tmp/ab-session.log: 兜底, GUI 脱离终端 / 重定向丢了也能 tail 看
    // 2026-10-26 fix-dangling: toUtf8() 返回临时 QByteArray, constData() 在语句结束后悬垂
    //   (之前 "[info]" 前缀偶尔变成消息内容本身, 如 "[任务数: 1] 任务数: 1")
    const QByteArray lvl_ba  = level.toUtf8();
    const QByteArray msg_ba  = msg.toUtf8();
    const char* lvl = lvl_ba.constData();
    const char* m_str = msg_ba.constData();
    std::fprintf(stderr, "[%s] %s\n", lvl, m_str);
    std::fflush(stderr);

    // 写文件 (追加模式, 静默失败不弹错) — 2026-10-26 opt-perf: 与 onOutput 共用常驻句柄
    if (FILE* s_log_fp = abSessionLogFp()) {
        std::fprintf(s_log_fp, "[%s] %s\n", lvl, m_str);
        std::fflush(s_log_fp);
    }

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
    // 2026-09-19: 实现真正的 reload — 重读 ai_build.json + 重建工具栏/菜单/按钮
    //   之前是 TODO, 用户改 ai_build.json 后必须重启 ab 才能生效, 体验差.
    //   现在: F5 快捷键 + QFileSystemWatcher 自动触发, 实时反映修改.
    if (config_path_.isEmpty()) {
        log("warn", "[reload] config_path_ 空, 跳过");
        return;
    }
    AbConfig new_cfg = AbConfig::load(config_path_);
    // sanity check: 加载失败 (空 cfg 或 cwd 变了) → 保留旧配置, 不破坏 UI
    if (new_cfg.cwd.isEmpty() && new_cfg.tasks.empty() && new_cfg.auto_chain.isEmpty()) {
        log("warn", QString("[reload] %1 加载失败 (解析错误?), 保留旧配置").arg(config_path_));
        return;
    }
    cfg_ = new_cfg;
    log("ok", QString("[reload] 已重读 ai_build.json: tasks=%1 toolbar=%2 buttons=%3 auto_chain=%4")
        .arg(cfg_.tasks.size())
        .arg(cfg_.toolbar.size())
        .arg(cfg_.buttons.size())
        .arg(cfg_.auto_chain.join("→")));

    // 1) 清掉旧 toolbar ("AbMainToolBar" 是 buildToolbar 创建的标识)
    QToolBar* old_tb = nullptr;
    for (QToolBar* tb : findChildren<QToolBar*>()) {
        if (tb->objectName() == "AbMainToolBar") { old_tb = tb; break; }
    }
    if (old_tb) {
        old_tb->clear();  // 清 action (不 delete, action 还在 actions_ 里等下重建)
        removeToolBar(old_tb);
        old_tb->deleteLater();
    }
    // 2) 清掉旧 main buttons (主窗口中心 widget 里加的按钮行)
    //    buildMainButtons 创建 QHBoxLayout (没设 objectName) 加到 vl 末尾
    if (QWidget* cw = centralWidget()) {
        if (QVBoxLayout* vl = qobject_cast<QVBoxLayout*>(cw->layout())) {
            // 从末尾往前删所有 QHBoxLayout (那是 buildMainButtons 加的)
            for (int i = vl->count() - 1; i >= 0; --i) {
                QLayoutItem* it = vl->itemAt(i);
                if (it && it->layout() && qobject_cast<QHBoxLayout*>(it->layout())) {
                    QLayout* l = it->layout();
                    // 清 layout 内的 widget
                    while (l->count() > 0) {
                        QLayoutItem* child = l->takeAt(0);
                        if (child->widget()) child->widget()->deleteLater();
                        delete child;
                    }
                    vl->removeItem(it);
                    delete l;
                }
            }
        }
    }
    // 3) 清 actions_ 和 buttons_ (让 buildToolbar / buildMainButtons 重建)
    //   Qt5 QHash 解引用直接返回值, 不是 std::pair — 用 key() 拿 key, 用 value() 拿 value
    //   (Qt6 的 begin/end 返 std::pair 才用 .first/.second)
    for (auto it = actions_.begin(); it != actions_.end(); ++it) {
        it.value()->deleteLater();
    }
    actions_.clear();
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) {
        it.value()->deleteLater();
    }
    buttons_.clear();

    // 4) 重建
    buildToolbar();
    buildMainButtons();
    updateWindowTitle();
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
        panel_progress_->setFormat(T("0 / %1  (sub-task)").arg(panel_total_sub_));
    }

    // 顶部 header
    if (panel_lbl_title_) {
        panel_lbl_title_->setText(panel_task_name_.isEmpty() ? T("⏸ No Task") : panel_task_name_);
        // 2026-10-19 fix-theme: 不硬编码标题色 (跟随主题)
    }
    if (panel_lbl_status_) {
        panel_lbl_status_->setText(T("⏳ Waiting"));
        // 2026-10-19 fix-theme: 用 level=info 让 QSS 接管灰色背景
        panel_lbl_status_->setProperty("level", "info");
        panel_lbl_status_->style()->unpolish(panel_lbl_status_);
        panel_lbl_status_->style()->polish(panel_lbl_status_);
    }
    if (panel_lbl_elapsed_) panel_lbl_elapsed_->setText("⏱ 00:00");
    panel_elapsed_timer_->start();

    // 当前执行区
    if (panel_lbl_current_idx_) {
        panel_lbl_current_idx_->setText("—");
        // 2026-10-19 fix-theme: 用 level=info 让 QSS 接管蓝色背景
        panel_lbl_current_idx_->setProperty("level", "info");
        panel_lbl_current_idx_->style()->unpolish(panel_lbl_current_idx_);
        panel_lbl_current_idx_->style()->polish(panel_lbl_current_idx_);
    }
    if (panel_lbl_current_dt_) panel_lbl_current_dt_->setText("⏱ 0s");
    if (panel_txt_current_cmd_) panel_txt_current_cmd_->clear();

    // log 区不清空 (保留上次输出), 用户可手动清
    // 摘要更新
    if (panel_lbl_log_summary_ && panel_log_view_) {
        panel_lbl_log_summary_->setText(
            T("📄 Live Log (%1 lines)").arg(panel_log_view_->blockCount()));
    }

    // 详情区
    if (panel_lbl_detail_meta_) panel_lbl_detail_meta_->setText(T("(no sub-task selected)"));
    if (panel_txt_detail_cmd_) panel_txt_detail_cmd_->clear();
    if (panel_btn_open_sub_log_) panel_btn_open_sub_log_->setEnabled(false);
    if (panel_btn_copy_sub_cmd_) panel_btn_copy_sub_cmd_->setEnabled(false);
    // 2026-10-19: 新任务开始 → 详情面板恢复隐藏 (用户点击 sub-task 再显示)
    if (panel_detail_box_) panel_detail_box_->setVisible(false);

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
                    ? T("  ⏳ [%1/%2] waiting...").arg(idx).arg(total)
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
        QString tip = T("sub-task [%1/%2]  running...\n").arg(idx).arg(total);
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
        panel_progress_->setFormat(T("%1 / %2  (sub-task)").arg(done).arg(total));
    }

    // 当前执行区
    if (panel_lbl_current_idx_) {
        // 2026-10-19 fix-theme: 用 level=ok 让 QSS 接管绿色背景 (跑完)
        panel_lbl_current_idx_->setProperty("level", "ok");
        panel_lbl_current_idx_->style()->unpolish(panel_lbl_current_idx_);
        panel_lbl_current_idx_->style()->polish(panel_lbl_current_idx_);
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
        // 2026-10-19 fix-theme: 用 level=warn 让 QSS 接管橙色背景 (running)
        panel_lbl_current_idx_->setProperty("level", "warn");
        panel_lbl_current_idx_->style()->unpolish(panel_lbl_current_idx_);
        panel_lbl_current_idx_->style()->polish(panel_lbl_current_idx_);
    }
    // 顶部 status badge
    if (panel_lbl_status_) {
        panel_lbl_status_->setText(T("⏳ Running (%1/%2)").arg(idx).arg(total));
        // 2026-10-19 fix-theme: 用 level=info 让 QSS 接管蓝色背景 (running)
        panel_lbl_status_->setProperty("level", "info");
        panel_lbl_status_->style()->unpolish(panel_lbl_status_);
        panel_lbl_status_->style()->polish(panel_lbl_status_);
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
        panel_lbl_status_->setText(T("✗ Failed at %1/%2").arg(idx).arg(total));
        // 2026-10-19 fix-theme: 用 level=err 让 QSS 接管红色背景 (failed)
        panel_lbl_status_->setProperty("level", "err");
        panel_lbl_status_->style()->unpolish(panel_lbl_status_);
        panel_lbl_status_->style()->polish(panel_lbl_status_);
    }
    // 失败时清掉编译进度 (避免残留)
    if (panel_lbl_compile_progress_) {
        panel_lbl_compile_progress_->setVisible(false);
    }
}

// 2026-10-15: 单 sub-task 内"编译 N 个文件"实时进度
//   - compile_count == 0: 隐藏 (新 sub-task 刚启动, 还没扫到任何编译动作)
//   - compile_count > 0:  显示 "🔨 编译 12 个 (foo.cpp)"
//   - 200ms 节流, 不会刷太快
void AbMainWindow::onSubProgress(const QString& task_name, int /*idx*/, int /*total*/,
                                 int compile_count, const QString& last_file) {
    if (task_name != current_task_) return;
    if (!panel_lbl_compile_progress_) return;
    if (compile_count <= 0) {
        panel_lbl_compile_progress_->setVisible(false);
        panel_lbl_compile_progress_->setText("");
        return;
    }
    QString text;
    if (last_file.isEmpty()) {
        text = T("🔨 compiled %1 files").arg(compile_count);
    } else {
        text = T("🔨 compiled %1 (%2)").arg(compile_count).arg(last_file);
    }
    panel_lbl_compile_progress_->setText(text);
    panel_lbl_compile_progress_->setVisible(true);
    // 截断超长文本 (label 不会自动 wrap, 用 elide 防止撑爆 header)
    QFontMetrics fm(panel_lbl_compile_progress_->font());
    int max_w = 460;
    QString elided = fm.elidedText(text, Qt::ElideRight, max_w);
    if (elided != text) panel_lbl_compile_progress_->setText(elided);
}

// ---- 中央面板 UI 槽 ----
void AbMainWindow::onPanelTaskListClicked() {
    if (!panel_sub_list_) return;
    QListWidgetItem* it = panel_sub_list_->currentItem();
    if (!it) {
        if (panel_lbl_detail_meta_) panel_lbl_detail_meta_->setText(T("(no sub-task selected)"));
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
    // 2026-10-19: 点击 sub-task → 显示详情面板 (如果之前被关闭)
    if (panel_detail_box_) panel_detail_box_->setVisible(true);
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
        log("info", T("⏹ Stopping task..."));
        runner_->stop();
    }
}

void AbMainWindow::onPanelCopyLog() {
    if (!panel_log_view_) return;
    QApplication::clipboard()->setText(panel_log_view_->toPlainText());
    log("ok", T("✓ log copied to clipboard"));
}

void AbMainWindow::onPanelOpenLogDir() {
    QString dir = panelLogDirForCurrentTask();
    QStringList candidates = { "xdg-open", "nautilus", "dolphin", "thunar", "pcmanfm" };
    for (const QString& c : candidates) {
        QProcess* p = new QProcess(this);
        p->start(c, {dir});
        if (p->waitForStarted(1500)) {
            log("ok", T("📂 Opened %1").arg(dir));
            return;
        }
        delete p;
    }
    log("warn", T("⚠ No file manager found, log dir: %1").arg(dir));
}

// 2026-09-20: 移除 AbMainWindow::onPanelLaunchCloud() — 这是 godot_ui_linux 项目专用
//   启动 cloud_main (Vulkan/GL) 的逻辑, 不属于 ai_tools 默认工具栏功能.
//   需要在项目内启动 cloud_main 时, 走 ai_build.json 的 start-cloud-main task.
//   相关成员变量 panel_btn_launch_cloud_ / panel_btn_launch_cloud_gl_ 也已从 .h 移除.

}  // namespace ab
