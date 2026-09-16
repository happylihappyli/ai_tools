#ifndef AB_TASK_INSPECTOR_H
#define AB_TASK_INSPECTOR_H
// SPDX-License-Identifier: MIT
//
// AbTaskInspector — 任务 + 进程 + sub-task 三区 dock
//
// 2026-09-16 v3: 新增 sub-task tab (📊), 显示当前 task 的 sub-task 列表 + 进度条
//   信号 requestRunTask(QString) → MainWindow 调 runTaskByName
//   sub-task 事件 (由 MainWindow 从 runner_ 转过来):
//     onTaskSubStarted → 清空列表 + 预填所有 sub-task (查 cfg_) + 标记当前为 running
//     onTaskSubFinished → 标记该 sub-task 为 ✓/✗
//     onTaskSubFailed → 标记当前为 ✗
// 2026-09-08 v2: 任务 tab 加 "命令" 列 + 双击直接跑 (之前主窗口 GroupBox 的功能合并进来)
// 2026-09-02 v1: 新增. 显示:
//   [任务]  ai_build.json 任务列表 + 状态 (⏸/🟡/✓/✗) + 历史 (最近 rc/耗时/时间)
//   [进程]  系统进程 (ps), 过滤项目相关 (cloud_main/scons/cmake/godot/...)
//
// 状态历史持久化: ~/.config/ai_tools/task_history.json
// 自动刷新: 3s (QTimer)

#include <QDockWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QListWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>

#include "AbConfig.h"

class QProcess;

namespace ab {

class AbTaskInspector : public QDockWidget {
    Q_OBJECT
public:
    explicit AbTaskInspector(QWidget* parent = nullptr);

    // 项目配置变化时刷新 (重新读 cfg_ + 历史)
    void setConfig(const AbConfig& cfg);
    void setCwd(const QString& cwd) { cwd_ = cwd; refreshProcesses(); }

    // 2026-09-08 v2: 给 MainWindow 用, 拿当前选中的任务行 (供 F5 跑选中)
    //   没选中返回 nullptr
    QTreeWidgetItem* selectedTaskItem() const {
        return task_tree_ ? task_tree_->currentItem() : nullptr;
    }

    // 任务开始 / 完成回调 (由 MainWindow 调)
    void onTaskStarted(const QString& task_name);
    void onTaskFinished(const QString& task_name, int exit_code, double elapsed);
    void onTaskRunning(const QString& task_name);  // 当前在跑

    // 2026-09-16 v3: sub-task 事件 (MainWindow 从 AbTaskRunner 转过来)
    //   sub-task tab 显示当前 task 的所有 sub-task + 状态
    void onTaskSubStarted(const QString& task_name,
                          int idx, int total,
                          const QString& desc, const QString& cmd);
    void onTaskSubFinished(const QString& task_name,
                           int idx, int total,
                           int rc, double dt_sec);
    void onTaskSubFailed(const QString& task_name,
                         int idx, int total);

signals:
    // 2026-09-08 v2: 用户双击任务 tab 行, 请求 MainWindow 跑
    //   MainWindow 在 buildFromConfig 里 connect 到自己的 runTaskByName
    void requestRunTask(const QString& task_name);

public slots:
    void onRefreshClicked();
    void onAutoRefreshToggled(bool checked);
    void onFilterChanged(const QString& text);
    void onAutoTick();      // 定时器: 刷新进程 + 当前任务状态
    void onItemDoubleClicked(QTreeWidgetItem* it, int col);

private:
    void buildUi();
    void loadHistory();
    void saveHistory();
    void refreshTasks();
    void refreshProcesses();
    void killProcess(int pid);
    // 2026-09-16 v3: 切换到 sub-task tab + 预填列表
    void switchToSubTaskTab(const QString& task_name);
    // 找到 task_name 在 cfg_ 的 sub-task list (sub_cmds/sub_descs)
    bool findTaskSubLists(const QString& task_name,
                          QStringList& out_cmds, QStringList& out_descs) const;

    QTabWidget*   tabs_       = nullptr;
    // 任务区
    QTreeWidget*  task_tree_  = nullptr;
    // 进程区
    QTreeWidget*  proc_tree_  = nullptr;
    // 2026-09-16 v3 新增: sub-task 区
    QWidget*      sub_task_panel_  = nullptr;  // 包 list + progress + 标题
    QLabel*       sub_task_title_  = nullptr;  // 顶部: "🔧 reset-and-restart (10 sub-tasks)"
    QListWidget*  sub_task_list_  = nullptr;  // sub-task 列表 (desc + 状态图标)
    QLabel*       sub_task_info_  = nullptr;  // 底部: "进度 3/10 · 耗时 12.5s"
    QProgressBar* sub_task_prog_  = nullptr;  // 进度条
    QPushButton*  refresh_btn_= nullptr;
    QCheckBox*    auto_chk_   = nullptr;
    QLineEdit*    filter_edit_= nullptr;
    QLabel*       info_lbl_   = nullptr;
    QTimer        auto_timer_;

    AbConfig cfg_;
    QString  cwd_;

    // 状态: 任务名 → {last_rc, last_elapsed, last_time, last_status}
    struct TaskStat {
        QString status;       // "ready" / "running" / "ok" / "err"
        int     last_rc     = 0;
        double  last_elapsed = 0;
        QString last_time;    // ISO-ish "2026-09-02 11:30:45"
        int     run_count    = 0;
        int     err_count    = 0;
    };
    QHash<QString, TaskStat> stats_;
    QString current_running_;   // 当前在跑的任务名

    // 2026-09-16 v3 新增: sub-task 运行状态
    struct SubTaskState {
        int     idx     = 0;   // 1-indexed
        QString desc;
        QString cmd;
        QString status;         // "pending" / "running" / "ok" / "err"
        double  dt_sec  = 0;
    };
    QString                       current_subtask_task_;  // 当前在跑 sub-task 的父 task
    QList<SubTaskState>            current_sub_states_;   // 当前 task 的 sub-task list (按 sub_idx 排)
    QHash<int, QListWidgetItem*>  sub_item_by_idx_;      // idx → list item, 用于更新状态
};

}  // namespace ab

#endif