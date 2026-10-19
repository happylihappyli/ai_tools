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

namespace {
// SGR 参数 -> #RRGGBB (前景色). 0/未识别 = 默认
QString sgrToColor(const QStringList& params) {
    int r = -1, g = -1, b = -1;
    bool bold = false;
    for (const QString& p : params) {
        bool ok = false;
        int v = p.toInt(&ok, 10);
        if (!ok) continue;
        if (v == 0) { r = g = b = -1; bold = false; }
        else if (v == 1) bold = true;
        else if (v >= 30 && v <= 37) {
            // 30 黑 31 红 32 绿 33 黄 34 蓝 35 品红 36 青 37 白
            static const int k[8][3] = {
                {0x10, 0x10, 0x10}, {0xcc, 0x44, 0x44}, {0x44, 0xcc, 0x44},
                {0xcc, 0x99, 0x33}, {0x44, 0x88, 0xcc}, {0xaa, 0x66, 0xaa},
                {0x44, 0xaa, 0xaa}, {0xcc, 0xcc, 0xcc},
            };
            int i = v - 30;
            r = k[i][0]; g = k[i][1]; b = k[i][2];
        } else if (v >= 90 && v <= 97) {
            // 90 亮黑 91 亮红 ... 97 亮白
            static const int k[8][3] = {
                {0x66, 0x66, 0x66}, {0xff, 0x55, 0x55}, {0x55, 0xff, 0x55},
                {0xff, 0xcc, 0x55}, {0x55, 0xaa, 0xff}, {0xff, 0x88, 0xff},
                {0x55, 0xff, 0xff}, {0xff, 0xff, 0xff},
            };
            int i = v - 90;
            r = k[i][0]; g = k[i][1]; b = k[i][2];
        }
    }
    Q_UNUSED(bold);
    if (r < 0) return QString();
    return QString("#%1%2%3")
        .arg(r, 2, 16, QChar('0'))
        .arg(g, 2, 16, QChar('0'))
        .arg(b, 2, 16, QChar('0'));
}

}  // namespace

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

// 2026-10-15 / 2026-10-19: 解析 ANSI 颜色码 + 剥 ESC 序列, 染到 QPlainTextEdit
//   scons / 编译工具会用 ESC[xxm 染色, QPlainTextEdit 默认不解析, 之前直接显示为 "[34m..."
//   拆 ESC[...m 段, SGR 参数翻 QColor, 用 QTextCursor 插入带 QTextCharFormat 的文本
//   符号标记 (▶▶▶ / ✓ / ✗ / 🚀 / ===) 走强制颜色
//
//   v2 (2026-10-19):
//     - 改成手动扫 ESC (不用 QRegularExpression, 避免 PCRE 在 0x1B 边界上的边角问题)
//     - 改进 SGR 颜色映射: 接收 "1;34" 这种多参数; ESC[0m 和 ESC[m 视作重置
//     - 不识别的 ESC 序列一律剥掉, log 区不再残留 [xxm 之类
//     - 修 fast path bug: symbolColorHex 非空时, 即使只有一段也走 cursor 路径
//       (否则带符号的行不会上色)
void appendColoredLine(QPlainTextEdit* view, const QString& line) {
    if (!view) return;

    // 符号强制颜色 (▶▶▶ / ✓ / ✗ / 🚀 / ===) - 不管 ANSI, 始终上色
    QString symbolColorHex;
    if (line.startsWith("▶▶▶")) symbolColorHex = "#88ccff";
    else if (line.startsWith("✓")) symbolColorHex = "#88ff88";
    else if (line.startsWith("✗")) symbolColorHex = "#ff8888";
    else if (line.startsWith("🚀") || line.contains("===")) symbolColorHex = "#ffaa44";

    struct Seg { QString colorHex; QString text; };
    QList<Seg> segs;
    QString curText;
    QString currentColorHex = symbolColorHex;

    // 手动扫描 ESC 序列. ESC = 0x1B, 序列以字母结尾 (m/K/H/J 等). 我们只处理 SGR 'm'.
    // 其它 CSI 序列 (e.g. "[2J") 也整体剥掉, 避免残留.
    const QChar ESC = QChar(0x1B);
    int n_len = line.size();
    int i = 0;
    while (i < n_len) {
        QChar c = line.at(i);
        if (c == ESC && i + 1 < n_len && line.at(i + 1) == QChar('[')) {
            // CSI 序列: ESC [ <params> <final>
            int params_start = i + 2;
            int final_idx = params_start;
            while (final_idx < n_len) {
                QChar fc = line.at(final_idx);
                // final byte: 0x40-0x7E (ASCII @ .. ~)
                if (fc.unicode() >= 0x40 && fc.unicode() <= 0x7e) break;
                final_idx++;
            }
            if (final_idx >= n_len) {
                // 序列未结束 (行被截断), 把剩下的当纯文本追加
                curText += line.mid(i);
                i = n_len;
                break;
            }
            QChar final_byte = line.at(final_idx);
            QString params_str = line.mid(params_start, final_idx - params_start);

            if (final_byte == QChar('m')) {
                // SGR: 解析参数, 改 currentColorHex
                QStringList ps;
                if (params_str.isEmpty()) {
                    ps << "0";
                } else {
                    ps = params_str.split(';', Qt::SkipEmptyParts);
                    if (ps.isEmpty()) ps << "0";
                }
                QString new_color = sgrToColor(ps);
                if (!new_color.isEmpty()) {
                    currentColorHex = new_color;
                } else if (ps.contains("0") || params_str.isEmpty()) {
                    // 重置
                    currentColorHex = symbolColorHex;
                }
                // 其它 SGR (粗体/下划线等) — 忽略, 不影响颜色
            }
            // 其它 CSI 序列 (清屏/光标移动) — 不处理, 直接跳过

            // 提交已有 curText
            if (!curText.isEmpty()) {
                segs.append({currentColorHex, curText});
                curText.clear();
            }
            i = final_idx + 1;
            continue;
        }
        curText += c;
        i++;
    }
    if (!curText.isEmpty()) {
        segs.append({currentColorHex, curText});
    }

    // 没任何段 (空行) — 走 plain
    if (segs.isEmpty()) {
        view->appendPlainText(line + "\n");
        return;
    }

    // fast path: 只有一段且颜色与 symbolColorHex 相同, 而且没有 ANSI 颜色覆盖
    // (即没有遇到 ESC[xxm — 那时 sgrToColor 会改 currentColorHex)
    // 仅有 1 段 + symbolColorHex 空时 (普通行) 用 appendPlainText
    if (segs.size() == 1 && segs[0].colorHex == symbolColorHex && symbolColorHex.isEmpty()) {
        view->appendPlainText(segs[0].text + "\n");
        return;
    }

    // 富文本: QTextCursor + QTextCharFormat
    QTextCursor cur = view->textCursor();
    cur.movePosition(QTextCursor::End);
    for (const Seg& sg : segs) {
        QTextCharFormat fmt;
        if (!sg.colorHex.isEmpty()) fmt.setForeground(QColor(sg.colorHex));
        cur.insertText(sg.text, fmt);
    }
    cur.insertText("\n");
    // 自动滚到底
    QScrollBar* sb = view->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}

}  // namespace ab
