#include "services/AppStateStore.h"

#include "app/ProjectPaths.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QDir>
#include <QSaveFile>

AppStateStore::AppStateStore(QString stateFilePath)
    : m_stateFilePath(stateFilePath.isEmpty() ? ProjectPaths::stateFilePath() : std::move(stateFilePath))
{
}

bool AppStateStore::save(const AppState &state)
{
    QSaveFile file(m_stateFilePath);
    const QString parentDir = QFileInfo(m_stateFilePath).absolutePath();
    if (!QDir().mkpath(parentDir)) {
        m_lastError = QStringLiteral("无法创建状态目录：%1").arg(parentDir);
        return false;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = file.errorString();
        return false;
    }
    const QJsonDocument doc(state.toJson());
    if (file.write(doc.toJson(QJsonDocument::Indented)) < 0) {
        m_lastError = file.errorString();
        return false;
    }
    if (!file.commit()) {
        m_lastError = file.errorString();
        return false;
    }
    return true;
}

std::optional<AppState> AppStateStore::load()
{
    QFile file(m_stateFilePath);
    if (!file.exists()) {
        m_lastError = QStringLiteral("状态文件不存在");
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = file.errorString();
        return std::nullopt;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        m_lastError = QStringLiteral("状态文件 JSON 格式无效");
        return std::nullopt;
    }
    return AppState::fromJson(doc.object());
}

QString AppStateStore::lastError() const
{
    return m_lastError;
}
