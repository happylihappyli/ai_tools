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

// 2026-10-15: 解析 ANSI 颜色码 + 剥 ESC 序列, 染到 QPlainTextEdit
//   scons / 编译工具会用 ESC[xxm 染色, QPlainTextEdit 默认不解析, 之前直接显示为 "[34m..."
//   拆 ESC[...m 段, SGR 参数翻 hex 颜色, 输出成 <span style="color:..">
//   符号标记 (▶▶▶ / ✓ / ✗ / 🚀 / ===) 走强制颜色
void appendColoredLine(QPlainTextEdit* view, const QString& line);

}  // namespace ab

#endif
