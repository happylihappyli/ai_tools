// AbLogDock.cpp
#include "AbLogDock.h"
#include <QDateTime>
#include <QFont>
#include <QScrollBar>
#include <QClipboard>
#include <QGuiApplication>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QRegularExpression>
#include <QProcess>

namespace ab {

AbLogDock::AbLogDock(QWidget* parent) : QDockWidget("操作日志", parent) {
    QWidget* w = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(w);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // ===== 顶部工具栏 =====
    QHBoxLayout* bar = new QHBoxLayout();
    auto_chk_ = new QCheckBox("自动滚动: 开", this);
    auto_chk_->setChecked(true);
    connect(auto_chk_, &QCheckBox::toggled, this, &AbLogDock::onAutoScrollToggled);
    bar->addWidget(auto_chk_);

    // 2026-09-09: 复制错误 (跟 build_poc_a_stage3_gui.py 同样)
    copy_err_btn_ = new QPushButton("📋 复制错误", this);
    copy_err_btn_->setToolTip("复制过滤后的 error/warn/traceback 行 (识别 FATAL, undefined reference 等关键字, 加 [E]/[W] 前缀)");
    connect(copy_err_btn_, &QPushButton::clicked, this, &AbLogDock::onCopyErrorsClicked);
    bar->addWidget(copy_err_btn_);

    // 2026-09-09: 复制全部
    copy_all_btn_ = new QPushButton("📄 复制全部", this);
    copy_all_btn_->setToolTip("复制全部 log 内容 (含时间戳)");
    connect(copy_all_btn_, &QPushButton::clicked, this, &AbLogDock::onCopyAllClicked);
    bar->addWidget(copy_all_btn_);

    bar->addStretch(1);
    clear_btn_ = new QPushButton("清空", this);
    connect(clear_btn_, &QPushButton::clicked, this, &AbLogDock::onClearClicked);
    bar->addWidget(clear_btn_);
    layout->addLayout(bar);

    // ===== log 区 =====
    edit_ = new QPlainTextEdit(this);
    edit_->setReadOnly(true);
    edit_->setMaximumBlockCount(5000);
    QFont f("monospace");
    f.setPointSize(10);
    edit_->setFont(f);
    layout->addWidget(edit_, 1);

    // ===== 状态栏 (黄底) =====
    // 2026-09-09: 复制结果回显 (跟 build_poc_a_stage3_gui.py 同样的 #ffff88 黄底)
    status_lbl_ = new QLabel("就绪", this);
    status_lbl_->setStyleSheet(
        "QLabel { background-color: #ffff88; color: #202020; padding: 2px 6px;"
        " border-radius: 2px; font-weight: bold; }");
    status_lbl_->setMaximumHeight(22);
    layout->addWidget(status_lbl_);

    setWidget(w);
}

// 2026-09-09: 染色 — err 红色, warn 黄色, ok 绿色, info/task 蓝色, debug 灰色
// 跟 build_poc_a_stage3_gui.py 的 ANSI 颜色对应
void AbLogDock::log(const QString& level, const QString& msg) {
    QString prefix = "•";
    QColor color = QColor("#cccccc");  // 默认灰
    if (level == "ok")    { prefix = "✓"; color = QColor("#22cc22"); }  // 绿
    if (level == "err")   { prefix = "✗"; color = QColor("#ff4444"); }  // 红
    if (level == "warn")  { prefix = "⚠"; color = QColor("#ffcc00"); }  // 黄
    if (level == "task")  { prefix = "▶"; color = QColor("#4488ff"); }  // 蓝
    if (level == "info")  { prefix = "ℹ"; color = QColor("#88ccff"); }  // 浅蓝
    if (level == "debug") { prefix = "›"; color = QColor("#888888"); }  // 灰
    QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString line = QString("[%1] %2 %3").arg(ts, prefix, msg);

    // 2026-09-09: 用 QTextCharFormat 染色 (err 行整行红色)
    QTextCharFormat fmt;
    fmt.setForeground(color);
    QTextCursor cursor = edit_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(line + "\n", fmt);

    if (auto_chk_->isChecked()) {
        QScrollBar* sb = edit_->verticalScrollBar();
        sb->setValue(sb->maximum());
    }
}

void AbLogDock::clearLog() {
    edit_->clear();
    if (status_lbl_) status_lbl_->setText("已清空");
}

void AbLogDock::onAutoScrollToggled(bool checked) {
    auto_chk_->setText(QString("自动滚动: %1").arg(checked ? "开" : "关"));
}

void AbLogDock::onClearClicked() {
    clearLog();
}

// 2026-09-09: 复制 error/warn/traceback 行
// 跟 build_poc_a_stage3_gui.py 的 copy_errors_to_clipboard() 完全一致
// 关键字: FATAL, error:, Error, undefined reference, Traceback, FAILED,
//         warning, ld returned, link error, no RenderingDevice, ERR_*
void AbLogDock::onCopyErrorsClicked() {
    QStringList lines = edit_->toPlainText().split('\n');
    QStringList filtered;
    int err_count = 0, warn_count = 0;

    // 16 类关键字 (跟 build_poc_a_stage3_gui.py 的 _ERR_KEYWORDS 一致)
    static const QStringList err_keywords = {
        "error:", "Error:", "FATAL", "fatal:",
        "undefined reference", "ld returned",
        "Traceback", "Traceback (most recent call last):",
        "Aborted", "Segmentation fault", "SIGSEGV",
        "no RenderingDevice", "PushStat", "push_stat_ptr",
        "FAILED", "FAIL:", "ERR_"
    };
    static const QStringList warn_keywords = {
        "warning:", "Warning:", "WARN_"
    };

    for (const QString& line : lines) {
        bool is_err = false, is_warn = false;
        for (const auto& kw : err_keywords) {
            if (line.contains(kw, Qt::CaseInsensitive)) { is_err = true; break; }
        }
        if (!is_err) {
            for (const auto& kw : warn_keywords) {
                if (line.contains(kw, Qt::CaseInsensitive)) { is_warn = true; break; }
            }
        }
        if (is_err)   { filtered << "[E] " + line; err_count++; }
        else if (is_warn) { filtered << "[W] " + line; warn_count++; }
    }

    if (filtered.isEmpty()) {
        if (status_lbl_) status_lbl_->setText("✓ 无错误, 全部干净");
        return;
    }

    QGuiApplication::clipboard()->setText(filtered.join('\n'));
    QString msg = QString("✓ 已复制 %1 个 error / %2 个 warning (共 %3 行) 到剪贴板")
                      .arg(err_count).arg(warn_count).arg(filtered.size());
    if (status_lbl_) status_lbl_->setText(msg);

    // TTS 播报 (用户偏好, 跟 build_poc_a_stage3_gui.py 一样)
    QProcess::startDetached("spd-say",
        QStringList() << QString("已复制 %1 个错误, %2 个警告").arg(err_count).arg(warn_count));
}

// 2026-09-09: 复制全部 log
void AbLogDock::onCopyAllClicked() {
    QString text = edit_->toPlainText();
    if (text.isEmpty()) {
        if (status_lbl_) status_lbl_->setText("log 区为空, 无内容可复制");
        return;
    }
    int line_count = text.count('\n');
    QGuiApplication::clipboard()->setText(text);
    QString msg = QString("✓ 已复制全部 log (%1 行) 到剪贴板").arg(line_count);
    if (status_lbl_) status_lbl_->setText(msg);
}

// 2026-09-09: 复制 scons/build 命令 — 占位 (跨 dock 拿不到当前选中 task, 只提示)
void AbLogDock::onCopyCmdClicked() {
    if (status_lbl_) status_lbl_->setText("复制命令功能见 build_poc_a_stage3_gui.py (POC-A Stage 3 链)");
}

}  // namespace ab
