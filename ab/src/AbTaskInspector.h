#ifndef AB_TASK_INSPECTOR_H
#define AB_TASK_INSPECTOR_H
// SPDX-License-Identifier: MIT
//
// AbTaskInspector — 任务 + 进程 双区 dock
//
// 2026-09-02: 新增. 显示:
//   [任务]  ai_build.json 任务列表 + 状态 (⏸/🟡/✓/✗) + 历史 (最近 rc/耗时/时间)
//   [进程]  系统进程 (ps), 过滤项目相关 (cloud_main/scons/cmake/godot/...)
//
// 状态历史持久化: ~/.config/ai_tools/task_history.json
// 自动刷新: 3s (QTimer)

#include <QDockWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
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

    // 任务开始 / 完成回调 (由 MainWindow 调)
    void onTaskStarted(const QString& task_name);
    void onTaskFinished(const QString& task_name, int exit_code, double elapsed);
    void onTaskRunning(const QString& task_name);  // 当前在跑

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

    QTabWidget*   tabs_       = nullptr;
    // 任务区
    QTreeWidget*  task_tree_  = nullptr;
    // 进程区
    QTreeWidget*  proc_tree_  = nullptr;
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
};

}  // namespace ab

#endif
