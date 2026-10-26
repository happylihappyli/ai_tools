#ifndef AB_LANG_H
#define AB_LANG_H
// SPDX-License-Identifier: MIT
//
// AbLang — 语言管理 (中文/English, 2026-10-26 加)
//
// 用户要求:
//   1. 所有按钮/菜单尽量翻译为中文 (默认中文)
//   2. 用户可切换语言 (视图 → 语言), 自动保存
//   3. 实时 log 窗口的编译输出做"部分词汇翻译":
//        "Compiling editor/editor_node.cpp ..." → "编译 editor/editor_node.cpp ..."
//        "WARNING: msgfmt execution failed"     → "警告: msgfmt execution failed"
//
// 持久化: ~/.config/ai_tools/lang.json  (跟 theme.json 同目录, {"lang": "zh"|"en"})
//
// 用法:
//   AbLang::T("Stop")            → zh 模式 "停止" / en 模式 "Stop"
//   AbLang::T("⏹ 停止")          → zh 模式原样 / en 模式查表返回 "⏹ Stop"
//   AbLang::translateLogLine(s)  → 仅 zh 模式做编译日志词汇翻译 (文件名/路径不动)
//
// 双向表: 每条 entry = (en, zh)。T() 在 zh 模式查 en→zh, 在 en 模式查 zh→en,
// 查不到原样返回 — 所以代码里传英文或中文 key 都行。

#include <QString>

namespace ab {

class AbLang {
public:
    enum Kind { Zh, En, NumLangs };

    // 设置语言 + 写 lang.json
    static void apply(int kind);

    // 当前语言 (首次调用从 lang.json 读, 默认 Zh)
    static Kind current();

    // "zh"/"en" → Kind (其它 → Zh)
    static Kind parse(const QString& name);

    // 显示名: "中文" / "English"
    static QString displayName(int kind);

    // 短名: "zh" / "en"
    static QString shortName(int kind);

    // UI 字符串翻译 (双向表, 支持 %1/%2 占位符 — 返回 pattern 后再 .arg())
    static QString T(const char* s);

    // 编译日志行"部分词汇翻译" (仅 zh 模式; WARNING:/ERROR:/Compiling/Linking/...)
    // 文件名/路径/技术细节保持原样, 只换词汇。
    static QString translateLogLine(const QString& line);
};

}  // namespace ab

#endif
