// AbTaskRunner.cpp
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

void AbTaskRunner::run(const QString& task_name, const QString& cmd,
                       const QString& working_dir, const QStringList& /*env*/) {
    if (isRunning()) {
        qWarning() << "[AbTaskRunner] already running, ignoring";
        return;
    }
    task_name_ = task_name;
    pending_.clear();
    if (!proc_) {
        proc_ = new QProcess(this);
        // Qt 5 的 readyRead* 有 QPrivateSignal 参数, 用 lambda 包裹
        connect(proc_, &QProcess::readyReadStandardOutput,
                this, [this]() { onReadyRead(); });
        connect(proc_, &QProcess::readyReadStandardError,
                this, [this]() { onReadyRead(); });
        connect(proc_, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, &AbTaskRunner::onProcFinished);
        connect(proc_, &QProcess::errorOccurred, this, &AbTaskRunner::onProcError);
    }
    proc_->setWorkingDirectory(working_dir);
    proc_->setProcessChannelMode(QProcess::SeparateChannels);

    // shell 模式: /bin/sh -c "<cmd>"
    QStringList args;
    args << "-c" << cmd;
    start_t_ = std::chrono::steady_clock::now();
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
    if (!pending_.isEmpty()) {
        emit output(task_name_, pending_);
        pending_.clear();
    }
    auto end_t = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_t - start_t_).count();
    emit finished(task_name_, exit_code, elapsed);
}

void AbTaskRunner::onProcError(QProcess::ProcessError err) {
    emit error(task_name_, static_cast<int>(err));
}

}  // namespace ab
