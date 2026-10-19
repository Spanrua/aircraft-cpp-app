#include "services/AppStateStore.h"

#include "app/ProjectPaths.h"

#include <QFileInfo>
#include <QDateTime>
#include <QJsonDocument>
#include <QDir>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

#include <memory>

namespace
{
struct CachedState
{
    qint64 size = -1;
    QDateTime lastModified;
    std::shared_ptr<AppState> state;
};

QMutex &stateCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, CachedState> &stateCache()
{
    static QHash<QString, CachedState> cache;
    return cache;
}
} // namespace

AppStateStore::AppStateStore(QString stateFilePath)
    : m_stateFilePath(stateFilePath.isEmpty() ? ProjectPaths::stateFilePath() : std::move(stateFilePath))
{
}

bool AppStateStore::save(const AppState &state)
{
    if (m_loadAttempted && m_loadFailed && !m_stateFileMissing) {
        m_lastError = QStringLiteral("状态文件读取失败，拒绝覆盖原文件：%1").arg(m_stateFilePath);
        return false;
    }

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
    {
        QMutexLocker locker(&stateCacheMutex());
        stateCache().remove(QFileInfo(m_stateFilePath).absoluteFilePath());
    }
    return true;
}

std::optional<AppState> AppStateStore::load()
{
    m_loadAttempted = true;
    m_loadFailed = false;
    m_stateFileMissing = false;

    QFile file(m_stateFilePath);
    if (!file.exists()) {
        m_lastError = QStringLiteral("状态文件不存在");
        m_stateFileMissing = true;
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = file.errorString();
        m_loadFailed = true;
        return std::nullopt;
    }

    const QFileInfo fileInfo(file);
    const QString cacheKey = fileInfo.absoluteFilePath();
    {
        QMutexLocker locker(&stateCacheMutex());
        const auto cached = stateCache().constFind(cacheKey);
        if (cached != stateCache().constEnd()
            && cached->state
            && cached->size == fileInfo.size()
            && cached->lastModified == fileInfo.lastModified()) {
            return *cached->state;
        }
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        m_lastError = QStringLiteral("状态文件 JSON 格式无效");
        m_loadFailed = true;
        QMutexLocker locker(&stateCacheMutex());
        stateCache().remove(cacheKey);
        return std::nullopt;
    }

    AppState state = AppState::fromJson(doc.object());
    {
        QMutexLocker locker(&stateCacheMutex());
        stateCache().insert(cacheKey, CachedState{fileInfo.size(), fileInfo.lastModified(),
                                                   std::make_shared<AppState>(state)});
    }
    return state;
}

QString AppStateStore::lastError() const
{
    return m_lastError;
}
