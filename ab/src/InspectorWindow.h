#ifndef AB_INSPECTOR_WINDOW_H
#define AB_INSPECTOR_WINDOW_H
// SPDX-License-Identifier: MIT
//
// InspectorWindow — 独立任务检查器窗口 (2026-09-16 v4)
//
// 设计:
//   - 独立 QMainWindow, 通过 ab 工具栏"📋 任务检查器"按钮弹出
//   - 中央 widget = AbTaskInspector (任务/进程/sub-task 三 tab)
//   - 接 AbTaskRunner* signals → 转发到 AbTaskInspector (跟 AbMainWindow 之前一样)
//     onTaskStarted/onTaskFinished/onTaskRunning/onTaskSubStarted/.../Finished/Failed
//   - requestRunTask → 转给 caller (MainWindow 调 runTaskByName)
//
// 跟 TaskRunnerWindow 的区别:
//   - TaskRunnerWindow 是"任务运行器" (仿 ac_task_runner_gui.py 实时跑任务的 UI)
//   - InspectorWindow 是"任务检查器" (看 ai_build.json 任务列表 + 进程 + 当前 task 的 sub-task)
//
// 2026-09-16 v4: 新增, 把 inspector_ dock 从主窗口拆出来.

#include <QMainWindow>
#include <QString>

#include "AbConfig.h"

class QWidget;

namespace ab {

class AbTaskRunner;
class AbTaskInspector;

class InspectorWindow : public QMainWindow {
    Q_OBJECT
public:
    InspectorWindow(AbTaskRunner* runner,
                    const AbConfig& cfg,
                    const QString& cwd,
                    QWidget* parent = nullptr);
    ~InspectorWindow() override;

    AbTaskInspector* inspector() const { return inspector_; }

    // 2026-09-16 v4: 项目配置变化时刷新 (跟 AbMainWindow 之前一样)
    void setConfig(const AbConfig& cfg);
    void setCwd(const QString& cwd);

signals:
    void requestRunTask(const QString& task_name);

private slots:
    // 接 AbTaskRunner signals → 转发给 inspector_
    void onTaskFinished(const QString& task_name, int exit_code, double elapsed);
    void onSubStarted(const QString& task_name, int idx, int total,
                      const QString& desc, const QString& cmd);
    void onSubFinished(const QString& task_name, int idx, int total,
                       int rc, double dt_sec);
    void onSubFailed(const QString& task_name, int idx, int total);
    // 接 inspector_ signal → 转发出去
    void onInspectorRequestRunTask(const QString& task_name);

private:
    void buildUi();

    AbTaskRunner*    runner_     = nullptr;
    AbTaskInspector* inspector_  = nullptr;
    AbConfig         cfg_;
    QString          cwd_;
};

}  // namespace ab

#endif