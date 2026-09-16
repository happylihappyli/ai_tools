// AbTaskRunner.cpp
// 2026-09-16 升级: 支持 sub-task list (串行跑多 cmd, 每 cmd 实时 emit sub_started/sub_finished)
#include "AbTaskRunner.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>

namespace ab {

AbTaskRunner::AbTaskRunner(QObject* parent) : QObject(parent) {}

AbTaskRunner::~AbTaskRunner() {
    if (proc_) {
        if (proc_->state() != QProcess::NotRunning) {
            proc_->kill();
            proc_->waitForFinished(2000);
        }
        delete proc_;
    }
}

// 共享: 准备 QProcess (lazy init + signal connect)
QProcess* AbTaskRunner::ensureProc() {
    if (!proc_) {
        proc_ = new QProcess(this);
        connect(proc_, &QProcess::readyReadStandardOutput,
                this, [this]() { onReadyRead(); });
        connect(proc_, &QProcess::readyReadStandardError,
                this, [this]() { onReadyRead(); });
        connect(proc_, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, &AbTaskRunner::onProcFinished);
        connect(proc_, &QProcess::errorOccurred, this, &AbTaskRunner::onProcError);
    }
    return proc_;
}

// 跑单 task (shell 模式, 兼容 && 链) - 兼容老 API
void AbTaskRunner::run(const QString& task_name, const QString& cmd,
                       const QString& working_dir, const QStringList& /*env*/) {
    if (isRunning()) {
        qWarning() << "[AbTaskRunner] already running, ignoring";
        return;
    }
    task_name_  = task_name;
    pending_.clear();
    sub_total_  = 0;   // 标记为单 task 模式 (区别于 sub-task 模式)
    sub_idx_    = 0;
    sub_cmds_.clear();
    sub_descs_.clear();
    QProcess* p = ensureProc();
    p->setWorkingDirectory(working_dir);
    p->setProcessChannelMode(QProcess::SeparateChannels);

    // shell 模式: /bin/sh -c "<cmd>"
    QStringList args;
    args << "-c" << cmd;
    start_t_ = std::chrono::steady_clock::now();
    p->start("/bin/sh", args);
}

// 2026-09-16 新增: 跑 sub-task list (串行, 失败立即停)
void AbTaskRunner::runSubTasks(const QString& task_name,
                               const QStringList& sub_cmds,
                               const QStringList& sub_descs,
                               const QString& working_dir,
                               const QStringList& /*env*/) {
    if (isRunning()) {
        qWarning() << "[AbTaskRunner] already running, ignoring runSubTasks";
        return;
    }
    if (sub_cmds.isEmpty()) {
        qWarning() << "[AbTaskRunner] runSubTasks called with empty sub_cmds";
        emit error(task_name, -1);
        return;
    }
    task_name_   = task_name;
    sub_cmds_    = sub_cmds;
    sub_descs_   = sub_descs;
    sub_total_   = sub_cmds.size();
    sub_idx_     = 0;  // 下一个要跑的 idx (0-indexed)
    pending_.clear();
    QProcess* p = ensureProc();
    p->setWorkingDirectory(working_dir);
    p->setProcessChannelMode(QProcess::SeparateChannels);

    runNextSubTask();
}

// 启动当前 sub_idx_ 的 sub-task
void AbTaskRunner::runNextSubTask() {
    if (sub_idx_ >= sub_total_) {
        // 全部 sub-task 完成, 恢复单 task 模式
        sub_total_ = 0;
        sub_cmds_.clear();
        sub_descs_.clear();
        return;
    }
    QString sub_cmd  = sub_cmds_[sub_idx_];
    QString sub_desc = (sub_idx_ < sub_descs_.size()) ? sub_descs_[sub_idx_] : QString();

    // 标记 task 开始时间 (整 task 的 elapsed)
    if (sub_idx_ == 0) {
        start_t_ = std::chrono::steady_clock::now();
    }
    sub_t0_ = std::chrono::steady_clock::now();

    // emit sub_started (idx 从 1 开始, total 是 sub-task 总数)
    emit sub_started(task_name_, sub_idx_ + 1, sub_total_, sub_desc, sub_cmd);

    if (!proc_) {
        qWarning() << "[AbTaskRunner] proc_ is null in runNextSubTask";
        return;
    }
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    QStringList args;
    args << "-c" << sub_cmd;
    proc_->start("/bin/sh", args);
}

bool AbTaskRunner::isRunning() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

void AbTaskRunner::stop() {
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->terminate();
        if (!proc_->waitForFinished(2000)) {
            proc_->kill();
        }
    }
    // 2026-09-16: 停止时也清 sub-task 模式标志
    sub_total_ = 0;
    sub_cmds_.clear();
    sub_descs_.clear();
}

void AbTaskRunner::onReadyRead() {
    if (!proc_) return;
    QByteArray data;
    // 读 stdout
    data = proc_->readAllStandardOutput();
    pending_ += QString::fromUtf8(data);
    data = proc_->readAllStandardError();
    pending_ += QString::fromUtf8(data);

    // 按行切分
    int idx;
    while ((idx = pending_.indexOf('\n')) >= 0) {
        QString line = pending_.left(idx);
        pending_.remove(0, idx + 1);
        if (line.endsWith('\r')) line.chop(1);
        emit output(task_name_, line);
    }
}

void AbTaskRunner::onProcFinished(int exit_code, QProcess::ExitStatus /*status*/) {
    // 任何剩余的 pending 一次性 emit
    if (!pending_.isEmpty()) {
        emit output(task_name_, pending_);
        pending_.clear();
    }

    // 2026-09-16: sub-task 模式分支
    if (sub_total_ > 0) {
        double dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - sub_t0_).count();
        int idx1 = sub_idx_ + 1;  // 1-indexed 给 UI
        emit sub_finished(task_name_, idx1, sub_total_, exit_code, dt);

        if (exit_code != 0) {
            // 失败: emit sub_failed, 然后 emit 整 task finished (rc = exit_code)
            emit sub_failed(task_name_, idx1, sub_total_);
            sub_total_ = 0;
            sub_cmds_.clear();
            sub_descs_.clear();
            double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_t_).count();
            emit finished(task_name_, exit_code, elapsed);
            return;
        }
        // 成功: 继续下一个
        sub_idx_++;
        runNextSubTask();
        return;
    }

    // 单 task 模式 (老 API)
    auto end_t = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_t - start_t_).count();
    emit finished(task_name_, exit_code, elapsed);
}

void AbTaskRunner::onProcError(QProcess::ProcessError err) {
    emit error(task_name_, static_cast<int>(err));
    // 2026-09-16: QProcess 错误 (启动失败 / 崩溃) 也清 sub-task 模式
    if (sub_total_ > 0) {
        sub_total_ = 0;
        sub_cmds_.clear();
        sub_descs_.clear();
    }
}

}  // namespace ab