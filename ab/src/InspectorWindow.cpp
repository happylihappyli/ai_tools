// InspectorWindow.cpp
// SPDX-License-Identifier: MIT
//
// InspectorWindow — 独立任务检查器窗口 (2026-09-16 v4)

#include "InspectorWindow.h"
#include "AbTaskRunner.h"
#include "AbTaskInspector.h"

#include <QApplication>
#include <QString>

namespace ab {

InspectorWindow::InspectorWindow(AbTaskRunner* runner,
                                 const AbConfig& cfg,
                                 const QString& cwd,
                                 QWidget* parent)
    : QMainWindow(parent), runner_(runner), cfg_(cfg), cwd_(cwd) {
    setWindowTitle(QString("ab — 任务检查器 (📋 任务 + ⚙️ 进程 + 🔧 sub-task)"));
    resize(900, 600);

    buildUi();

    // 接 AbTaskRunner signals
    if (runner_) {
        connect(runner_, &AbTaskRunner::finished,
                this, &InspectorWindow::onTaskFinished);
        connect(runner_, &AbTaskRunner::sub_started,
                this, &InspectorWindow::onSubStarted);
        connect(runner_, &AbTaskRunner::sub_finished,
                this, &InspectorWindow::onSubFinished);
        connect(runner_, &AbTaskRunner::sub_failed,
                this, &InspectorWindow::onSubFailed);
    }

    // 接 inspector_ signal → 转给外面
    connect(inspector_, &AbTaskInspector::requestRunTask,
            this, &InspectorWindow::onInspectorRequestRunTask);

    // 关窗口时不退出 app (只是隐藏)
    setAttribute(Qt::WA_DeleteOnClose, false);
}

InspectorWindow::~InspectorWindow() {
}

void InspectorWindow::setConfig(const AbConfig& cfg) {
    cfg_ = cfg;
    if (inspector_) inspector_->setConfig(cfg);
}

void InspectorWindow::setCwd(const QString& cwd) {
    cwd_ = cwd;
    if (inspector_) inspector_->setCwd(cwd);
}

void InspectorWindow::buildUi() {
    inspector_ = new AbTaskInspector(this);
    inspector_->setConfig(cfg_);
    inspector_->setCwd(cwd_);
    setCentralWidget(inspector_);
}

void InspectorWindow::onTaskFinished(const QString& task_name, int exit_code, double elapsed) {
    if (inspector_) inspector_->onTaskFinished(task_name, exit_code, elapsed);
}

void InspectorWindow::onSubStarted(const QString& task_name, int idx, int total,
                                   const QString& desc, const QString& cmd) {
    if (inspector_) inspector_->onTaskSubStarted(task_name, idx, total, desc, cmd);
}

void InspectorWindow::onSubFinished(const QString& task_name, int idx, int total,
                                    int rc, double dt_sec) {
    if (inspector_) inspector_->onTaskSubFinished(task_name, idx, total, rc, dt_sec);
}

void InspectorWindow::onSubFailed(const QString& task_name, int idx, int total) {
    if (inspector_) inspector_->onTaskSubFailed(task_name, idx, total);
}

void InspectorWindow::onInspectorRequestRunTask(const QString& task_name) {
    emit requestRunTask(task_name);
}

}  // namespace ab