#include "app/ProjectPaths.h"
#include "ui/AppTheme.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QFont>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("无人机射频信号识别系统"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));

    QString error;
    if (!ProjectPaths::ensureRuntimeDirectories(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("启动失败"), error);
        return 1;
    }
    AppTheme::apply(app);

    MainWindow window;
    window.show();
    return app.exec();
}
