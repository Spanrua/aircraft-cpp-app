#pragma once

#include <QString>

class ProjectPaths
{
public:
    static QString repositoryRoot();
    static QString workspaceRoot();
    static QString cppAppRoot();
    static QString aircraftProjectRoot();
    static QString dataDirectory();
    static QString stateDirectory();
    static QString stateFilePath();
    static QString samplesDirectory();
    static QString preprocessRunsDirectory();
    static QString preprocessProfilesDirectory();
    static QString modelsDirectory();
    static QString signalNoiseModelDirectory();
    static QString signalCleaningRunsDirectory();
    static QString exportsDirectory();
    static QString trainingCacheDirectory();
    static bool ensureRuntimeDirectories(QString *errorMessage = nullptr);
};
