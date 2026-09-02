#ifndef AB_MAIN_WINDOW_H
#define AB_MAIN_WINDOW_H
// SPDX-License-Identifier: MIT
//
// AbMainWindow — ab 主窗口
//
// 按 AbConfig.ui 段动态生成:
// - 菜单 (menus[])
// - 工具栏 (toolbar[])
// - 主按钮行 (buttons[])
// - run_after_build 启动按钮
// - 日志 Dock

#include <QMainWindow>
#include <QString>
#include <QHash>
#include "AbConfig.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QProgressBar;
class QPushButton;
class QStatusBar;
class QAction;
class QMenu;
class QToolBar;
class QKeySequence;

namespace ab {

class AbTaskRunner;
class AbLogDock;

class AbMainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AbMainWindow(const AbConfig& cfg, QWidget* parent = nullptr);
    ~AbMainWindow() override;

    // 重新加载配置
    void reloadConfig();

public slots:
    void onOutput(const QString& task_name, const QString& line);
    void onFinished(const QString& task_name, int exit_code, double elapsed);
    void onError(const QString& task_name, int err);

    // 通用槽: 按钮/菜单触发
    void onRunSelectedTask();
    void onRunAuto();
    void onStop();
    void onRunCloud();
    void onBuildAndRun();
    void onToggleTheme();
    void onToggleLogDock(bool checked);
    void onAbout();
    void onQuit();

    // 通用槽: 按 id 触发 (按钮/工具栏/菜单 共用)
    void onActionTriggered();

private:
    void buildFromConfig();
    void buildMenus();
    void buildToolbar();
    void buildMainButtons();
    void buildTaskList();
    void buildStatusBar();
    void wireRunner();
    void enableRunCloudButton(bool en);
    QString findRunBinary() const;
    // 找系统工具的绝对路径 (which-like), 找不到返回 ""
    // 搜索顺序: PATH -> ~/.local/bin -> /usr/local/bin -> /usr/bin
    QString findTool(const QString& name) const;
    void runTaskByName(const QString& name, std::function<void()> on_done = nullptr);  // 通用跑 task
    void runCmd(const QString& cmd, const QString& task_name = "<cmd>");
    void log(const QString& level, const QString& msg);
    QString resolveTaskCmd(const QString& task_name) const;
    void runAutoQueue();
    void runNextInAuto();
    QAction* createActionForButton(const AbButtonDef& b, QWidget* parent);

    AbConfig cfg_;
    AbTaskRunner* runner_ = nullptr;
    AbLogDock*    log_dock_ = nullptr;

    // UI
    QTreeWidget*   task_list_  = nullptr;
    QLabel*        prog_label_ = nullptr;
    QProgressBar*  prog_bar_   = nullptr;
    QStatusBar*    statusbar_  = nullptr;
    QLabel*        sb_left_    = nullptr;
    QLabel*        sb_right_   = nullptr;

    // 状态
    bool   current_aborted_ = false;
    QStringList auto_queue_;
    int    auto_index_ = 0;
    QString current_task_;
    std::function<void()> current_on_done_;
    QString cloud_binary_;  // 找到的 cloud_main 路径
    QString ac_binary_;     // 找到的 ac 绝对路径 (启动时探测, 解决桌面 GUI PATH 不带 ~/.local/bin 问题)
    QString spd_say_binary_;
    QString bak_binary_;

    // 动作注册 (id → QAction*)
    QHash<QString, QAction*> actions_;
    QHash<QString, QPushButton*> buttons_;
};

}  // namespace ab

#endif
