#pragma once

#include "core/Records.h"

#include <optional>

#include <QString>

class AppStateStore
{
public:
    explicit AppStateStore(QString stateFilePath = QString());

    bool save(const AppState &state);
    std::optional<AppState> load();
    QString lastError() const;

private:
    QString m_stateFilePath;
    QString m_lastError;
};

