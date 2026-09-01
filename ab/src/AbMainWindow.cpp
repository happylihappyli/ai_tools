// AbMainWindow.cpp
#include "AbMainWindow.h"
#include "AbTaskRunner.h"
#include "AbLogDock.h"
#include "AbTheme.h"

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
    buildMenus();
    buildToolbar();
    buildMainButtons();
    if (cfg_.show_log_dock) {
        log_dock_ = new AbLogDock(this);
        addDockWidget(Qt::BottomDockWidgetArea, log_dock_);
    }
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
    if (id == "toggle_log")   {
        if (log_dock_) log_dock_->setVisible(!log_dock_->isVisible());
        return;
    }
    if (id == "about")        { onAbout(); return; }
    if (id == "quit")         { onQuit(); return; }
    if (id == "open_ght")     {
        // 跨进程: 调 ac ght
        QProcess::startDetached("ac", QStringList() << "ght" << "--no-check");
        return;
    }
    if (id == "tts") {
        QProcess::startDetached("spd-say", QStringList() << "ab 工具链就绪");
        return;
    }
    if (id == "bak") {
        QProcess::startDetached("bak", QStringList() << cfg_.cwd);
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
    log("task", QString("🚀 启动 %1").arg(QFileInfo(cloud_binary_).fileName()));
    QStringList args = cfg_.run_after_build.args;
    QProcess* p = new QProcess(this);
    // 用 QProcessEnvironment 而不是 setProcessEnvironment(QStringList)
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/nvidia_icd.json");
    p->setProcessEnvironment(env);
    p->start(cloud_binary_, args);
    p->waitForFinished(-1);
    log(p->exitCode() == 0 ? "ok" : "err",
        QString("cloud_main 退出 rc=%1").arg(p->exitCode()));
    p->deleteLater();
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
