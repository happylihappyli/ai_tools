#ifndef AB_THEME_H
#define AB_THEME_H
// SPDX-License-Identifier: MIT
//
// AbTheme — 主题管理 (dark/light/solarized/nord)
//
// 2026-09-02: 加 solarized + nord 主题, 4 选 1.
//
// 用 QSS 文件 (ui/ab_<name>.qss), 找不到时用内嵌字符串 fallback.
// 主题切换后遍历所有 widget 调 unpolish/polish 强制刷新, 否则 setStyleSheet 不生效.

#include <QString>

namespace ab {

class AbTheme {
public:
    enum Kind { Dark, Light, Solarized, Nord, NumThemes };

    // 应用主题到 QApplication, 同时存 ~/.config/ai_tools/theme.json
    static void apply(int kind);

    // 字符串解析: "dark"/"light"/"solarized"/"nord" → Kind, 其它 → Dark
    static Kind parse(const QString& name);

    // Kind → 显示名 (e.g. "暗色 (default)")
    static QString displayName(int kind);

    // 短名 (e.g. "dark")
    static QString shortName(int kind);

    // 当前主题 (从 ~/.config/ai_tools/theme.json 读, 默认 Dark)
    static Kind current();

    // 保存主题
    static void save(Kind kind);

    // 重新加载所有 widget 的 QSS (主题切换时调)
    static void refreshAllWidgets();

    // 2026-09-02: 拿内嵌 QSS 字符串 (dry-run 验证用, 不触发 save/apply)
    static QString embeddedQssForTest(int kind);
};

}  // namespace ab

#endif
