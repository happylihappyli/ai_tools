#ifndef AB_LOG_DOCK_H
#define AB_LOG_DOCK_H
// SPDX-License-Identifier: MIT
//
// AbLogDock — 彩色日志面板 (QPlainTextEdit + 自动滚动开关 + 清空 + 复制错误)
#include <QDockWidget>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

namespace ab {

class AbLogDock : public QDockWidget {
    Q_OBJECT
public:
    explicit AbLogDock(QWidget* parent = nullptr);

    // 写一行 (level: ok/err/warn/info/task/debug)
    // 2026-09-09: err/warn 染色 (跟 build_poc_a_stage3_gui.py ANSI 一致, 用户偏好)
    void log(const QString& level, const QString& msg);

    // 清空
    void clearLog();

public slots:
    void onAutoScrollToggled(bool checked);
    void onClearClicked();
    void onCopyErrorsClicked();   // 2026-09-09: 复制 error/warn/traceback 行
    void onCopyAllClicked();      // 2026-09-09: 复制全部 log
    void onCopyCmdClicked();      // 2026-09-09: 复制 scons/build 命令 (跟 GUI 选中的 task 有关)

private:
    QPlainTextEdit* edit_         = nullptr;
    QCheckBox*      auto_chk_     = nullptr;
    QPushButton*    clear_btn_    = nullptr;
    QPushButton*    copy_err_btn_ = nullptr;   // 2026-09-09
    QPushButton*    copy_all_btn_ = nullptr;   // 2026-09-09
    QLabel*         status_lbl_   = nullptr;   // 2026-09-09: 黄底状态栏
};

}  // namespace ab

#endif
