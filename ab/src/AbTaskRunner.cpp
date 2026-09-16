// AbTaskRunner.cpp
// 2026-09-16 升级: 支持 sub-task list (串行跑多 cmd, 每 cmd 实时 emit sub_started/sub_finished)
#include "AbTaskRunner.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <cstdio>

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

    // 2026-09-16 v10: emit output 标记让实时 log 区立即有内容 (用户反馈"实时 log 没输出")
    //   - 写 desc 行 → onOutput 自动按 ▶▶▶ 标蓝色
    //   - 写 cmd 行 → 用户能 copy 调试
    //   - 子进程 stdout 也走 onReadyRead emit output, 这俩互不干扰
    if (!sub_desc.isEmpty()) {
        emit output(task_name_, QString("▶▶▶ [sub %1/%2] %3").arg(sub_idx_ + 1).arg(sub_total_).arg(sub_desc));
    } else {
        emit output(task_name_, QString("▶▶▶ [sub %1/%2]").arg(sub_idx_ + 1).arg(sub_total_));
    }
    emit output(task_name_, QString("  $ %1").arg(sub_cmd));
    emit output(task_name_, QString(""));  // 空行分割

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

        // 2026-09-16 v10: emit output 总结标记, 让实时 log 区能看到 sub-task 退出码 + 耗时
        if (exit_code == 0) {
            emit output(task_name_,
                QString("✓ [sub %1/%2] 完成 (rc=0 耗时 %.2fs)")
                    .arg(idx1).arg(sub_total_).arg(dt));
        } else {
            emit output(task_name_,
                QString("✗ [sub %1/%2] 失败 (rc=%3 耗时 %.2fs)")
                    .arg(idx1).arg(sub_total_).arg(exit_code).arg(dt));
        }

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
    // 2026-09-16 v9: 输出详细错误信息 (QProcess::errorString 包含具体失败原因, 比如
    //   "No such file or directory" / "Permission denied" / "Invalid argument" 等)
    std::string err_str = proc_ ? proc_->errorString().toStdString() : "(proc_ null)";
    const char* err_name = "?";
    switch (err) {
        case QProcess::FailedToStart: err_name = "FailedToStart"; break;
        case QProcess::Crashed:       err_name = "Crashed"; break;
        case QProcess::Timedout:      err_name = "Timedout"; break;
        case QProcess::WriteError:    err_name = "WriteError"; break;
        case QProcess::ReadError:     err_name = "ReadError"; break;
        case QProcess::UnknownError:  err_name = "UnknownError"; break;
    }
    // stderr + /tmp/ab-session.log 都写 (跟 log() 一样的 fallback 机制, 让用户本地也能 grep 到)
    fprintf(stderr,
        "[AbTaskRunner] QProcess 错误: %s (%d) msg=\"%s\" task=%s cmd=\"%.200s\"\n",
        err_name, static_cast<int>(err), err_str.c_str(),
        task_name_.toUtf8().constData(),
        (sub_idx_ < sub_cmds_.size() ? sub_cmds_[sub_idx_] : QString()).toUtf8().constData());
    fflush(stderr);
    {
        FILE* fp = fopen("/tmp/ab-session.log", "a");
        if (fp) {
            fprintf(fp,
                "[AbTaskRunner] QProcess 错误: %s (%d) msg=\"%s\" task=%s cmd=\"%.200s\"\n",
                err_name, static_cast<int>(err), err_str.c_str(),
                task_name_.toUtf8().constData(),
                (sub_idx_ < sub_cmds_.size() ? sub_cmds_[sub_idx_] : QString()).toUtf8().constData());
            fflush(fp);
            fclose(fp);
        }
    }
    // 2026-09-16 v10: emit output 总结让实时 log 区能看到 QProcess 错误 (用户反馈"实时 log 没输出")
    //   注意: 在 emit error 之前调, 否则 onProcFinished 之后 sub_total_ 清 0 走单 task 分支
    if (sub_total_ > 0) {
        int idx1 = sub_idx_ + 1;
        emit output(task_name_,
            QString("✗ [sub %1/%2] QProcess %3 (msg=\"%4\")")
                .arg(idx1).arg(sub_total_).arg(err_name).arg(QString::fromStdString(err_str)));
    }
    emit error(task_name_, static_cast<int>(err));
    // 2026-09-16: QProcess 错误 (启动失败 / 崩溃) 也清 sub-task 模式
    if (sub_total_ > 0) {
        sub_total_ = 0;
        sub_cmds_.clear();
        sub_descs_.clear();
    }
}

}  // namespace ab