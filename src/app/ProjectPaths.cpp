#include "app/ProjectPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace
{
QString ascendForMarker(QString startPath)
{
    QDir current(startPath);
    for (int depth = 0; depth < 6; ++depth) {
        if (QFileInfo::exists(current.filePath("README.md")) && QFileInfo::exists(current.filePath("requirements.txt"))) {
            return current.absolutePath();
        }
        if (!current.cdUp()) {
            break;
        }
    }
    return QDir(startPath).absolutePath();
}
} // namespace

QString ProjectPaths::repositoryRoot()
{
    const QString applicationDir = QCoreApplication::applicationDirPath();
    return ascendForMarker(applicationDir);
}

QString ProjectPaths::workspaceRoot()
{
    return QDir(repositoryRoot()).absolutePath();
}

QString ProjectPaths::dataDirectory()
{
    return QDir(workspaceRoot()).filePath("data");
}

QString ProjectPaths::stateDirectory()
{
    return QDir(dataDirectory()).filePath("state");
}

QString ProjectPaths::stateFilePath()
{
    return QDir(stateDirectory()).filePath("app_state.json");
}

QString ProjectPaths::samplesDirectory()
{
    return QDir(dataDirectory()).filePath("samples");
}

QString ProjectPaths::modelsDirectory()
{
    return QDir(dataDirectory()).filePath("models");
}

QString ProjectPaths::exportsDirectory()
{
    return QDir(dataDirectory()).filePath("exports");
}

bool ProjectPaths::ensureRuntimeDirectories(QString *errorMessage)
{
    const QStringList paths = {
        dataDirectory(),
        stateDirectory(),
        samplesDirectory(),
        modelsDirectory(),
        exportsDirectory(),
    };

    for (const QString &path : paths) {
        QDir dir;
        if (!dir.mkpath(path)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("无法创建运行时目录：%1").arg(path);
            }
            return false;
        }
    }
    return true;
}
