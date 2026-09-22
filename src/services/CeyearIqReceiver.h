#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>

#include <memory>

class QFile;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

enum class CeyearIqReceiveProtocol
{
    Tcp,
    Udp,
};

struct CeyearIqReceiverConfig
{
    CeyearIqReceiveProtocol protocol = CeyearIqReceiveProtocol::Tcp;
    QString bindAddress = QStringLiteral("0.0.0.0");
    quint16 port = 5030;
    QString outputFilePath;
    int timeoutMs = 5000;
    int targetSampleCount = 0;
    int dataIdleFinishMs = 300;
    int firstByteWaitMs = 3000;
};

struct CeyearIqReceiverResult
{
    bool success = false;
    QString message;
    QString outputFilePath;
    qint64 bytesReceived = 0;
    qint64 sampleCount = 0;
    quint16 boundPort = 0;
};

class CeyearIqReceiver : public QObject
{
    Q_OBJECT

public:
    explicit CeyearIqReceiver(QObject *parent = nullptr);
    ~CeyearIqReceiver() override;

    bool start(const CeyearIqReceiverConfig &config, QString *error = nullptr);
    CeyearIqReceiverResult waitForFinished(int maxWaitMs = -1);
    CeyearIqReceiverResult stop();
    quint16 boundPort() const;
    qint64 bytesReceived() const;

public slots:
    void cancel();

signals:
    void targetSampleCountReached();
    void stopped();

private:
    bool openOutputFile(QString *error);
    void appendBytes(const QByteArray &bytes);
    void parseSampleCount(const QByteArray &bytes);
    CeyearIqReceiverResult makeResult(bool success, const QString &message) const;

    CeyearIqReceiverConfig m_config;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QTcpServer> m_tcpServer;
    QPointer<QTcpSocket> m_tcpSocket;
    std::unique_ptr<QUdpSocket> m_udpSocket;
    QByteArray m_parseBuffer;
    qint64 m_bytesReceived = 0;
    qint64 m_sampleCount = 0;
    QElapsedTimer m_lastByteTimer;
    quint16 m_boundPort = 0;
    bool m_running = false;
    bool m_targetReachedEmitted = false;
};
