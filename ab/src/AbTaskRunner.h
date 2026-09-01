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

    // 跑 task (用 shell 解析, 兼容 && 链)
    void run(const QString& task_name,
             const QString& cmd,
             const QString& working_dir,
             const QStringList& env = {});

    bool isRunning() const;
    void stop();

signals:
    void output(const QString& task_name, const QString& line);
    void finished(const QString& task_name, int exit_code, double elapsed);
    void error(const QString& task_name, int err);

private slots:
    void onReadyRead();
    void onProcFinished(int exit_code, QProcess::ExitStatus status);
    void onProcError(QProcess::ProcessError err);

private:
    QProcess* proc_ = nullptr;
    QString   task_name_;
    std::chrono::steady_clock::time_point start_t_;
    QString   pending_;  // 行缓冲
};

}  // namespace ab

#endif
