#include "main_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QScrollBar>
#include <QTabWidget>
#include <QSplitter>

#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QMessageBox>

MainWindow::MainWindow(const QString &cwd, const QString &initCmd, const QString &testCmd, QWidget *parent) 
    : QMainWindow(parent), compileProcess(new QProcess(this)) {
    // 1. 设置置顶 (topmost)
    setWindowFlags(Qt::WindowStaysOnTopHint);

    currentWorkingDir = cwd.isEmpty() ? QDir::currentPath() : cwd;
    setWindowTitle("C++ 专家编译工具 - " + currentWorkingDir);

    setupUi();

    if (!initCmd.isEmpty()) {
        cmdInput->setText(initCmd);
    }
    if (!testCmd.isEmpty()) {
        testCmdInput->setText(testCmd);
    }

    connect(compileProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onProcessReadyReadStandardOutput);
    connect(compileProcess, &QProcess::readyReadStandardError, this, &MainWindow::onProcessReadyReadStandardError);
    connect(compileProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onProcessFinished);
}

MainWindow::~MainWindow() {}

void MainWindow::setupUi() {
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(splitter);

    // ============ 左侧：任务管理 ============
    QWidget *leftPanel = new QWidget(this);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    
    QLabel *dashTitle = new QLabel("🧭 驾驶舱", this);
    dashTitle->setStyleSheet("font-weight: bold; color: #0d9488;");
    leftLayout->addWidget(dashTitle);

    QTabWidget *tabs = new QTabWidget(this);
    
    // 图形视图
    graphView = new QGraphicsView(this);
    graphScene = new QGraphicsScene(this);
    graphView->setScene(graphScene);
    graphView->setBackgroundBrush(QBrush(QColor("#1e1e2e")));
    tabs->addTab(graphView, "关系图(构建中)");
    
    // 列表视图
    taskTree = new QTreeWidget(this);
    taskTree->setHeaderHidden(true);
    tabs->addTab(taskTree, "列表");
    
    leftLayout->addWidget(tabs);
    splitter->addWidget(leftPanel);

    // ============ 右侧：编译控制 ============
    QWidget *rightPanel = new QWidget(this);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);

    // 控制区
    QHBoxLayout *cmdLayout = new QHBoxLayout();
    cmdLayout->addWidget(new QLabel("🛠 命令:", this));
    cmdInput = new QLineEdit(this);
    cmdInput->setText("scons platform=linuxbsd -j8");
    cmdLayout->addWidget(cmdInput);
    rightLayout->addLayout(cmdLayout);

    QHBoxLayout *taskLayout = new QHBoxLayout();
    taskLayout->addWidget(new QLabel("📝 任务:", this));
    taskInput = new QLineEdit(this);
    taskInput->setPlaceholderText("输入本次编译的任务目标...");
    taskLayout->addWidget(taskInput);
    rightLayout->addLayout(taskLayout);

    QHBoxLayout *testLayout = new QHBoxLayout();
    testLayout->addWidget(new QLabel("🧪 测试:", this));
    testCmdInput = new QLineEdit(this);
    testCmdInput->setPlaceholderText("输入测试命令 (例如: ./run_demo_local.sh)");
    testLayout->addWidget(testCmdInput);
    rightLayout->addLayout(testLayout);

    // 按钮区
    QHBoxLayout *btnLayout = new QHBoxLayout();
    runBtn = new QPushButton("⚡ 执行编译", this);
    runBtn->setStyleSheet("background-color: #0d9488; font-weight: bold; padding: 6px;");
    testBtn = new QPushButton("🧪 运行测试", this);
    testBtn->setStyleSheet("background-color: #3b82f6; font-weight: bold; padding: 6px;");
    
    QPushButton *saveBtn = new QPushButton("💾 保存配置", this);
    saveBtn->setStyleSheet("background-color: #6366f1; font-weight: bold; padding: 6px;");
    
    stopBtn = new QPushButton("🛑 停止", this);
    stopBtn->setEnabled(false);
    btnLayout->addWidget(runBtn);
    btnLayout->addWidget(testBtn);
    btnLayout->addWidget(saveBtn);
    btnLayout->addWidget(stopBtn);
    rightLayout->addLayout(btnLayout);

    connect(runBtn, &QPushButton::clicked, this, &MainWindow::onExecuteClicked);
    connect(testBtn, &QPushButton::clicked, this, &MainWindow::onTestClicked);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveConfigClicked);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStopClicked);

    // 进度条
    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressBar->setAlignment(Qt::AlignCenter);
    rightLayout->addWidget(progressBar);

    // 日志区
    logView = new QTextEdit(this);
    logView->setReadOnly(true);
    logView->setStyleSheet("background-color: #1e1e2e; color: #e0e0e0; font-family: monospace;");
    rightLayout->addWidget(logView);

    // 状态栏
    statusLabel = new QLabel("Ready", this);
    statusLabel->setStyleSheet("color: #888;");
    rightLayout->addWidget(statusLabel);

    splitter->addWidget(rightPanel);
    splitter->setSizes(QList<int>() << 300 << 600);

    resize(900, 600);
    setStyleSheet("background-color: #1e1e2e; color: #e0e0e0;");
}

void MainWindow::onExecuteClicked() {
    QString cmd = cmdInput->text().trimmed();
    QString task = taskInput->text().trimmed();
    if (cmd.isEmpty()) return;

    if (compileProcess->state() != QProcess::NotRunning) {
        return;
    }

    // 更新UI状态
    runBtn->setEnabled(false);
    testBtn->setEnabled(false);
    stopBtn->setEnabled(true);
    progressBar->setValue(0);
    logView->clear();
    statusLabel->setText("编译中...");

    if (!task.isEmpty()) {
        QTreeWidgetItem *taskItem = new QTreeWidgetItem(taskTree);
        taskItem->setText(0, "🚀 " + task);
        taskTree->addTopLevelItem(taskItem);
        taskTree->scrollToBottom();
    }

    logMessage("🛠 启动编译: " + cmd, Qt::white);
    logMessage("📂 工作目录: " + currentWorkingDir, Qt::lightGray);
    
    // 运行
    compileProcess->setWorkingDirectory(currentWorkingDir);
    // 简单的按空格拆分（暂不支持复杂引号参数）
    QStringList args = cmd.split(" ", Qt::SkipEmptyParts);
    QString program = args.takeFirst();
    compileProcess->start(program, args);
}

void MainWindow::onTestClicked() {
    QString cmd = testCmdInput->text().trimmed();
    if (cmd.isEmpty()) return;

    if (compileProcess->state() != QProcess::NotRunning) {
        return;
    }

    // 更新UI状态
    runBtn->setEnabled(false);
    testBtn->setEnabled(false);
    stopBtn->setEnabled(true);
    progressBar->setValue(0);
    logView->clear();
    statusLabel->setText("运行测试中...");

    logMessage("🧪 启动测试: " + cmd, Qt::cyan);
    logMessage("📂 工作目录: " + currentWorkingDir, Qt::lightGray);
    
    // 运行
    compileProcess->setWorkingDirectory(currentWorkingDir);
    QStringList args = cmd.split(" ", Qt::SkipEmptyParts);
    QString program = args.takeFirst();
    compileProcess->start(program, args);
}

void MainWindow::onStopClicked() {
    if (compileProcess->state() != QProcess::NotRunning) {
        compileProcess->kill();
        logMessage("🛑 编译已由用户停止。", Qt::yellow);
        statusLabel->setText("已停止");
    }
}

void MainWindow::onSaveConfigClicked() {
    QString configPath = QDir(currentWorkingDir).filePath("ai_build.json");
    
    QJsonObject jsonObj;
    jsonObj["cwd"] = currentWorkingDir;
    jsonObj["cmd"] = cmdInput->text().trimmed();
    jsonObj["test"] = testCmdInput->text().trimmed();
    
    QJsonDocument doc(jsonObj);
    QFile file(configPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();
        logMessage("💾 配置已保存到: " + configPath, Qt::green);
        QMessageBox::information(this, "成功", "配置已保存成功！");
    } else {
        logMessage("❌ 无法保存配置到: " + configPath, Qt::red);
        QMessageBox::critical(this, "错误", "无法保存配置文件，请检查权限。");
    }
}

void MainWindow::updateProgress(const QString &outputLine) {
    // 简单的进度模拟：如果是 scons 的输出，匹配 [N/M] 或文件编译行
    if (outputLine.contains("Compiling") || outputLine.contains("Building") || outputLine.contains("Linking")) {
        int cur = progressBar->value();
        if (cur < 95) {
            progressBar->setValue(cur + 1); // 粗略递增
        }
    } else if (outputLine.contains("done building targets")) {
        progressBar->setValue(100);
    }
}

void MainWindow::onProcessReadyReadStandardOutput() {
    QString out = QString::fromUtf8(compileProcess->readAllStandardOutput());
    for (const QString &line : out.split("\n", Qt::SkipEmptyParts)) {
        updateProgress(line);
        logMessage(line, Qt::white);
    }
}

void MainWindow::onProcessReadyReadStandardError() {
    QString err = QString::fromUtf8(compileProcess->readAllStandardError());
    for (const QString &line : err.split("\n", Qt::SkipEmptyParts)) {
        if (line.contains("error", Qt::CaseInsensitive)) {
            logMessage(line, QColor("#ff6b6b"));
        } else if (line.contains("warning", Qt::CaseInsensitive)) {
            logMessage(line, QColor("#fbbf24"));
        } else {
            // stderr 中的其他内容（如警告上下文或备注）默认使用灰色或黄色，而不是红色
            logMessage(line, QColor("#cbd5e1"));
        }
    }
}

void MainWindow::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    runBtn->setEnabled(true);
    testBtn->setEnabled(true);
    stopBtn->setEnabled(false);

    if (exitStatus == QProcess::CrashExit) {
        logMessage("❌ 编译崩溃", QColor("#ff6b6b"));
        statusLabel->setText("崩溃退出");
    } else if (exitCode == 0) {
        logMessage("✅ 编译成功", QColor("#34d399"));
        progressBar->setValue(100);
        statusLabel->setText("编译成功");
        
        if (taskTree->topLevelItemCount() > 0) {
            QTreeWidgetItem *last = taskTree->topLevelItem(taskTree->topLevelItemCount() - 1);
            QString txt = last->text(0);
            txt.replace("🚀", "✅");
            last->setText(0, txt);
        }
    } else {
        logMessage("❌ 编译失败 (code " + QString::number(exitCode) + ")", QColor("#ff6b6b"));
        statusLabel->setText("编译失败");
        if (taskTree->topLevelItemCount() > 0) {
            QTreeWidgetItem *last = taskTree->topLevelItem(taskTree->topLevelItemCount() - 1);
            QString txt = last->text(0);
            txt.replace("🚀", "❌");
            last->setText(0, txt);
        }
    }
}

void MainWindow::logMessage(const QString &msg, const QColor &color) {
    if (msg.trimmed().isEmpty()) return;
    logView->setTextColor(color);
    logView->append(msg.trimmed());
    logView->verticalScrollBar()->setValue(logView->verticalScrollBar()->maximum());
}
