#ifndef AB_TASK_RUNNER_H
#define AB_TASK_RUNNER_H
// SPDX-License-Identifier: MIT
//
// AbTaskRunner — QProcess 包装, 跑 task 命令 (shell 模式)
//
// 特性:
// - 异步, 不阻塞 GUI 线程
// - stdout/stderr 实时回调 (on_output)
// - 退出回调 (on_finished: exit_code, elapsed)
// - 强制终止 (stop)
// - 2026-09-16: 支持 sub-task list (串行跑多 cmd, 每 cmd 实时 emit sub_started/sub_finished)

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <functional>
#include <chrono>

namespace ab {

class AbTaskRunner : public QObject {
    Q_OBJECT
public:
    using OutputCb    = std::function<void(const QString& /*line*/)>;
    using FinishedCb  = std::function<void(int /*exit_code*/, double /*elapsed_s*/)>;
    using ErrorCb     = std::function<void(QProcess::ProcessError)>;

    explicit AbTaskRunner(QObject* parent = nullptr);
    ~AbTaskRunner() override;

    // 跑单 task (用 shell 解析, 兼容 && 链) - 兼容老 API
    void run(const QString& task_name,
             const QString& cmd,
             const QString& working_dir,
             const QStringList& env = {});

    // 2026-09-16 新增: 跑 sub-task list (串行, 每 sub-task 实时 emit 信号)
    //   sub_descs 可以为空 list (跟 sub_cmds 等长), UI 优先用 desc 显示
    //   任意 sub-task 失败 (rc != 0) 立即停止, task_finished emit 那个 rc
    void runSubTasks(const QString& task_name,
                     const QStringList& sub_cmds,
                     const QStringList& sub_descs,
                     const QString& working_dir,
                     const QStringList& env = {});

    bool isRunning() const;
    void stop();

signals:
    void output(const QString& task_name, const QString& line);
    void finished(const QString& task_name, int exit_code, double elapsed);
    void error(const QString& task_name, int err);
    // 2026-09-16 新增: sub-task 事件 (跟 ac 的 stdout 对齐, GUI 实时更新进度)
    //   sub_started: 新的 sub-task 开始 (idx 从 1 开始, total 是 sub-task 总数)
    //     desc 是中文说明 (可以为空), cmd 是完整命令
    //   sub_finished: 当前 sub-task 完成 (idx, total, rc, dt_sec)
    //   sub_failed: 任意 sub-task 失败, task 终止 (idx, total)
    void sub_started(const QString& task_name, int idx, int total,
                     const QString& desc, const QString& cmd);
    void sub_finished(const QString& task_name, int idx, int total,
                      int rc, double dt_sec);
    void sub_failed(const QString& task_name, int idx, int total);

private slots:
    void onReadyRead();
    void onProcFinished(int exit_code, QProcess::ExitStatus status);
    void onProcError(QProcess::ProcessError err);
    void runNextSubTask();  // 串行跑下一个 sub-task

private:
    QProcess* ensureProc();  // 共享: 准备 QProcess (lazy init + signal connect)
    QProcess* proc_ = nullptr;
    QString   task_name_;
    std::chrono::steady_clock::time_point start_t_;
    QString   pending_;  // 行缓冲

    // 2026-09-16 新增: sub-task 串行跑状态
    QStringList   sub_cmds_;
    QStringList   sub_descs_;
    int           sub_idx_   = 0;   // 当前跑的 idx (0 = 没在跑)
    int           sub_total_ = 0;
    std::chrono::steady_clock::time_point sub_t0_;
    QString       working_dir_;
    QStringList   env_;
};

}  // namespace ab

#endif
