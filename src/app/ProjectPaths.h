#pragma once

#include <QString>

class ProjectPaths
{
public:
    static QString repositoryRoot();
    static QString workspaceRoot();
    static QString dataDirectory();
    static QString stateDirectory();
    static QString stateFilePath();
    static QString samplesDirectory();
    static QString modelsDirectory();
    static QString exportsDirectory();
    static bool ensureRuntimeDirectories(QString *errorMessage = nullptr);
};
