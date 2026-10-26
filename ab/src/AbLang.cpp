// AbLang.cpp — 中文/English 语言管理 + UI 翻译表 + 编译日志词汇翻译
// 2026-10-26 加 (用户要求: 按钮尽量中文 + 可切换语言自动保存 + log 部分词汇翻译)
#include "AbLang.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHash>

namespace ab {

// =====================================================================
// 持久化: ~/.config/ai_tools/lang.json
// =====================================================================
static QString langConfigPath() {
    QStringList p = QStandardPaths::standardLocations(QStandardPaths::ConfigLocation);
    if (!p.isEmpty()) return p.first() + "/ai_tools/lang.json";
    return QDir::homePath() + "/.ai_tools_lang.json";
}

static AbLang::Kind s_current_kind = AbLang::NumLangs;  // 未初始化标记
static bool s_loaded = false;

static AbLang::Kind loadFromDisk() {
    QFile f(langConfigPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return AbLang::Zh;
    QByteArray data = f.readAll();
    f.close();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return AbLang::Zh;
    return AbLang::parse(doc.object().value("lang").toString());
}

AbLang::Kind AbLang::current() {
    if (!s_loaded) {
        s_current_kind = loadFromDisk();
        s_loaded = true;
    }
    return s_current_kind;
}

void AbLang::apply(int kind) {
    if (kind < 0 || kind >= NumLangs) kind = Zh;
    s_current_kind = static_cast<Kind>(kind);
    s_loaded = true;
    // 写 lang.json (跟 theme.json 同目录同风格)
    QFileInfo fi(langConfigPath());
    QDir().mkpath(fi.absolutePath());
    QFile f(langConfigPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QJsonObject o;
    o["lang"] = shortName(kind);
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

QString AbLang::displayName(int kind) {
    switch (kind) {
        case Zh: return QString::fromUtf8("中文");
        case En: return QString::fromUtf8("English");
    }
    return QString::fromUtf8("中文");
}

QString AbLang::shortName(int kind) {
    switch (kind) {
        case Zh: return "zh";
        case En: return "en";
    }
    return "zh";
}

AbLang::Kind AbLang::parse(const QString& name) {
    QString n = name.trimmed().toLower();
    if (n == "en" || n == "english") return En;
    return Zh;
}

// =====================================================================
// UI 翻译表 (en, zh) 双向索引
//   T("Stop")      → zh: "停止"        (en key → zh)
//   T("⏹ 停止")    → en: "⏹ Stop"     (zh key → en)
// =====================================================================
namespace {

struct TrEntry { const char* en; const char* zh; };

// 说明: 带 %1/%2 的是 pattern, 调用方拿到后自己 .arg(...)
static const TrEntry kTrTable[] = {
    // ---------- 菜单栏 ----------
    {"File (&F)", "文件(&F)"},
    {"GitHub Token Manager...", "GitHub Token 管理..."},
    {"Backup Project...", "备份项目..."},
    {"Quit", "退出"},
    {"Tools (&T)", "工具(&T)"},
    {"GitHub Token Diagnostics", "GitHub Token 诊断"},
    {"Proxy Test...", "代理测试..."},
    {"TTS Speak...", "TTS 播报..."},
    {"Log (&L)", "日志(&L)"},
    {"📋 Copy Log", "📋 复制 log"},
    {"📂 Open Log Directory", "📂 打开 log 目录"},
    {"View (&V)", "视图(&V)"},
    {"Show Log Panel", "显示日志面板"},
    {"📋 Open Task Inspector", "📋 打开任务检查器"},
    {"Theme (&T)", "主题(&T)"},
    {"Language (&L)", "语言(&L)"},
    {"Help (&H)", "帮助(&H)"},
    {"About ab...", "关于 ab..."},
    {"Main Toolbar", "主工具栏"},

    // ---------- 内置工具栏按钮 ----------
    {"Run selected task (F5)", "跑选中任务 (F5)"},
    {"Run Auto chain (F6)", "跑 Auto 链 (F6)"},
    {"⚡ Build && Run (Vulkan)", "⚡ 编译并启动 (Vulkan)"},
    {"⚡ Build && Run (OpenGL)", "⚡ 编译并启动 (OpenGL)"},
    {"Build project (build+deploy task), then start start_cloud_main.sh (Vulkan)",
     "编译当前项目 (走 build+deploy task), 成功后自动调 start_cloud_main.sh (Vulkan)"},
    {"Build project, then start start_cloud_main.sh --gl (OpenGL compat)",
     "编译当前项目, 成功后自动调 start_cloud_main.sh --gl (OpenGL 兼容模式)"},
    {"🚀 Run", "🚀 启动"},
    {"🚀 Run %1", "🚀 启动 %1"},
    {"Run compiled %1", "启动已编译的 %1"},
    {"🟢 Run Compat Mode", "🟢 启动兼容模式"},
    {"Run compiled %1 (OpenGL compat mode)", "启动已编译的 %1 (OpenGL 兼容模式)"},
    {"🎯 AR Runner", "🎯 AR 运行工具"},
    {"Open AI Run tool via ac ar (reads ai_build.json run_panel)",
     "调 ac ar 打开 AI Run 运行工具 (读 ai_build.json run_panel 段)"},
    {"■ Stop", "■ 停止"},
    {"Stop current task", "停止当前 task"},
    {"📊 Task Runner", "📊 任务运行器"},
    {"Open Task Runner window (sub-task list + live log + current + details)",
     "打开独立任务运行器窗口 (仿 ac_task_runner_gui.py)\n左侧 sub-task 列表 + 右侧实时 log + ⚡ 当前执行 + 详情区\n底部按钮: Stop / 复制 log / 打开 log / 启动 cloud_main"},

    // ---------- 中央面板 ----------
    {"⏹ Stop", "⏹ 停止"},
    {"Actions", "操作"},
    {"⏸ No Task", "⏸ 暂无任务"},
    {"⏳ Waiting", "⏳ 等待启动"},
    {"Startup tips (click to view)", "启动提示 (点开看)"},
    {"Startup tips: %1 sections (click 🔔 to view)", "启动提示 %1 段, 点开看"},
    {"⚡ Startup Tip (%1/%2)", "⚡ 启动提示 (%1/%2)"},
    {"auto-exit countdown:", "auto-exit 倒计时:"},
    {" sec", "秒"},
    {"Wait N seconds before auto-exit after auto chain finishes. 0 = exit immediately. Range [0, 60]",
     "auto 链跑完后等几秒自动退出. 0 = 立即退. 范围 [0, 60]"},
    {"✕ Cancel Exit", "✕ 取消退出"},
    {"✕ Cancel Exit (%1s)", "✕ 取消退出 (%1s)"},
    {"Click to cancel the auto-exit countdown (keep GUI open)",
     "auto-exit 倒计时中点这个取消退出 (GUI 留着)"},
    {"auto-exit wait set to %1 sec (saved to ai_build.json)",
     "auto-exit 等待时间设 %1 秒 (写回 ai_build.json)"},
    {"0 / %1  (sub-task)", "0 / %1  (子任务)"},
    {"%1 / %2  (sub-task)", "%1 / %2  (子任务)"},
    {"⚡ Current", "⚡ 当前执行"},
    {"(waiting for sub-task to start)", "(等待 sub-task 开始)"},
    {"📋 Sub-task Progress", "📋 子任务进度"},
    {"📄 Live Log (%1 lines)", "📄 实时日志 (%1 行)"},
    {"📜 Sub-task Details", "📜 子任务详情"},
    {"(no sub-task selected)", "(未选中子任务)"},
    {"📂 Open sub-task log", "📂 打开 sub-task log"},
    {"📋 Copy Full Command", "📋 复制完整命令"},
    {"Close details panel (click a sub-task row to reopen)",
     "关闭详情面板 (点击左侧 sub-task 重新打开)"},
    {"(click a sub-task row on the left to see the full command)",
     "(点击左侧 sub-task 行, 显示完整命令)"},
    {"  ⏳ [%1/%2] waiting...", "  ⏳ [%1/%2] 等待…"},
    {"sub-task [%1/%2]  running...\n", "sub-task [%1/%2]  跑中…\n"},
    {"✓ Done", "✓ 完成"},
    {"✗ Fail (rc=%1)", "✗ 失败 (rc=%1)"},
    {"⏳ Running (%1/%2)", "⏳ 运行中 (%1/%2)"},
    {"✗ Failed at %1/%2", "✗ 第 %1/%2 步失败"},
    {"[DONE]", "[完成]"},
    {"🔨 compiled %1 files", "🔨 已编译 %1 个文件"},
    {"🔨 compiled %1 (%2)", "🔨 已编译 %1 个 (%2)"},

    // ---------- 状态栏 ----------
    {"Ready", "就绪"},
    {"(none)", "(无)"},
    {"Renderer: —", "渲染模式: —"},
    {"Renderer: Vulkan (Forward+)", "渲染模式: Vulkan (Forward+)"},
    {"Renderer: OpenGL3 (Compatibility)", "渲染模式: OpenGL3 (Compatibility)"},
    {"Qt=%1 | Theme=%2", "Qt=%1 | 主题=%2"},

    // ---------- 主题/语言显示名 ----------
    {"🌑 Dark (default)", "🌑 暗色 (默认)"},
    {"☀️ Light", "☀️ 浅色"},
    {"🟤 Solarized Dark", "🟤 Solarized 暗色"},
    {"❄️ Nord", "❄️ Nord 冷色"},
    {"Language switched: %1 (saved)", "语言切换: %1 (已保存)"},

    // ---------- 对话框 ----------
    {"Not Found", "未找到"},
    {"%1 does not exist, run build+deploy first", "%1 不存在, 请先跑 build+deploy 编译"},
    {"Not Configured", "未配置"},
    {"ai_build.json has no build+deploy task", "ai_build.json 缺 build+deploy 任务"},
    {"About ab", "关于 ab"},
    {"ab — AI Build GUI (C++ Qt5/6)\nVersion: 1.0.0\nProject: %1\nTasks: %2\nTheme: %3\n\nab is a generic AI-assisted build tool,\ndriven by ai_build.json.\nTo change buttons/menus, edit the ui section of ai_build.json.",
     "ab — AI Build 编译/调试 GUI (C++ Qt5/6)\n版本: 1.0.0\n项目: %1\n任务: %2 个\n主题: %3\n\nab 是通用的 AI 辅助构建工具,\n通过 ai_build.json 配置文件驱动 UI.\n想改按钮/菜单, 改 ai_build.json 的 ui 段即可."},
    {"TTS Speak", "TTS 播报"},
    {"Text to speak:", "请输入要播报的内容:"},
    {"ab toolchain ready", "ab 工具链就绪"},
    {"TTS cancelled", "TTS 已取消"},
    {"Confirm", "确认"},
    {"Kill process pid=%1 ?", "杀进程 pid=%1 ?"},
    {"Process Action", "进程操作"},
    {"Process: %1\nPID: %2\n\nYes = kill (TERM + KILL), No = cancel",
     "进程: %1\nPID: %2\n\n点 Yes 杀进程 (TERM + KILL), No 取消"},

    // ---------- 常用 log 信息 ----------
    {"ab started v1.0.0", "ab 启动 v1.0.0"},
    {"Project: %1", "项目: %1"},
    {"Tasks: %1", "任务数: %1"},
    {"Theme: %1", "主题: %1"},
    {"Language: %1", "语言: %1"},
    {"Auto chain: %1", "auto 链: %1"},
    {"✓ %1 ready", "✓ %1 已就绪"},
    {"%1 not found, enabled after build", "未找到 %1, 编译后可启用"},
    {"TTS enabled: %1", "TTS 已启用: %1"},
    {"TTS unavailable: spd-say not found", "TTS 不可用: 未找到 spd-say"},
    {"Triggered action: %1", "触发动作: %1"},
    {"Run auto chain: %1", "跑 auto 链: %1"},
    {"Auto chain is empty (define in ai_build.json auto field)",
     "auto 链为空 (在 ai_build.json 的 auto 字段定义)"},
    {"▶ run task [%1]: %2 (mode=%3)", "▶ 跑 task [%1]: %2 (mode=%3)"},
    {"sub-task list (%1 steps)", "sub-task 列表 (%1 步)"},
    {"(%1 sub-tasks)", "(%1 个子任务)"},
    {"▶ run cmd [%1]: %2", "▶ 跑 cmd [%1]: %2"},
    {"[%1] exit code %2  elapsed %3s", "[%1] 退出码 %2  耗时 %3s"},
    {"[%1] QProcess error %2", "[%1] QProcess 错误码 %2"},
    {"[%1] rc=%2 error log: %3", "[%1] rc=%2 错误日志: %3"},
    {"A task is already running, stop it first", "已有 task 在跑, 请先停止"},
    {"Unknown task: %1", "未知 task: %1"},
    {"No task running", "没有 task 在跑"},
    {"Stopping current task", "停止当前 task"},
    {"⏹ Stopping task...", "⏹ 正在停止 task..."},
    {"Auto chain finished (all_ok=%1 last_rc=%2 auto_exit=%3)",
     "auto 链结束 (all_ok=%1 last_rc=%2 auto_exit=%3)"},
    {"✓ auto chain succeeded, exiting in %1 s (click ✕ to cancel)",
     "✓ auto 链成功, %1 秒后自动退出 (点 ✕ 取消)"},
    {"⚠ auto chain failed (rc=%1), exiting in %2 s — copy: cat /tmp/ab-error-*.log (click ✕ to cancel)",
     "⚠ auto 链失败 (rc=%1), %2 秒后自动退出 — 复制: cat /tmp/ab-error-*.log | grep task_name=<FAILED_TASK> (点 ✕ 取消)"},
    {"✓ success, exiting in %1 s (click ✕ to cancel)",
     "✓ 成功, %1 秒后退出 (点 ✕ 取消)"},
    {"⚠ failed (rc=%1), exiting in %2 s — copy cat /tmp/ab-error-*.log (click ✕ to cancel)",
     "⚠ 失败 (rc=%1), %2 秒后退出 — 复制 cat /tmp/ab-error-*.log (点 ✕ 取消)"},
    {"✕ user cancelled auto-exit", "✕ 用户取消了 auto-exit"},
    {"✕ user clicked cancel-exit (next timer tick won't quit)", "✕ 用户点了取消退出 (next timer tick 不再 quit)"},
    {"Undefined action: %1", "未定义的动作: %1"},
    {"Menu item %1 has no task/cmd", "菜单项 %1 没有 task/cmd"},
    {"Task Inspector window not open, press Ctrl+I first",
     "任务检查器窗口未打开, 先按 Ctrl+I 打开 [任务检查器] 窗口"},
    {"No task selected (select a row in Task Inspector → 📋 Tasks, then F5)",
     "没选中任务 (在 [任务检查器] 窗口 → [📋 任务] tab 选中一行, 再按 F5)"},
    {"📊 Task Runner opened — %1", "📊 任务运行器已打开 — %1"},
    {"📋 Task Inspector opened (separate window — 📋 Tasks / ⚙️ Processes / 🔧 sub-task)",
     "📋 任务检查器已打开 (独立窗口 — 📋 任务 / ⚙️ 进程 / 🔧 sub-task)"},
    {"✓ log copied to clipboard", "✓ log 已复制到剪贴板"},
    {"✓ full command copied to clipboard", "✓ 完整命令已复制到剪贴板"},
    {"📂 Opened %1", "📂 打开 %1"},
    {"⚠ No file manager found, log dir: %1", "⚠ 找不到文件管理器, log 目录: %1"},
    {"⚠ No log found for sub-task %1", "⚠ 找不到 sub-task %1 的 log"},
    {"Reload ai_build.json", "重读 ai_build.json"},
    {"Reload ai_build.json + rebuild toolbar/menus/buttons (no ab restart needed)",
     "重读 ai_build.json + 重建工具栏/菜单/按钮 (免去重启 ab)"},
    {"[F5] re-reading ai_build.json...", "[F5] 重读 ai_build.json..."},
    {"[auto-reload] ai_build.json changed, re-reading...",
     "[auto-reload] ai_build.json 改动, 自动重读..."},
    {"🚀 Launch %1 (background)", "🚀 启动 %1 (后台)"},
    {"🚀 Launch %1 (OpenGL compat mode)", "🚀 启动 %1 (OpenGL 兼容模式)"},
    {"✗ Launch failed: %1", "✗ 启动失败: %1"},
    {"✗ ac not found, cannot open Token Manager (install ai_tools and check ~/.local/bin in PATH)",
     "✗ ac 未找到, 没法打开 Token 管理 (请装 ai_tools 并确认 ~/.local/bin 在 PATH)"},
    {"✓ Token manager started, pid=%1", "✓ Token 管理已启动, pid=%1"},
    {"✗ failed to start ac ght", "✗ 启动 ac ght 失败"},
    {"✗ ac not found, backup unavailable", "✗ ac 未找到, 备份功能不可用"},
    {"✗ ac not found, cannot run proxy test", "✗ ac 未找到, 没法跑代理测试"},

    // ---------- TaskRunnerWindow ----------
    {"📊 Task Runner — %1", "📊 任务运行器 — %1"},
    {"⏹ Stop (kill task)", "⏹ 停止 (杀 task)"},
    {"🚀 Launch cloud_main", "🚀 启动 cloud_main"},
    {"Run", "运行"},
    {"Close", "关闭"},
    {"✗ Launch failed", "✗ 启动失败"},
    {"Launch bin/Debug/cloud_main (Vulkan + Wayland)\nFor: after ac task, view 3D area + FPS Overlay",
     "启动 bin/Debug/cloud_main (Vulkan + Wayland)\n适用于: ac task 跑完后看 3D 区域 + FPS Overlay"},
    {"🚀 sub-task [%1/%2] started", "🚀 sub-task [%1/%2] 开始"},
    {"✗ sub-task [%1/%2] failed, rest skipped", "✗ sub-task [%1/%2] 失败, 后续停止"},
    {"✓ %1 done (rc=0)  log: %2/latest.log", "✓ %1 完成 (rc=0)  log: %2/latest.log"},
    {"✗ %1 failed (rc=%2)", "✗ %1 失败 (rc=%2)"},
    {"cloud_main started (pid=%1, Vulkan+Wayland)", "cloud_main 已启动 (pid=%1, Vulkan+Wayland)"},
    {"Binary not found:\n%1\n\nRun ac task to build first", "找不到 binary:\n%1\n\n请先跑 ac task 编译一次"},

    // ---------- AbTaskInspector ----------
    {"Task Inspector", "任务检查器"},
    {"ab — Task Inspector (📋 Tasks + ⚙️ Processes + 🔧 sub-task)",
     "ab — 任务检查器 (📋 任务 + ⚙️ 进程 + 🔧 sub-task)"},
    {"🔍 Filter (task / process / PID)", "🔍 过滤 (任务名 / 进程名 / PID)"},
    {"Auto Refresh", "自动刷新"},
    {"🔄 Refresh", "🔄 刷新"},
    {"📋 Tasks", "📋 任务"},
    {"⚙️ Processes", "⚙️ 进程"},
    {"⏸ No task running", "⏸ 暂无任务在跑"},
    {"%v / %m  (sub-task)", "%v / %m  (子任务)"},
    {"Status", "状态"},
    {"Task", "任务名"},
    {"Description", "说明"},
    {"Last Result", "上次结果"},
    {"Time", "耗时"},
    {"Timestamp", "时间"},
    {"Command", "命令"},
    {"User", "用户"},
    {"Elapsed", "已运行"},
    {"Memory%", "内存%"},
    {"running", "跑中"},
    {"⚡ trigger task: %1", "⚡ 触发跑任务: %1"},
    {"Tasks: %1 (showing %2) | current: %3", "任务: %1 个 (显示 %2) | 当前跑: %3"},
    {"Tasks: %1 | Processes: %2 (project related) | current: %3",
     "任务: %1 个 | 进程: %2 个 (项目相关) | 当前跑: %3"},
    {"Process refresh failed (ps timeout)", "进程刷新失败 (ps 超时)"},
    {"🔧 %1 (%2 sub-tasks)", "🔧 %1 (%2 个子任务)"},
    {"Progress: 0 / %1", "进度: 0 / %1"},
    {"Progress: %1 / %2  ·  current: %3", "进度: %1 / %2  ·  当前: %3"},
    {"Progress: %1 / %2  ·  last: %3s (rc=%4)", "进度: %1 / %2  ·  上一步: %3s (rc=%4)"},
    {"✗✗ %1  (step %2/%3 failed, rest skipped)", "✗✗ %1  (第 %2/%3 步失败, 后续不跑)"},
    {"✗ failed: step %1/%2 → rest skipped", "✗ 失败: 第 %1/%2 步 → 后续 sub-task 跳过"},

    // ---------- AbLogDock (legacy, 但还在编译) ----------
    {"Operation Log", "操作日志"},
    {"Auto Scroll: On", "自动滚动: 开"},
    {"Auto Scroll: Off", "自动滚动: 关"},
    {"📋 Copy Errors", "📋 复制错误"},
    {"📄 Copy All", "📄 复制全部"},
    {"Clear", "清空"},
    {"Cleared", "已清空"},
    {"Copy filtered error/warn/traceback lines (FATAL, undefined reference etc., with [E]/[W] prefix)",
     "复制过滤后的 error/warn/traceback 行 (识别 FATAL, undefined reference 等关键字, 加 [E]/[W] 前缀)"},
    {"Copy all log content (with timestamps)", "复制全部 log 内容 (含时间戳)"},
};

// 双向索引 (首次调用构建)
struct TrIndex {
    QHash<QString, QString> en2zh;
    QHash<QString, QString> zh2en;
    bool built = false;
    void build() {
        if (built) return;
        const int n = sizeof(kTrTable) / sizeof(kTrTable[0]);
        for (int i = 0; i < n; ++i) {
            const QString en = QString::fromUtf8(kTrTable[i].en);
            const QString zh = QString::fromUtf8(kTrTable[i].zh);
            en2zh.insert(en, zh);
            zh2en.insert(zh, en);
        }
        built = true;
    }
};

static TrIndex& trIndex() {
    static TrIndex idx;
    idx.build();
    return idx;
}

}  // namespace

QString AbLang::T(const char* s) {
    if (!s || !*s) return QString();
    const QString src = QString::fromUtf8(s);
    const TrIndex& idx = trIndex();
    if (current() == Zh) {
        // zh 模式: 英文 key → 中文; 中文 key / 未收录 → 原样
        return idx.en2zh.value(src, src);
    }
    // en 模式: 中文 key → 英文; 英文 key / 未收录 → 原样
    return idx.zh2en.value(src, src);
}

// =====================================================================
// 编译日志"部分词汇翻译" (用户示例:
//   "Compiling editor/editor_node.cpp ..." → "编译 editor/editor_node.cpp ..."
//   "WARNING: msgfmt execution failed"     → "警告: msgfmt execution failed"
// 文件名/路径/技术细节 (如 msgfmt execution failed) 保持原样)
// =====================================================================
QString AbLang::translateLogLine(const QString& line) {
    if (line.isEmpty() || current() != Zh) return line;
    QString out = line;

    // 长短语优先 (先于单词替换, 避免 "done building targets" 被单词先拆)
    static const TrEntry kPhrases[] = {
        {"scons: Reading SConscript files...", "scons: 读取 SConscript 文件..."},
        {"scons: done reading SConscript files.", "scons: SConscript 读取完成."},
        {"scons: done building targets.", "scons: 全部构建完成."},
        {"scons: Building targets ...", "scons: 构建目标 ..."},
        {"scons: building associated VariantDir targets ...", "scons: 构建关联 VariantDir 目标 ..."},
        {"fatal error:", "致命错误:"},
        {"undefined reference", "未定义引用"},
    };
    for (const auto& p : kPhrases) {
        out.replace(QString::fromUtf8(p.en), QString::fromUtf8(p.zh), Qt::CaseSensitive);
    }

    // 词汇级 (带冒号的日志级别 + 编译动作词)
    static const TrEntry kWords[] = {
        {"WARNING:", "警告:"},
        {"warning:", "警告:"},
        {"ERROR:",   "错误:"},
        {"error:",   "错误:"},
        {"FATAL:",   "致命:"},
        {"fatal:",   "致命:"},
        {"CRITICAL:", "严重:"},
        {"NOTICE:",  "注意:"},
        {"Compiling", "编译"},
        {"Linking",   "链接"},
        {"Generating", "生成"},
        {"Installing", "安装"},
        {"Building",  "构建"},
    };
    for (const auto& w : kWords) {
        out.replace(QString::fromUtf8(w.en), QString::fromUtf8(w.zh), Qt::CaseSensitive);
    }
    return out;
}

}  // namespace ab
