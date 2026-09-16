#ifndef AB_TASK_RUNNER_WINDOW_H
#define AB_TASK_RUNNER_WINDOW_H
// SPDX-License-Identifier: MIT
//
// TaskRunnerWindow — 独立任务运行器窗口 (仿 ac_task_runner_gui.py 2026-09-16)
//
// 设计:
//   - 独立 QMainWindow, 通过 ab 工具栏"📊 任务运行器"按钮弹出
//   - 接收 AbTaskRunner* (不创建自己的进程, 复用 ab 的 runner)
//   - 接 signals: output / sub_started / sub_finished / sub_failed / finished / error
//   - 界面:
//       顶部: 任务名 + 状态 badge + elapsed 计时
//       进度条: N / total
//       ⚡ 当前执行 区域: idx + desc + 实时计时器
//       中间 split (左: sub-task 列表, 右: 实时 log)
//       底部: 选中 sub-task 详情 (完整 cmd + log 路径 + 打开 + 复制)
//       底部按钮行: ⏹ Stop / 📋 复制 log / 📂 打开 log 目录 / 🚀 启动 cloud_main
//   - 同时也接收 task_name (跟 sub-task list 一起, 用于预填)
//
// 2026-09-16: 新增, Step 6.

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QList>

class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QListWidget;
class QPushButton;
class QListWidgetItem;
class QSplitter;
class QTimer;
class QFrame;
class QGroupBox;

namespace ab {

class AbTaskRunner;

class TaskRunnerWindow : public QMainWindow {
    Q_OBJECT
public:
    // 给 ab 调用: 接受 AbTaskRunner, 注册 signal 监听
    TaskRunnerWindow(AbTaskRunner* runner,
                    const QString& task_name,
                    const QStringList& sub_cmds,
                    const QStringList& sub_descs,
                    QWidget* parent = nullptr);
    ~TaskRunnerWindow() override;

    // 切换 task (重新打开窗口时调用, 重置 UI + 预填)
    void switchTask(const QString& task_name,
                    const QStringList& sub_cmds,
                    const QStringList& sub_descs);

signals:
    void requestRunTask(const QString& task_name);   // 暂时没用, 留作扩展

private slots:
    // 接 AbTaskRunner signals
    void onOutput(const QString& task_name, const QString& line);
    void onSubStarted(const QString& task_name, int idx, int total,
                      const QString& desc, const QString& cmd);
    void onSubFinished(const QString& task_name, int idx, int total,
                       int rc, double dt_sec);
    void onSubFailed(const QString& task_name, int idx, int total);
    void onTaskFinished(const QString& task_name, int exit_code, double elapsed);
    void onError(const QString& task_name, int err);

    // UI 槽
    void onStopClicked();
    void onCopyLog();
    void onOpenLogDir();
    void onOpenSubLog();
    void onCopySubCmd();
    void onLaunchCloudMain();
    void onSubItemClicked();           // 点击 sub-task 列表项 → 刷新详情区
    void onUpdateElapsed();            // 1s 周期刷新顶部 elapsed
    void onUpdateCurrentDt();          // 0.5s 周期刷新当前 sub-task 计时器

private:
    void buildUi();
    void wireRunner();
    void resetUi(int total);
    QString logDirForCurrentTask() const;

    // --- 控件 ---
    QFrame*      header_         = nullptr;   // 顶部条
    QLabel*      lbl_title_      = nullptr;   // task 名称 (左)
    QLabel*      lbl_status_     = nullptr;   // status badge (右, 颜色变化)
    QLabel*      lbl_elapsed_    = nullptr;   // ⏱ 00:00
    QProgressBar*progress_       = nullptr;
    QFrame*      current_box_    = nullptr;   // ⚡ 当前执行 框
    QLabel*      lbl_current_title_ = nullptr;
    QLabel*      lbl_current_idx_   = nullptr;
    QLabel*      lbl_current_dt_    = nullptr;
    QPlainTextEdit* txt_current_cmd_= nullptr;
    QSplitter*   splitter_       = nullptr;   // 中间 split
    QListWidget* sub_list_        = nullptr;   // 左: sub-task 列表
    QLabel*      lbl_log_summary_ = nullptr;
    QPlainTextEdit* log_view_     = nullptr;   // 右: 实时 log
    QGroupBox*   detail_box_     = nullptr;
    QLabel*      lbl_detail_meta_ = nullptr;
    QPushButton* btn_open_sub_log_= nullptr;
    QPushButton* btn_copy_sub_cmd_= nullptr;
    QPlainTextEdit* txt_detail_cmd_= nullptr;
    QPushButton* btn_stop_       = nullptr;
    QPushButton* btn_copy_log_   = nullptr;
    QPushButton* btn_open_dir_   = nullptr;
    QPushButton* btn_launch_cloud_= nullptr;

    QTimer* elapsed_timer_       = nullptr;   // 1s 周期
    QTimer* current_dt_timer_    = nullptr;   // 0.5s 周期

    // --- 数据 ---
    AbTaskRunner* runner_         = nullptr;
    QString       task_name_;
    QStringList   sub_cmds_;
    QStringList   sub_descs_;
    int           total_sub_      = 0;

    // sub-task 状态: idx → {rc, dt, cmd, desc, log_path, status}
    struct SubState {
        int     rc      = 0;
        double  dt      = 0;
        QString cmd;
        QString desc;
        QString log_path;
        QString status;  // "pending" / "running" / "ok" / "fail"
    };
    QHash<int, SubState> sub_states_;

    // 选中 sub-task idx (用于详情区)
    int selected_sub_idx_ = 0;

    // task 开始时间 (用于 elapsed)
    qint64 task_start_ms_ = 0;
    // 当前 sub-task 开始时间
    qint64 current_sub_start_ms_ = 0;
};

}  // namespace ab

#endif