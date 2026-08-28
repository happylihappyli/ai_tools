#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QProcess>
#include <QTreeWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QPushButton>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString &cwd = "", const QString &initCmd = "", const QString &testCmd = "", QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onExecuteClicked();
    void onTestClicked();
    void onStopClicked();
    void onSaveConfigClicked();
    void onProcessReadyReadStandardOutput();
    void onProcessReadyReadStandardError();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void setupUi();
    void logMessage(const QString &msg, const QColor &color);
    void updateProgress(const QString &outputLine);

    QLineEdit *cmdInput;
    QLineEdit *testCmdInput;
    QLineEdit *taskInput;
    QPushButton *runBtn;
    QPushButton *testBtn;
    QPushButton *stopBtn;
    
    QTextEdit *logView;
    QTreeWidget *taskTree;
    QGraphicsView *graphView;
    QGraphicsScene *graphScene;
    
    QProgressBar *progressBar;
    QLabel *statusLabel;
    
    QProcess *compileProcess;
    QString currentWorkingDir;
};

#endif // MAIN_WINDOW_H
