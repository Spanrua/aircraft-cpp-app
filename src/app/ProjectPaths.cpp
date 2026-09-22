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
        if (QFileInfo::exists(current.filePath("CMakeLists.txt"))
            && QFileInfo::exists(current.filePath("cpp_app.pro"))
            && QFileInfo::exists(current.filePath("src/main.cpp"))) {
            return current.absolutePath();
        }
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

QString ProjectPaths::cppAppRoot()
{
    const QString root = workspaceRoot();
    if (QFileInfo::exists(QDir(root).filePath("src/main.cpp"))) {
        return root;
    }
    const QString nested = QDir(root).filePath("cpp_app");
    if (QFileInfo::exists(QDir(nested).filePath("src/main.cpp"))) {
        return QDir(nested).absolutePath();
    }
    return root;
}

QString ProjectPaths::aircraftProjectRoot()
{
    QDir root(cppAppRoot());
    if (root.dirName() == QStringLiteral("cpp_app") && root.cdUp()) {
        return root.absolutePath();
    }
    return cppAppRoot();
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

QString ProjectPaths::preprocessRunsDirectory()
{
    return QDir(samplesDirectory()).filePath("preprocess_runs");
}

QString ProjectPaths::preprocessProfilesDirectory()
{
    return QDir(stateDirectory()).filePath("preprocess_profiles");
}

QString ProjectPaths::modelsDirectory()
{
    return QDir(dataDirectory()).filePath("models");
}

QString ProjectPaths::exportsDirectory()
{
    return QDir(dataDirectory()).filePath("exports");
}

QString ProjectPaths::trainingCacheDirectory()
{
    return QDir(dataDirectory()).filePath("cache/training");
}

bool ProjectPaths::ensureRuntimeDirectories(QString *errorMessage)
{
    const QStringList paths = {
        dataDirectory(),
        stateDirectory(),
        samplesDirectory(),
        preprocessRunsDirectory(),
        preprocessProfilesDirectory(),
        modelsDirectory(),
        exportsDirectory(),
        trainingCacheDirectory(),
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
