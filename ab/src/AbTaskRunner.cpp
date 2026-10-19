// AbTaskRunner.cpp
// 2026-09-16 升级: 支持 sub-task list (串行跑多 cmd, 每 cmd 实时 emit sub_started/sub_finished)
#include "AbTaskRunner.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <cstdio>
#include <cstring>   // 2026-10-19: strerror
#include <cerrno>    // 2026-10-19: errno
#include <ctime>     // 2026-10-19: localtime_r / strftime
#include <unistd.h>  // 2026-10-15: isatty / STDERR_FILENO

namespace ab {

AbTaskRunner::AbTaskRunner(QObject* parent) : QObject(parent) {
    // 2026-10-15: 200ms 节流 emit sub_progress (避免日志洪水, GUI 也能跟上)
    progress_timer_.setSingleShot(true);
    progress_timer_.setInterval(200);
    connect(&progress_timer_, &QTimer::timeout, this, [this]() {
        if (sub_total_ > 0 && sub_idx_ < sub_total_) {
            emit sub_progress(task_name_, sub_idx_ + 1, sub_total_,
                              cur_compile_count_, cur_last_file_);
        }
    });
}

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
    last_single_cmd_ = cmd;  // 2026-09-18 加 — onProcError 时能报 cmd (跟 sub_cmds_ 对称)
    pending_.clear();
    error_ring_.clear();  // 2026-10-19 ab-error-log: 新 task 开始, 清空 ring (上次的输出别混入)
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
    error_ring_.clear();  // 2026-10-19 ab-error-log: 新 sub-task list 开始, 清空 ring
    QProcess* p = ensureProc();
    p->setWorkingDirectory(working_dir);
    p->setProcessChannelMode(QProcess::SeparateChannels);

    runNextSubTask();
}

// 启动当前 sub_idx_ 的 sub-task
void AbTaskRunner::runNextSubTask() {
    if (sub_idx_ >= sub_total_) {
        // 全部 sub-task 完成, emit finished signal 让 AbMainWindow.onFinished
        //   调 current_on_done_() → runNextInAuto() (auto 链下一步).
        // 修前 BUG: 这里直接 return 没 emit finished, 导致 auto 链跑完 build-v6
        //   (4/4 sub-task) 后卡住, [auto 2/3] run-v6-vk 永远不触发.
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_t_).count();
        QString done_name = task_name_;
        sub_total_ = 0;
        sub_cmds_.clear();
        sub_descs_.clear();
        emit finished(done_name, 0, elapsed);  // 修: 触发 callback → runNextInAuto
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

    // 2026-10-15: 重置"编译 N 个文件"实时进度
    cur_compile_count_ = 0;
    cur_last_file_.clear();
    // 立即 emit 一次, 让 UI 立即清掉上次的数字
    emit sub_progress(task_name_, sub_idx_ + 1, sub_total_, 0, QString());

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
        // 2026-10-19 ab-error-log: 失败时 dump 最近 N 行给 agent, 这里 push 到 ring
        pushErrorRing(line);
        emit output(task_name_, line);
        // 2026-10-15: 喂给"编译 N 个文件"检测器, 200ms 节流 emit
        bumpCompileProgress(line);

        // 2026-10-15 fix-tee-stderr: 把子进程原始输出回写到 stderr
        //   这样 `ab dev --auto-exit 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tee log | grep ...` 能拿到实时编译日志
        //   之前编译输出只在 QProcess → onOutput → panel_log_view_ 里, 不进 ab 的 stdout/stderr, sed/grep 拿不到
        //   只在 stderr 不是 tty (即用户 pipe 到 sed/tee/grep) 时才回写, 避免 GUI 模式下污染用户终端
        static const int kStderrIsTty = ::isatty(STDERR_FILENO);
        if (!kStderrIsTty) {
            QByteArray raw = line.toUtf8();
            std::fwrite(raw.constData(), 1, raw.size(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        }
    }
}

// 2026-10-15: 喂一行, 内部 regex 匹配, 命中 +1, 启动 200ms 节流 emit
//   覆盖:
//     scons:        "Compiling editor/gui/foo.cpp"  / "Compiling shared/lib/bar.cpp"
//     gcc/g++/clang: "g++ -c foo.cpp -o foo.o"      (cmd 行, 不常见, 但兼容)
//     ninja/meson:  类似 "Compiling xxx.cpp" / "[42/N] xxx.cpp"
void AbTaskRunner::bumpCompileProgress(const QString& line) {
    static const QRegularExpression re_scons(
        R"(Compiling\s+([^\s].*?\.(?:cpp|cc|cxx|c|c\+\+|m|mm|swift)))",
        QRegularExpression::CaseInsensitiveOption);
    // ninja: "[42/100] Compiling xxx.cpp" / "[42/100] CXX xxx.o"
    static const QRegularExpression re_ninja(
        R"(\[\s*\d+/\s*\d+\]\s+(?:Compiling|CXX|CC)\s+([^\s].*?))",
        QRegularExpression::CaseInsensitiveOption);
    // gcc/clang 直接调: "cc -c foo.cpp ..." — 用 first arg
    static const QRegularExpression re_gcc(
        R"((?:gcc|g\+\+|clang|clang\+\+|cc|c\+\+)\s+(?:-[^\s]+\s+)*([^\s-][^\s]*\.(?:cpp|cc|cxx|c))\b)",
        QRegularExpression::CaseInsensitiveOption);

    auto tryMatch = [&](const QRegularExpression& re) -> QString {
        auto m = re.match(line);
        if (m.hasMatch()) {
            QString f = m.captured(1).trimmed();
            // 去掉 ANSI 残留
            f.remove(QRegularExpression(R"(\x1B\[[\d;]*m)"));
            return f;
        }
        return QString();
    };
    QString file = tryMatch(re_scons);
    if (file.isEmpty()) file = tryMatch(re_ninja);
    if (file.isEmpty()) file = tryMatch(re_gcc);
    if (file.isEmpty()) return;

    cur_compile_count_++;
    // 只保留 basename, 太长切 60 字符
    int slash = file.lastIndexOf('/');
    QString short_name = (slash >= 0) ? file.mid(slash + 1) : file;
    if (short_name.size() > 60) short_name = short_name.left(57) + "...";
    cur_last_file_ = short_name;

    // 启动节流 timer
    if (!progress_timer_.isActive()) progress_timer_.start();
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
                QString("✓ [sub %1/%2] 完成 (rc=0 耗时 %3s)")
                    .arg(idx1).arg(sub_total_).arg(dt, 0, 'f', 2));
        } else {
            emit output(task_name_,
                QString("✗ [sub %1/%2] 失败 (rc=%3 耗时 %4s)")
                    .arg(idx1).arg(sub_total_).arg(exit_code).arg(dt, 0, 'f', 2));
        }

        if (exit_code != 0) {
            // 失败: emit sub_failed, 然后 emit 整 task finished (rc = exit_code)
            // 2026-10-19 ab-error-log: 失败时 dump 最近 200 行 ring buffer 到 /tmp/ab-error-<task>-<ts>.log
            //   + emit error_log_written signal (AbMainWindow 接到显示在 UI + agent 可读)
            QString err_log_path = dumpErrorRingToFile(exit_code);
            if (!err_log_path.isEmpty()) {
                emit output(task_name_, QString("⚠ [sub %1/%2] rc=%3, 最近 %4 行输出写到 %5")
                    .arg(idx1).arg(sub_total_).arg(exit_code)
                    .arg(error_ring_.size()).arg(err_log_path));
                emit error_log_written(task_name_, err_log_path, exit_code);
            }
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
    // 2026-10-19 ab-error-log: 单 task 失败也 dump (跟 sub-task 模式一致)
    if (exit_code != 0) {
        QString err_log_path = dumpErrorRingToFile(exit_code);
        if (!err_log_path.isEmpty()) {
            emit output(task_name_, QString("⚠ [task] rc=%1, 最近 %2 行输出写到 %3")
                .arg(exit_code).arg(error_ring_.size()).arg(err_log_path));
            emit error_log_written(task_name_, err_log_path, exit_code);
        }
    }
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
    // 2026-09-18: 报 cmd 时检查 sub_cmds_ (sub-task 模式) 和 last_single_cmd_ (单 cmd 模式)
    QString report_cmd;
    if (sub_idx_ < sub_cmds_.size()) {
        report_cmd = sub_cmds_[sub_idx_];
    } else {
        report_cmd = last_single_cmd_;
    }
    fprintf(stderr,
        "[AbTaskRunner] QProcess 错误: %s (%d) msg=\"%s\" task=%s cmd=\"%.200s\"\n",
        err_name, static_cast<int>(err), err_str.c_str(),
        task_name_.toUtf8().constData(),
        report_cmd.toUtf8().constData());
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

// 2026-10-19 ab-error-log: ring buffer 加行 (固定 kErrorRingMaxLines 大小,
//   满了从头覆盖, 失败时 dump 出去). 包括 stdout + stderr 全部行.
void AbTaskRunner::pushErrorRing(const QString& line) {
    error_ring_.append(line);
    if (error_ring_.size() > kErrorRingMaxLines) {
        error_ring_.removeFirst();  // 循环覆盖最旧的行
    }
}

// 2026-10-19 ab-error-log: 失败时 dump ring 到 /tmp/ab-error-<task>-<ts>.log
//   文件包含:
//     - 头: task_name / exit_code / sub_idx (if sub-task mode) / 时间戳 / 工作目录
//     - 体: error_ring_ 里最近 kErrorRingMaxLines 行 (旧→新)
//   返回写入的文件路径 (失败时返回空).
QString AbTaskRunner::dumpErrorRingToFile(int exit_code) {
    if (error_ring_.isEmpty()) {
        // 没收集到任何输出 (e.g. cmd 没跑起来) — 仍然写文件, 至少有 header 上下文
    }
    // 用时间戳保证唯一, agent 可以 grep /tmp/ab-error-*.log 看所有历史失败
    auto now_t = std::chrono::system_clock::now();
    auto now_tt = std::chrono::system_clock::to_time_t(now_t);
    struct tm tm_buf;
    localtime_r(&now_tt, &tm_buf);
    char ts_buf[32];
    strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm_buf);

    // task_name 替换非法字符 (/) 保证合法文件名
    QString safe_task = task_name_;
    safe_task.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
    QString path = QString("/tmp/ab-error-%1-%2.log").arg(safe_task).arg(ts_buf);

    FILE* fp = fopen(path.toUtf8().constData(), "w");
    if (!fp) {
        fprintf(stderr, "[AbTaskRunner] dumpErrorRingToFile: fopen(%s) failed: %s\n",
            path.toUtf8().constData(), std::strerror(errno));
        fflush(stderr);
        return QString();
    }
    // 头: 元信息 (agent grep task / exit_code 快速定位)
    fprintf(fp, "# ab error dump\n");
    fprintf(fp, "task_name=%s\n", task_name_.toUtf8().constData());
    fprintf(fp, "exit_code=%d\n", exit_code);
    fprintf(fp, "sub_mode=%s\n", sub_total_ > 0 ? "yes" : "no");
    if (sub_total_ > 0) {
        fprintf(fp, "sub_failed_idx=%d\n", sub_idx_ + 1);
        fprintf(fp, "sub_total=%d\n", sub_total_);
        if (sub_idx_ < sub_cmds_.size()) {
            fprintf(fp, "failed_cmd=%s\n", sub_cmds_[sub_idx_].toUtf8().constData());
        }
        if (sub_idx_ < sub_descs_.size()) {
            fprintf(fp, "failed_desc=%s\n", sub_descs_[sub_idx_].toUtf8().constData());
        }
    } else {
        fprintf(fp, "single_cmd=%s\n", last_single_cmd_.toUtf8().constData());
    }
    fprintf(fp, "working_dir=%s\n", working_dir_.toUtf8().constData());
    fprintf(fp, "ring_lines=%d (last %d max)\n", (int)error_ring_.size(), kErrorRingMaxLines);
    fprintf(fp, "# ---- ring buffer (oldest -> newest, last %d lines) ----\n", (int)error_ring_.size());
    // 写所有 ring 行 (append 顺序就是旧→新, 已经按时间顺序)
    for (const QString& line : error_ring_) {
        QByteArray utf8 = line.toUtf8();
        fwrite(utf8.constData(), 1, utf8.size(), fp);
        fputc('\n', fp);
    }
    fflush(fp);
    fclose(fp);
    return path;
}

}  // namespace ab