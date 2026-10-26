#ifndef AB_MAIN_WINDOW_H
#define AB_MAIN_WINDOW_H
// SPDX-License-Identifier: MIT
//
// AbMainWindow — ab 主窗口 (2026-09-16 v3 全面改造)
//
// 布局 (仿 ac_task_runner_gui.py):
//   [工具栏]   ← 保留项目特定 buttons (build/run/stop/cloud)
//   ─────────────────────────────────────────────────
//   [顶部 header]   当前任务名 + status badge + elapsed
//   [进度条]        sub-task 进度
//   [⚡ 当前执行]   idx badge + desc + 实时计时器
//   [split 左|右]   sub-task 列表  |  实时 log
//   [详情区]        选中 sub-task 的完整命令 + log 路径
//   [底部按钮行]    Stop / 复制 log / 打开 log 目录 / 启动 cloud_main / 启动 cloud_main (GL)
//   [状态栏]        动态 log + 项目路径 + auto 链 + Qt 主题
//   [Dock 右侧]     进程检查器 (inspector_ 只剩进程 tab, 任务列表合并到中央面板)
//
// 改动:
//   - 移除 prog_label_/prog_bar_ (合并到顶部 header + 进度条)
//   - 2026-09-16 v4: 移除 inspector_ dock, 改成独立 InspectorWindow
//   - 2026-09-16 v3: 移除 log_dock_ (log 已在中央面板)
//   - inspector_ 任务 tab 改为只显示进程 (避免重复)

#include <QMainWindow>
#include <QString>
#include <QHash>
#include <QStringList>
#include <QList>
// 2026-09-19: ai_build.json 热重载 (auto-reload + F5 手动 reload)
#include <QFileSystemWatcher>
#include <functional>
#include "AbConfig.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QProgressBar;
class QPushButton;
class QToolButton;
class QSpinBox;  // 2026-10-19: auto-exit 倒计时秒数配置 spinbox
class QStatusBar;
class QAction;
class QMenu;
class QToolBar;
class QFrame;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QPlainTextEdit;
class QGroupBox;
class QTimer;
class QScrollBar;

namespace ab {

class AbTaskRunner;
class InspectorWindow;

class AbMainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AbMainWindow(const AbConfig& cfg, const QString& configPath = QString(), QWidget* parent = nullptr);
    ~AbMainWindow() override;

    void reloadConfig();

protected:
    // 2026-10-19: 窗口缩放后强制保持 1:2 splitter 比例 (setSizes 在初次布局前不生效)
    void resizeEvent(QResizeEvent* e) override;

public slots:
    void onOutput(const QString& task_name, const QString& line);
    void onFinished(const QString& task_name, int exit_code, double elapsed);
    void onError(const QString& task_name, int err);

    // 通用槽: 按钮/菜单触发
    void onRunSelectedTask();
    void onRunAuto();
    void onStop();
    void onRunCloud();
    void onRunCloudGL();
    void onOpenAR();
    void onBuildAndRun();
    void onBuildAndRunGL();
    void onToggleTheme();
    void onToggleLogDock(bool checked);
    void onAbout();
    void onQuit();
    // 2026-09-16: 打开独立 TaskRunnerWindow (备份独立窗口, 主要在中央面板看)
    void onOpenTaskRunner();
    // 2026-09-16 v4: 打开独立 InspectorWindow (任务检查器, 主窗口不再显示 dock)
    void onOpenInspector();

    // 通用槽: 按 id 触发 (按钮/工具栏/菜单 共用)
    void onActionTriggered();

    // 2026-09-08 v2: 桥接 inspector signal (1 参) → runTaskByName (2 参, on_done 默认 nullptr)
    void onInspectorRunTask(const QString& task_name);

    // 2026-09-16 v3: 中央面板 sub-task UI 槽 (接 runner signals)
    void onSubStartedPanel(const QString& task_name, int idx, int total,
                           const QString& desc, const QString& cmd);
    void onSubFinishedPanel(const QString& task_name, int idx, int total,
                            int rc, double dt_sec);
    void onSubFailedPanel(const QString& task_name, int idx, int total);
    // 2026-10-15: 单 sub-task 内"编译 N 个文件"实时进度 (200ms 节流)
    void onSubProgress(const QString& task_name, int idx, int total,
                       int compile_count, const QString& last_file);

    // 中央面板 UI 槽
    void onPanelTaskListClicked();      // 点击左侧 sub-task 行 → 刷新详情区
    void onPanelUpdateElapsed();         // 1s 周期
    void onPanelUpdateCurrentDt();       // 0.5s 周期

    void onPanelStopClicked();
    void onPanelCopyLog();
    void onPanelOpenLogDir();
    // 2026-09-20: 移除 onPanelLaunchCloud — 不属于 ai_tools 默认工具栏功能

private:
    void buildFromConfig();
    void buildBuiltInMenus();
    void buildBuiltInToolbar();
    void buildBuiltInButtons();
    void buildMenus();
    void buildToolbar();
    void buildMainButtons();
    void buildCentralPanel();   // 2026-09-16 v3: 新中央面板 (顶部 + split + 详情 + 底部按钮)
    void buildStatusBar();
    void wireRunner();
    // 2026-10-26 i18n: 语言切换后整体重建 UI (菜单/工具栏/中央面板/状态栏 全按新语言生成)
    void rebuildUi();
    // 2026-10-19 加 — 启动时弹大字体模态窗口显示 ai_build.json 的 startup_prompts.
    //   每段一个 dialog (按 OK 跳下一段), 字体大小 startup_prompt_font_pt (默认 28).
    //   allow_modal=false (语言切换 rebuildUi 用): 只回填 🔔 按钮, 不重新弹模态框.
    void showStartupPrompts(bool allow_modal = true);

    void enableRunCloudButton(bool en);
    QString findRunBinary() const;
    QString findTool(const QString& name) const;
    void runTaskByName(const QString& name, std::function<void()> on_done = nullptr);
    void runCmd(const QString& cmd, const QString& task_name = "<cmd>");
    void log(const QString& level, const QString& msg);
    QString resolveTaskCmd(const QString& name, QStringList* out_subs = nullptr,
                           QStringList* out_descs = nullptr) const;
    void updateWindowTitle(const QString& current_cmd = QString());
    void speakTextAsync(const QString& text, bool log_when_disabled = false);
    void speakTaskFinished(const QString& task_name, int exit_code) const;
    void runAutoQueue();
    void runNextInAuto();
    QAction* createActionForButton(const AbButtonDef& b, QWidget* parent);

    // 2026-09-16 v3: 中央面板 sub-task 状态 + 操作
    void panelResetUi(int total);
    void panelAppendSubTaskItem(int idx, int total, const QString& desc, const QString& cmd);
    void panelUpdateRunningItem(int idx, int total);
    void panelUpdateFinishedItem(int idx, int total, int rc, double dt);
    QString panelLogDirForCurrentTask() const;
    // 给 task_name 找 sub-task list, 找不到返回 false
    bool panelFindTaskLists(const QString& name, QStringList& cmds, QStringList& descs) const;

    AbConfig cfg_;
    // 2026-09-19: ai_build.json 路径 — 用于 reload + QFileSystemWatcher 自动重读
    QString config_path_;
    // 2026-09-19: 监听 ai_build.json 改动, 自动 reload (免去重启 ab)
    QFileSystemWatcher* config_watcher_ = nullptr;
    AbTaskRunner* runner_ = nullptr;
    // 2026-09-16 v4: 任务检查器改为独立窗口 (主窗口不再 dock)
    InspectorWindow* inspector_window_ = nullptr;
    class TaskRunnerWindow* task_runner_win_ = nullptr;  // 独立窗口备份

    // ============== 中央面板成员 (仿 Python RunnerWindow) ==============
    // 顶部 header
    QLabel*      panel_lbl_title_     = nullptr;
    QLabel*      panel_lbl_elapsed_   = nullptr;
    QLabel*      panel_lbl_status_    = nullptr;
    // 2026-10-15: 编译进度 (scons "Compiling xxx" 实时累计)
    QLabel*      panel_lbl_compile_progress_ = nullptr;
    // 2026-10-19 startup-prompt-button: 顶部 header 提示按钮 (默认隐藏, 有 prompt 时显示, 点开看)
    QToolButton*  panel_btn_prompts_  = nullptr;
    // 进度条
    QProgressBar*panel_progress_      = nullptr;
    // 当前执行区
    QFrame*      panel_current_box_   = nullptr;
    // 2026-09-16 v8: 把"⚡ 当前执行"和"📜 选中任务详情"合并到 QTabWidget (panel_task_tab_)
    QTabWidget*  panel_task_tab_      = nullptr;
    QLabel*      panel_lbl_current_idx_ = nullptr;
    QLabel*      panel_lbl_current_dt_  = nullptr;
    QPlainTextEdit* panel_txt_current_cmd_ = nullptr;
    // 中间 split
    QSplitter*   panel_splitter_      = nullptr;
    QListWidget* panel_sub_list_      = nullptr;
    QLabel*      panel_lbl_log_summary_ = nullptr;
    QPlainTextEdit* panel_log_view_  = nullptr;
    // 详情区
    QGroupBox*   panel_detail_box_    = nullptr;
    QLabel*      panel_lbl_detail_meta_ = nullptr;
    QPushButton* panel_btn_open_sub_log_ = nullptr;
    QPushButton* panel_btn_copy_sub_cmd_ = nullptr;
    QPushButton* panel_btn_close_detail_ = nullptr;  // 2026-10-19: 关闭按钮 (默认隐藏)
    QPlainTextEdit* panel_txt_detail_cmd_ = nullptr;
    // 顶部工具栏 (2026-09-16 v7: 5 个按钮合并到这里, 不再占底部空间)
    QToolBar*    panel_toolbar_      = nullptr;
    // 顶部工具栏按钮 (2026-09-20: 工具栏只保留 Stop, 复制 log / 打开 log 目录已移到菜单)
    QPushButton* panel_btn_stop_      = nullptr;
    // 2026-09-20: 移除 panel_btn_copy_log_ / panel_btn_open_dir_ — 已移到 "日志(&L)" 菜单
    // 2026-09-20: 移除 panel_btn_launch_cloud_ / panel_btn_launch_cloud_gl_ — 不属于 ai_tools 默认工具栏功能
    // 计时器
    QTimer*      panel_elapsed_timer_ = nullptr;
    QTimer*      panel_current_dt_timer_ = nullptr;

    // 状态栏
    QStatusBar*    statusbar_  = nullptr;
    QLabel*        sb_left_    = nullptr;
    QLabel*        sb_proj_    = nullptr;
    QLabel*        sb_right_   = nullptr;
    QLabel*        sb_render_  = nullptr;

    // 2026-10-19 auto-exit-countdown: 倒计时 + cancel 按钮. cfg_.auto_exit_wait_sec 默认 3,
    //   启动 QTimer 每秒 -1, 倒计时显示到 GUI + log. 用户点 ✕ 设 cancel_auto_exit_=true 跳过 quit.
    QTimer*   auto_exit_countdown_timer_ = nullptr;
    int       remaining_counter_ = 0;
    bool      cancel_auto_exit_  = false;
    QPushButton* auto_exit_cancel_btn_ = nullptr;
    QSpinBox* auto_exit_wait_spin_ = nullptr;  // GUI 配置: 改 cfg_.auto_exit_wait_sec (写回 ai_build.json / QSettings)

    // 中央面板状态
    int     panel_total_sub_      = 0;
    int     panel_selected_sub_idx_ = 0;
    qint64  panel_task_start_ms_   = 0;
    qint64  panel_current_sub_start_ms_ = 0;
    QString panel_task_name_;
    struct PanelSubState {
        int     rc      = 0;
        double  dt      = 0;
        QString cmd;
        QString desc;
        QString log_path;
        QString status;   // "pending" / "running" / "ok" / "fail"
    };
    QHash<int, PanelSubState> panel_sub_states_;

    // 通用状态
    bool   current_aborted_ = false;
    QStringList auto_queue_;
    int    auto_index_ = 0;
    // 2026-10-09 add-auto-exit: 上一个 task 退出码. 0=成功, !=0=失败. auto_exit 模式下
    //   失败时不要退 (让用户看错误); 链成功跑完时才退.
    int    last_task_exit_code_ = 0;
    QString current_task_;
    QString current_cmd_;
    std::function<void()> current_on_done_;
    QString cloud_binary_;
    QString ac_binary_;
    QString ar_binary_;
    QString spd_say_binary_;
    bool tts_enabled_ = true;

    QHash<QString, QAction*> actions_;
    QHash<QString, QPushButton*> buttons_;
};

}  // namespace ab

#endif