// AbLogDock.cpp
#include "AbLogDock.h"
#include <QDateTime>
#include <QFont>
#include <QScrollBar>

namespace ab {

AbLogDock::AbLogDock(QWidget* parent) : QDockWidget("操作日志", parent) {
    QWidget* w = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(w);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* bar = new QHBoxLayout();
    auto_chk_ = new QCheckBox("自动滚动: 开", this);
    auto_chk_->setChecked(true);
    connect(auto_chk_, &QCheckBox::toggled, this, &AbLogDock::onAutoScrollToggled);
    bar->addWidget(auto_chk_);
    bar->addStretch(1);
    clear_btn_ = new QPushButton("清空", this);
    connect(clear_btn_, &QPushButton::clicked, this, &AbLogDock::onClearClicked);
    bar->addWidget(clear_btn_);
    layout->addLayout(bar);

    edit_ = new QPlainTextEdit(this);
    edit_->setReadOnly(true);
    edit_->setMaximumBlockCount(5000);
    QFont f("monospace");
    f.setPointSize(10);
    edit_->setFont(f);
    layout->addWidget(edit_, 1);

    setWidget(w);
}

void AbLogDock::log(const QString& level, const QString& msg) {
    QString prefix = "•";
    if (level == "ok")   prefix = "✓";
    if (level == "err")  prefix = "✗";
    if (level == "warn") prefix = "⚠";
    if (level == "task") prefix = "▶";
    if (level == "debug")prefix = "›";
    QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    edit_->appendPlainText(QString("[%1] %2 %3").arg(ts, prefix, msg));
    if (auto_chk_->isChecked()) {
        QScrollBar* sb = edit_->verticalScrollBar();
        sb->setValue(sb->maximum());
    }
}

void AbLogDock::clearLog() {
    edit_->clear();
}

void AbLogDock::onAutoScrollToggled(bool checked) {
    auto_chk_->setText(QString("自动滚动: %1").arg(checked ? "开" : "关"));
}

void AbLogDock::onClearClicked() {
    clearLog();
}

}  // namespace ab
