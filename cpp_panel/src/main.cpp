#include "main_window.h"
#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    QCommandLineParser parser;
    parser.setApplicationDescription("C++ Expert Build Tool");
    parser.addHelpOption();
    
    QCommandLineOption cwdOption(QStringList() << "d" << "cwd", "Set working directory.", "directory");
    parser.addOption(cwdOption);
    
    QCommandLineOption cmdOption(QStringList() << "c" << "cmd", "Set initial command.", "command");
    parser.addOption(cmdOption);
    
    QCommandLineOption testOption(QStringList() << "t" << "test", "Set test command.", "test_command");
    parser.addOption(testOption);
    
    parser.process(app);
    
    QString cwd = parser.value(cwdOption);
    QString initCmd = parser.value(cmdOption);
    QString testCmd = parser.value(testOption);
    
    MainWindow window(cwd, initCmd, testCmd);
    window.show();
    
    return app.exec();
}
