#ifndef AB_LOG_DOCK_H
#define AB_LOG_DOCK_H
// SPDX-License-Identifier: MIT
//
// AbLogDock — 彩色日志面板 (QPlainTextEdit + 自动滚动开关 + 清空)

#include <QDockWidget>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace ab {

class AbLogDock : public QDockWidget {
    Q_OBJECT
public:
    explicit AbLogDock(QWidget* parent = nullptr);

    // 写一行 (level: ok/err/warn/info/task/debug)
    void log(const QString& level, const QString& msg);

    // 清空
    void clearLog();

public slots:
    void onAutoScrollToggled(bool checked);
    void onClearClicked();

private:
    QPlainTextEdit* edit_      = nullptr;
    QCheckBox*      auto_chk_  = nullptr;
    QPushButton*    clear_btn_ = nullptr;
};

}  // namespace ab

#endif
