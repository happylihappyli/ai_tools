#ifndef AB_THEME_H
#define AB_THEME_H
// SPDX-License-Identifier: MIT
//
// AbTheme — 主题管理 (dark/light)
//
// 用 QSS 文件 (ui/ab_dark.qss, ui/ab_light.qss),
// 找不到 QSS 时用内嵌字符串 fallback.

#include <QString>

namespace ab {

class AbTheme {
public:
    enum Kind { Dark, Light };

    // 应用主题到 QApplication
    static void apply(int kind);

    // 字符串解析: "dark" → Dark, "light" → Light, 其它 → Dark
    static Kind parse(const QString& name);

    // 当前主题 (从 ~/.config/ai_tools/theme.json 读, 默认 dark)
    static Kind current();

    // 保存主题
    static void save(Kind kind);
};

}  // namespace ab

#endif
