// AbMainWindow.cpp
#include "AbMainWindow.h"
#include "AbTaskRunner.h"
#include "AbLogDock.h"
#include "AbTheme.h"
#include "AbTaskInspector.h"  // 2026-09-02: 任务/进程检查器 dock

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QGroupBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QKeySequence>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QApplication>
#include <QDebug>
#include <unistd.h>
#include <cstdlib>

namespace ab {

AbMainWindow::AbMainWindow(const AbConfig& cfg, QWidget* parent)
    : QMainWindow(parent), cfg_(cfg) {
    setWindowTitle(cfg_.title);
    resize(cfg_.window);

    runner_ = new AbTaskRunner(this);
    wireRunner();

    buildFromConfig();

    log("info", QString("ab 启动 v1.0.0"));
    log("info", QString("项目: %1").arg(cfg_.cwd));
    log("info", QString("任务数: %1").arg(static_cast<int>(cfg_.tasks.size())));
    log("info", QString("主题: %1").arg(cfg_.theme));

    // 检查 cloud_main (如果配置了 run_after_build)
    if (!cfg_.run_after_build.binary_path.isEmpty()) {
        QString bin = findRunBinary();
        if (!bin.isEmpty()) {
            cloud_binary_ = bin;
            enableRunCloudButton(true);
            log("ok", QString("✓ %1 已就绪").arg(QFileInfo(bin).fileName()));
        } else {
            log("warn", QString("未找到 %1, 编译后可启用").arg(cfg_.run_after_build.binary_path));
        }
    }

    // 探测外部工具绝对路径 (避免桌面 GUI PATH 不带 ~/.local/bin)
    ac_binary_     = findTool("ac");
    spd_say_binary_ = findTool("spd-say");
    bak_binary_    = findTool("bak");
    if (ac_binary_.isEmpty()) {
        log("warn", "未找到 ac 命令, 菜单 [GitHub Token 管理] 等会失败 (PATH 不全?)");
    } else {
        log("info", QString("ac: %1").arg(ac_binary_));
    }
    if (spd_say_binary_.isEmpty()) {
        log("warn", "未找到 spd-say, TTS 播报不可用");
    }

    // auto_start
    if (cfg_.auto_start) {
        QTimer::singleShot(300, this, &AbMainWindow::onRunAuto);
    }
}

AbMainWindow::~AbMainWindow() = default;

void AbMainWindow::wireRunner() {
    connect(runner_, &AbTaskRunner::output,   this, &AbMainWindow::onOutput);
    connect(runner_, &AbTaskRunner::finished, this, &AbMainWindow::onFinished);
    connect(runner_, &AbTaskRunner::error,    this, &AbMainWindow::onError);
}

void AbMainWindow::buildFromConfig() {
    buildTaskList();
    buildStatusBar();
    buildBuiltInMenus();   // 2026-09-02: 框架内置通用菜单 (GitHub Token/TTS/备份/视图/帮助)
    buildBuiltInToolbar(); // 2026-09-02: 框架内置通用工具栏 (跑选中/跑 Auto/停止)
    buildBuiltInButtons();  // 2026-09-02: 框架内置通用按钮 (编译并启动/启动/停止) - 仅当 run_after_build 有
    buildMenus();           // 配置文件追加: "运行" 菜单 (项目特定)
    buildToolbar();         // 配置文件追加: 工具栏 (项目特定, e.g. TTS 按钮)
    buildMainButtons();     // 配置文件追加: 主按钮行 (项目特定, e.g. 自定义任务)
    if (cfg_.show_log_dock) {
        log_dock_ = new AbLogDock(this);
        addDockWidget(Qt::BottomDockWidgetArea, log_dock_);
    }
    // 2026-09-02: 任务/进程检查器 dock (放右侧, 默认显示)
    inspector_ = new AbTaskInspector(this);
    inspector_->setConfig(cfg_);
    inspector_->setCwd(cfg_.cwd);
    addDockWidget(Qt::RightDockWidgetArea, inspector_);
    // tabify 在 log dock 旁边
    if (log_dock_) tabifyDockWidget(log_dock_, inspector_);
}

// 2026-09-02: 框架内置通用菜单 (所有调试程序都需要)
//   - 文件: GitHub Token / 备份 / 退出
//   - 工具: GitHub Token 诊断 / TTS 播报 / 代理测试
//   - 视图: 显示日志 / 切换主题
//   - 帮助: 关于
// ai_build.json 不再需要配这些, 减少冗余
void AbMainWindow::buildBuiltInMenus() {
    QMenuBar* mb = menuBar();

    // --- 文件 (&F) ---
    QMenu* m_file = mb->addMenu("文件(&F)");
    {
        QAction* a = m_file->addAction("GitHub Token 管理...");
        a->setShortcut(QKeySequence("Ctrl+G"));
        a->setData("open_ght");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["open_ght"] = a;
    }
    {
        QAction* a = m_file->addAction("备份项目...");
        a->setShortcut(QKeySequence("Ctrl+B"));
        a->setData("bak");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["bak"] = a;
    }
    m_file->addSeparator();
    {
        QAction* a = m_file->addAction("退出");
        a->setShortcut(QKeySequence("Ctrl+Q"));
        a->setData("quit");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["quit"] = a;
    }

    // --- 工具 (&T) ---
    QMenu* m_tools = mb->addMenu("工具(&T)");
    {
        QAction* a = m_tools->addAction("GitHub Token 诊断");
        a->setShortcut(QKeySequence("Ctrl+D"));
        a->setData("diag");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["diag"] = a;
    }
    {
        QAction* a = m_tools->addAction("代理测试...");
        a->setShortcut(QKeySequence("Ctrl+P"));
        a->setData("proxy_test");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["proxy_test"] = a;
    }
    m_tools->addSeparator();
    {
        QAction* a = m_tools->addAction("TTS 播报...");
        a->setShortcut(QKeySequence("Ctrl+T"));
        a->setData("tts");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["tts"] = a;
    }

    // --- 视图 (&V) ---
    QMenu* m_view = mb->addMenu("视图(&V)");
    {
        QAction* a = m_view->addAction("显示日志面板");
        a->setShortcut(QKeySequence("Ctrl+Shift+L"));
        a->setCheckable(true);
        a->setChecked(cfg_.show_log_dock);
        a->setData("toggle_log");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["toggle_log"] = a;
    }
    {
        // 2026-09-02: 任务检查器 (任务状态 + 进程)
        QAction* a = m_view->addAction("任务检查器");
        a->setShortcut(QKeySequence("Ctrl+I"));
        a->setCheckable(true);
        a->setChecked(true);
        a->setData("toggle_inspector");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["toggle_inspector"] = a;
    }
    // 2026-09-02: 主题改成 submenu 4 选 1 (单选 QActionGroup)
    {
        QMenu* m_theme = m_view->addMenu("主题(&T)");
        QActionGroup* group = new QActionGroup(this);
        group->setExclusive(true);
        AbTheme::Kind cur = AbTheme::current();
        for (int k = 0; k < AbTheme::NumThemes; ++k) {
            QAction* a = m_theme->addAction(AbTheme::displayName(k));
            a->setCheckable(true);
            a->setChecked(k == cur);
            a->setData(QString("theme:%1").arg(AbTheme::shortName(k)));
            connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
            group->addAction(a);
            actions_[QString("theme:%1").arg(AbTheme::shortName(k))] = a;
        }
    }

    // --- 帮助 (&H) ---
    QMenu* m_help = mb->addMenu("帮助(&H)");
    {
        QAction* a = m_help->addAction("关于 ab...");
        a->setData("about");
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_["about"] = a;
    }
}

// 2026-09-02: 框架内置通用工具栏 (跑选中/跑 Auto/停止 - 所有 task runner 通用)
void AbMainWindow::buildBuiltInToolbar() {
    QToolBar* tb = addToolBar("主工具栏");
    tb->setObjectName("AbMainToolBar");
    tb->setMovable(true);
    tb->setFloatable(true);

    auto add = [&](const QString& label, const QString& id, const QString& tip, const QString& sc = QString()) {
        QAction* a = tb->addAction(label);
        if (!tip.isEmpty())   a->setToolTip(tip);
        if (!sc.isEmpty())    a->setShortcut(QKeySequence(sc));
        a->setData(id);
        connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
        actions_[id] = a;
    };

    add("▶", "run_selected", "跑选中任务 (F5)", "F5");
    add("⚡", "run_auto",     "跑 Auto 链 (F6)", "F6");
    add("■", "stop",         "停止当前 task (F7)", "F7");
}

// 2026-09-02: 框架内置通用按钮 (编译并启动 / 启动 cloud_main / 停止)
//   仅当 ai_build.json 配置了 run_after_build.binary_path 时才加
void AbMainWindow::buildBuiltInButtons() {
    if (cfg_.run_after_build.binary_path.isEmpty()) return;
    QWidget* cw = centralWidget();
    if (!cw) return;
    QVBoxLayout* vl = qobject_cast<QVBoxLayout*>(cw->layout());
    if (!vl) return;

    QHBoxLayout* row = new QHBoxLayout();
    auto addBtn = [&](const QString& label, const QString& id, const QString& tip, const QString& color, bool enabled) {
        QPushButton* btn = new QPushButton(label, this);
        btn->setToolTip(tip);
        if (!color.isEmpty()) btn->setProperty("role", color);
        btn->setEnabled(enabled);
        btn->setProperty("abId", id);
        connect(btn, &QPushButton::clicked, this, &AbMainWindow::onActionTriggered);
        row->addWidget(btn);
        buttons_[id] = btn;
    };

    QString binary_name = QFileInfo(cfg_.run_after_build.binary_path).fileName();
    QString btn_label = cfg_.run_after_build.button_label.isEmpty()
                        ? QString("🚀 启动 %1").arg(binary_name)
                        : cfg_.run_after_build.button_label;
    addBtn(QString("⚡ 编译并启动"), "build_and_run",
           "编译 + 启动 " + binary_name, "primary", true);
    addBtn(btn_label, "run_cloud",
           "启动已编译的 " + binary_name, "success", !cloud_binary_.isEmpty());
    addBtn("■ 停止", "stop", "停止当前 task", "danger", false);

    row->addStretch(1);
    vl->addLayout(row);
}

void AbMainWindow::buildTaskList() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout* layout = new QVBoxLayout(central);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 顶部信息
    QFrame* info = new QFrame();
    QVBoxLayout* info_l = new QVBoxLayout(info);
    QLabel* proj = new QLabel(QString("📁 %1").arg(cfg_.cwd));
    QFont big;
    big.setPointSize(13);
    big.setBold(true);
    proj->setFont(big);
    info_l->addWidget(proj);
    QLabel* auto_lbl = new QLabel(QString("auto 链: %1")
        .arg(cfg_.auto_chain.isEmpty() ? "(无)" : cfg_.auto_chain.join(" → ")));
    info_l->addWidget(auto_lbl);
    layout->addWidget(info);

    // 任务列表
    QGroupBox* gb = new QGroupBox(QString("任务列表 (%1 个)").arg(static_cast<int>(cfg_.tasks.size())));
    QVBoxLayout* gb_l = new QVBoxLayout(gb);
    task_list_ = new QTreeWidget();
    task_list_->setHeaderLabels({"任务名", "说明", "命令"});
    task_list_->setRootIsDecorated(false);
    task_list_->setAlternatingRowColors(true);
    task_list_->setColumnWidth(0, 140);
    task_list_->setColumnWidth(1, 240);
    task_list_->setColumnWidth(2, 320);
    connect(task_list_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int){
        QString name = it->text(0);
        runTaskByName(name);
    });
    for (const auto& t : cfg_.tasks) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, t.name);
        item->setText(1, t.description);
        item->setText(2, t.cmd);
        task_list_->addTopLevelItem(item);
    }
    gb_l->addWidget(task_list_);
    layout->addWidget(gb, 1);

    // 进度条
    QHBoxLayout* prog = new QHBoxLayout();
    prog_label_ = new QLabel("当前: —");
    prog_bar_ = new QProgressBar();
    prog_bar_->setRange(0, 0);
    prog_bar_->setVisible(false);
    prog->addWidget(prog_label_, 1);
    prog->addWidget(prog_bar_, 2);
    layout->addLayout(prog);
}

void AbMainWindow::buildStatusBar() {
    statusbar_ = new QStatusBar(this);
    setStatusBar(statusbar_);
    sb_left_ = new QLabel("就绪");
    statusbar_->addWidget(sb_left_, 1);
    sb_right_ = new QLabel(QString("Qt=%1 | 主题=%2").arg(qApp ? "Qt5/6" : "?", cfg_.theme));
    statusbar_->addPermanentWidget(sb_right_);
}

QAction* AbMainWindow::createActionForButton(const AbButtonDef& b, QWidget* parent) {
    QAction* a = new QAction(b.label.isEmpty() ? b.id : b.label, parent);
    a->setToolTip(b.tooltip);
    if (!b.shortcut.isEmpty()) a->setShortcut(QKeySequence(b.shortcut));
    if (b.checkable) a->setCheckable(true);
    if (!b.enabled)  a->setEnabled(false);
    a->setData(b.id);
    connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
    return a;
}

void AbMainWindow::buildMenus() {
    if (cfg_.menus.empty()) return;
    QMenuBar* mb = menuBar();
    for (const auto& md : cfg_.menus) {
        QMenu* m = mb->addMenu(md.name);
        for (const auto& mi : md.items) {
            if (mi.type == ab::AbMenuItem::Separator) {
                m->addSeparator();
                continue;
            }
            QAction* a = new QAction(mi.label, this);
            if (!mi.shortcut.isEmpty()) a->setShortcut(QKeySequence(mi.shortcut));
            if (!mi.tooltip.isEmpty()) a->setToolTip(mi.tooltip);
            if (mi.checkable) a->setCheckable(true);
            a->setData(mi.id);
            connect(a, &QAction::triggered, this, &AbMainWindow::onActionTriggered);
            m->addAction(a);
            actions_[mi.id] = a;
        }
    }
}

void AbMainWindow::buildToolbar() {
    if (cfg_.toolbar.empty()) return;
    QToolBar* tb = addToolBar("主工具栏");
    tb->setObjectName("AbMainToolBar");
    tb->setMovable(true);
    tb->setFloatable(true);
    for (const auto& b : cfg_.toolbar) {
        QAction* a = createActionForButton(b, this);
        tb->addAction(a);
        actions_[b.id] = a;
    }
}

void AbMainWindow::buildMainButtons() {
    if (cfg_.buttons.empty()) return;
    // 在中心 widget 的进度条下面加按钮行
    QWidget* cw = centralWidget();
    if (!cw) return;
    QVBoxLayout* vl = qobject_cast<QVBoxLayout*>(cw->layout());
    if (!vl) return;

    QHBoxLayout* row = new QHBoxLayout();
    for (const auto& b : cfg_.buttons) {
        QPushButton* btn = new QPushButton(b.label, this);
        if (!b.tooltip.isEmpty()) btn->setToolTip(b.tooltip);
        if (!b.color.isEmpty())   btn->setProperty("role", b.color);
        if (!b.enabled)           btn->setEnabled(false);
        btn->setProperty("abId", b.id);
        connect(btn, &QPushButton::clicked, this, &AbMainWindow::onActionTriggered);
        row->addWidget(btn);
        buttons_[b.id] = btn;
    }
    row->addStretch(1);
    vl->addLayout(row);
}

void AbMainWindow::onActionTriggered() {
    QObject* s = sender();
    QString id;
    if (auto* a = qobject_cast<QAction*>(s)) {
        id = a->data().toString();
    } else if (auto* b = qobject_cast<QPushButton*>(s)) {
        id = b->property("abId").toString();
    }
    if (id.isEmpty()) return;
    log("debug", QString("触发动作: %1").arg(id));

    // 特殊 id 处理
    if (id == "run_selected") { onRunSelectedTask(); return; }
    if (id == "run_auto")     { onRunAuto(); return; }
    if (id == "stop")         { onStop(); return; }
    if (id == "run_cloud")    { onRunCloud(); return; }
    if (id == "build_and_run"){ onBuildAndRun(); return; }
    if (id == "toggle_theme") { onToggleTheme(); return; }
    // 2026-09-02: theme:dark/light/solarized/nord
    if (id.startsWith("theme:")) {
        QString name = id.mid(6);  // "dark" / "light" / "solarized" / "nord"
        AbTheme::Kind k = AbTheme::parse(name);
        AbTheme::apply(static_cast<int>(k));
        cfg_.theme = name;  // 同步到配置
        if (sb_right_) sb_right_->setText(QString("Qt=%1 | 主题=%2").arg(qApp ? "Qt5/6" : "?", name));
        log("ok", QString("主题切换: %1").arg(AbTheme::displayName(static_cast<int>(k))));
        return;
    }
    if (id == "toggle_log")   {
        if (log_dock_) log_dock_->setVisible(!log_dock_->isVisible());
        return;
    }
    if (id == "toggle_inspector") {
        if (inspector_) inspector_->setVisible(!inspector_->isVisible());
        return;
    }
    if (id == "about")        { onAbout(); return; }
    if (id == "quit")         { onQuit(); return; }
    if (id == "open_ght")     {
        // 跨进程: 调 ac ght (用探测到的绝对路径, 避免桌面 GUI PATH 不全)
        if (ac_binary_.isEmpty()) {
            log("err", "✗ ac 未找到, 没法打开 Token 管理 (请装 ai_tools 并确认 ~/.local/bin 在 PATH)");
            return;
        }
        log("info", QString("→ 调 %1 ght --no-check").arg(ac_binary_));
        qint64 pid = 0;
        if (QProcess::startDetached(ac_binary_, QStringList() << "ght" << "--no-check", QDir::homePath(), &pid)) {
            log("ok", QString("✓ Token 管理已启动, pid=%1").arg(pid));
        } else {
            log("err", "✗ 启动 ac ght 失败");
        }
        return;
    }
    if (id == "tts") {
        if (spd_say_binary_.isEmpty()) {
            log("err", "✗ spd-say 未找到, TTS 不可用 (apt install speech-dispatcher?)");
            return;
        }
        log("info", QString("→ 调 %1").arg(spd_say_binary_));
        QProcess::startDetached(spd_say_binary_, QStringList() << "ab 工具链就绪");
        return;
    }
    if (id == "bak") {
        if (bak_binary_.isEmpty()) {
            log("err", "✗ bak 未找到, 备份功能不可用");
            return;
        }
        log("info", QString("→ 调 %1 %2").arg(bak_binary_).arg(cfg_.cwd));
        QProcess::startDetached(bak_binary_, QStringList() << cfg_.cwd);
        return;
    }
    if (id == "proxy_test") {
        // 调 ac ght-cli --proxy-test, 检测 GitHub API 代理是否通
        if (ac_binary_.isEmpty()) {
            log("err", "✗ ac 未找到, 没法跑代理测试");
            return;
        }
        log("info", QString("→ 调 %1 ght-cli --proxy-test").arg(ac_binary_));
        QProcess::startDetached(ac_binary_, QStringList() << "ght-cli" << "--proxy-test", QDir::homePath());
        return;
    }
    if (id == "diag") { runTaskByName("diag"); return; }
    if (id == "view-log") { runTaskByName("view-log"); return; }
    if (id == "build+deploy") { runTaskByName("build+deploy"); return; }

    // 通用: 找 button/toolbar 定义, 按 task / cmd 跑
    for (const auto& b : cfg_.toolbar) {
        if (b.id == id) {
            if (!b.task.isEmpty()) runTaskByName(b.task);
            else if (!b.cmd.isEmpty()) runCmd(b.cmd, id);
            return;
        }
    }
    for (const auto& b : cfg_.buttons) {
        if (b.id == id) {
            if (!b.task.isEmpty()) runTaskByName(b.task);
            else if (!b.cmd.isEmpty()) runCmd(b.cmd, id);
            return;
        }
    }
    // 2026-09-02: menus 也支持 task/cmd 字段
    for (const auto& md : cfg_.menus) {
        for (const auto& mi : md.items) {
            if (mi.id == id) {
                if (!mi.task.isEmpty()) runTaskByName(mi.task);
                else if (!mi.cmd.isEmpty()) runCmd(mi.cmd, id);
                else log("warn", QString("菜单项 %1 没有 task/cmd").arg(id));
                return;
            }
        }
    }
    log("warn", QString("未定义的动作: %1").arg(id));
}

void AbMainWindow::onRunSelectedTask() {
    if (!task_list_) return;
    auto* it = task_list_->currentItem();
    if (!it) {
        log("warn", "没选中任务");
        return;
    }
    runTaskByName(it->text(0));
}

void AbMainWindow::onRunAuto() {
    if (cfg_.auto_chain.isEmpty()) {
        log("warn", "auto 链为空 (在 ai_build.json 的 auto 字段定义)");
        return;
    }
    log("task", QString("跑 auto 链: %1").arg(cfg_.auto_chain.join(" → ")));
    auto_queue_ = cfg_.auto_chain;
    auto_index_ = 0;
    runNextInAuto();
}

void AbMainWindow::runNextInAuto() {
    if (auto_index_ >= auto_queue_.size()) {
        log("ok", "auto 链全部完成 ✓");
        return;
    }
    QString name = auto_queue_[auto_index_++];
    log("info", QString("[auto %1/%2] %3").arg(auto_index_).arg(auto_queue_.size()).arg(name));
    runTaskByName(name, [this]() { runNextInAuto(); });
}

QString AbMainWindow::resolveTaskCmd(const QString& task_name) const {
    for (const auto& t : cfg_.tasks) {
        if (t.name == task_name) return t.cmd;
    }
    return QString();
}

void AbMainWindow::runTaskByName(const QString& name, std::function<void()> on_done) {
    if (runner_->isRunning()) {
        log("warn", "已有 task 在跑, 请先停止");
        return;
    }
    QString cmd = resolveTaskCmd(name);
    if (cmd.isEmpty()) {
        log("err", QString("未知 task: %1").arg(name));
        if (on_done) on_done();
        return;
    }
    log("task", QString("▶ 跑 task [%1]: %2").arg(name, cmd));
    current_task_ = name;
    current_on_done_ = on_done;
    prog_label_->setText(QString("当前: %1").arg(name));
    prog_bar_->setVisible(true);
    if (inspector_) inspector_->onTaskStarted(name);
    if (auto a = actions_.value("run_selected")) a->setEnabled(false);
    if (auto a = actions_.value("run_auto"))    a->setEnabled(false);
    if (auto a = actions_.value("stop"))        a->setEnabled(true);
    if (auto b = buttons_.value("run_selected")) b->setEnabled(false);
    if (auto b = buttons_.value("run_auto"))    b->setEnabled(false);
    if (auto b = buttons_.value("stop"))        b->setEnabled(true);
    runner_->run(name, cmd, cfg_.cwd);
}

void AbMainWindow::runCmd(const QString& cmd, const QString& task_name) {
    if (runner_->isRunning()) {
        log("warn", "已有 task 在跑");
        return;
    }
    log("task", QString("▶ 跑 cmd [%1]: %2").arg(task_name, cmd));
    current_task_ = task_name;
    current_on_done_ = nullptr;
    prog_bar_->setVisible(true);
    runner_->run(task_name, cmd, cfg_.cwd);
}

void AbMainWindow::onStop() {
    if (runner_->isRunning()) {
        log("warn", "停止当前 task");
        runner_->stop();
    } else {
        log("info", "没有 task 在跑");
    }
}

void AbMainWindow::onOutput(const QString& task_name, const QString& line) {
    if (line.isEmpty()) return;
    QString ll = line.toLower();
    QString level = "info";
    if (ll.contains("error") || ll.contains("fatal") || ll.contains("failed")) level = "err";
    else if (ll.contains("warning")) level = "warn";
    else if (line.contains("✓") || ll.contains("[ok]")) level = "ok";
    log(level, QString("[%1] %2").arg(task_name, line));
}

void AbMainWindow::onFinished(const QString& task_name, int exit_code, double elapsed) {
    log(exit_code == 0 ? "ok" : "err",
        QString("[%1] 退出码 %2  耗时 %.1fs").arg(task_name).arg(exit_code).arg(elapsed));
    prog_label_->setText(QString("当前: — (上次: %1, rc=%2, %.1fs)").arg(task_name).arg(exit_code).arg(elapsed));
    prog_bar_->setVisible(false);
    if (inspector_) inspector_->onTaskFinished(task_name, exit_code, elapsed);
    if (auto a = actions_.value("run_selected")) a->setEnabled(true);
    if (auto a = actions_.value("run_auto"))    a->setEnabled(true);
    if (auto a = actions_.value("stop"))        a->setEnabled(false);
    if (auto b = buttons_.value("run_selected")) b->setEnabled(true);
    if (auto b = buttons_.value("run_auto"))    b->setEnabled(true);
    if (auto b = buttons_.value("stop"))        b->setEnabled(false);

    // 编译类 task 成功 → 找 cloud_main
    if (exit_code == 0 && (task_name == "build+deploy" || task_name == "build-only")) {
        if (!cfg_.run_after_build.binary_path.isEmpty()) {
            QString bin = findRunBinary();
            if (!bin.isEmpty()) {
                cloud_binary_ = bin;
                enableRunCloudButton(true);
                log("ok", QString("✓ %1 已就绪").arg(QFileInfo(bin).fileName()));
                // auto_run 模式: 立即跑
                if (cfg_.run_after_build.auto_run) {
                    QTimer::singleShot(500, this, &AbMainWindow::onRunCloud);
                }
            }
        }
    }

    if (current_on_done_) {
        auto cb = current_on_done_;
        current_on_done_ = nullptr;
        cb();
    }
}

void AbMainWindow::onError(const QString& task_name, int err) {
    log("err", QString("[%1] QProcess 错误码 %2").arg(task_name).arg(err));
}

QString AbMainWindow::findRunBinary() const {
    if (cfg_.run_after_build.binary_path.isEmpty()) return QString();
    // binary_path 可能是 "bin/Debug/cloud_main" 或绝对路径
    QFileInfo fi(cfg_.run_after_build.binary_path);
    if (fi.isAbsolute() && fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    QString rel = fi.fileName();
    for (const QString& sub : {"Debug", "Release"}) {
        QString p = cfg_.cwd + "/bin/" + sub + "/" + rel;
        QFileInfo fi2(p);
        if (fi2.exists() && fi2.isExecutable()) return fi2.absoluteFilePath();
    }
    return QString();
}

QString AbMainWindow::findTool(const QString& name) const {
    // which-like: 找 name 的绝对路径. 找不到返回 ''.
    // 1. PATH 探测
    QByteArray path_env = qgetenv("PATH");
    if (!path_env.isEmpty()) {
        const QStringList dirs = QString::fromLocal8Bit(path_env).split(':', Qt::SkipEmptyParts);
        for (const QString& d : dirs) {
            QString cand = d + '/' + name;
            QFileInfo fi(cand);
            if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
        }
    }
    // 2. 常见 PATH 兜底 (桌面 GUI 经常没有 ~/.local/bin)
    const QString home = QDir::homePath();
    const QStringList fallbacks = {
        home + "/.local/bin/" + name,
        "/usr/local/bin/" + name,
        "/usr/bin/" + name,
        "/bin/" + name,
    };
    for (const QString& cand : fallbacks) {
        QFileInfo fi(cand);
        if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    }
    return QString();
}

void AbMainWindow::enableRunCloudButton(bool en) {
    if (auto a = actions_.value("run_cloud")) a->setEnabled(en);
    if (auto b = buttons_.value("run_cloud")) b->setEnabled(en);
}

void AbMainWindow::onRunCloud() {
    if (cloud_binary_.isEmpty()) {
        cloud_binary_ = findRunBinary();
        if (cloud_binary_.isEmpty()) {
            QMessageBox::warning(this, "未找到",
                QString("%1 不存在, 请先跑 build+deploy 编译").arg(cfg_.run_after_build.binary_path));
            return;
        }
    }
    log("task", QString("🚀 启动 %1 (后台)").arg(QFileInfo(cloud_binary_).fileName()));
    QStringList args = cfg_.run_after_build.args;
    // cloud_main 是长跑 GUI 程序, 用 startDetached 后台跑, 不阻塞 ab GUI.
    // 早期版本用 p->start + waitForFinished(-1) 会卡住 GUI, 而且 cloud_main 退出时
    // 才记一条 rc, 中间用户看不到任何 stderr. 这里改成 detached, 立刻 rc=0 表示启动成功.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/nvidia_icd.json");
    qint64 pid = 0;
    bool ok = QProcess::startDetached(cloud_binary_, args, cfg_.cwd, &pid);
    if (ok) {
        log("ok", QString("✓ %1 已在后台启动, pid=%2").arg(QFileInfo(cloud_binary_).fileName()).arg(pid));
        log("info", QString("  stdout/stderr 直接进自己的终端/日志文件, 不进 ab 日志 dock"));
        log("info", QString("  停止: kill %1  或  pkill -f %2").arg(pid).arg(QFileInfo(cloud_binary_).fileName()));
    } else {
        log("err", QString("✗ 启动失败: %1").arg(cloud_binary_));
    }
}

void AbMainWindow::onBuildAndRun() {
    log("task", "⚡ 编译并启动");
    runTaskByName("build+deploy", [this]() {
        if (!cloud_binary_.isEmpty()) onRunCloud();
    });
}

void AbMainWindow::onToggleTheme() {
    // dark → light, light → dark
    auto cur = AbTheme::current();
    AbTheme::Kind nxt = (cur == AbTheme::Dark) ? AbTheme::Light : AbTheme::Dark;
    AbTheme::apply(static_cast<int>(nxt));
    log("ok", QString("主题切换: %1").arg(nxt == AbTheme::Dark ? "暗色" : "浅色"));
}

void AbMainWindow::onToggleLogDock(bool checked) {
    if (log_dock_) log_dock_->setVisible(checked);
}

void AbMainWindow::onAbout() {
    QMessageBox::information(this, "关于 ab",
        QString("ab — AI Build 编译/调试 GUI (C++ Qt5/6)\n"
                "版本: 1.0.0\n"
                "项目: %1\n"
                "任务: %2 个\n"
                "主题: %3\n\n"
                "ab 是通用的 AI 辅助构建工具,\n"
                "通过 ai_build.json 配置文件驱动 UI.\n"
                "想改按钮/菜单, 改 ai_build.json 的 ui 段即可.")
        .arg(cfg_.cwd)
        .arg(static_cast<int>(cfg_.tasks.size()))
        .arg(cfg_.theme));
}

void AbMainWindow::onQuit() {
    close();
}

void AbMainWindow::log(const QString& level, const QString& msg) {
    if (log_dock_) log_dock_->log(level, msg);
    // 同步状态栏
    if (level == "err") {
        if (sb_left_) {
            sb_left_->setText(QString("✗ %1").arg(msg));
            sb_left_->setProperty("level", "err");
            sb_left_->style()->unpolish(sb_left_);
            sb_left_->style()->polish(sb_left_);
        }
    } else if (level == "ok") {
        if (sb_left_) {
            sb_left_->setText(QString("✓ %1").arg(msg));
            sb_left_->setProperty("level", "ok");
            sb_left_->style()->unpolish(sb_left_);
            sb_left_->style()->polish(sb_left_);
        }
    }
}

void AbMainWindow::reloadConfig() {
    // TODO: 重读 ai_build.json, 重建 UI
}

}  // namespace ab
