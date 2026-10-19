#include "services/CeyearIqReceiver.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QAbstractSocket>
#include <QHostAddress>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

namespace
{
constexpr int kMinimumPrefixLength = 28;
constexpr int kUdpReceiveBufferSize = 64 * 1024 * 1024;
constexpr quint32 kMagic = 0x000EB200;
constexpr quint16 kIfPacketHeader = 901;

quint16 readUInt16BE(const QByteArray &bytes, int offset)
{
    return static_cast<quint16>((static_cast<quint8>(bytes.at(offset)) << 8)
                                | static_cast<quint8>(bytes.at(offset + 1)));
}

quint32 readUInt32BE(const QByteArray &bytes, int offset)
{
    return (static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) << 24)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 8)
        | static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3)));
}

int findMagic(const QByteArray &bytes)
{
    for (int i = 0; i + 4 <= bytes.size(); ++i) {
        if (readUInt32BE(bytes, i) == kMagic) {
            return i;
        }
    }
    return -1;
}
} // namespace

CeyearIqReceiver::CeyearIqReceiver(QObject *parent)
    : QObject(parent)
{
}

CeyearIqReceiver::~CeyearIqReceiver()
{
    stop();
}

bool CeyearIqReceiver::start(const CeyearIqReceiverConfig &config, QString *error)
{
    stop();
    m_config = config;
    m_bytesReceived = 0;
    m_sampleCount = 0;
    m_boundPort = 0;
    m_parseBuffer.clear();
    m_targetReachedEmitted = false;
    m_lastByteTimer = QElapsedTimer();

    if (m_config.outputFilePath.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("PC output file path is empty.");
        }
        return false;
    }

    QHostAddress address;
    if (!address.setAddress(m_config.bindAddress.trimmed())) {
        if (error) {
            *error = QStringLiteral("Invalid bind address: %1").arg(m_config.bindAddress);
        }
        return false;
    }
    if (!openOutputFile(error)) {
        return false;
    }

    if (m_config.protocol == CeyearIqReceiveProtocol::Tcp) {
        m_tcpServer = std::make_unique<QTcpServer>();
        connect(m_tcpServer.get(), &QTcpServer::newConnection, this, [this]() {
            while (m_tcpServer && m_tcpServer->hasPendingConnections()) {
                QTcpSocket *socket = m_tcpServer->nextPendingConnection();
                socket->setParent(this);
                m_tcpSocket = socket;
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    appendBytes(socket->readAll());
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
                connect(socket, &QObject::destroyed, this, [this, socket]() {
                    if (m_tcpSocket == socket) {
                        m_tcpSocket.clear();
                    }
                });
            }
        });
        if (!m_tcpServer->listen(address, m_config.port)) {
            if (error) {
                *error = QStringLiteral("TCP listen failed: %1").arg(m_tcpServer->errorString());
            }
            m_tcpServer.reset();
            discardOutputFile();
            return false;
        }
        m_boundPort = m_tcpServer->serverPort();
    } else {
        m_udpSocket = std::make_unique<QUdpSocket>();
        m_udpSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
                                     kUdpReceiveBufferSize);
        connect(m_udpSocket.get(), &QUdpSocket::readyRead, this, [this]() {
            while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
                QByteArray datagram;
                datagram.resize(static_cast<qsizetype>(m_udpSocket->pendingDatagramSize()));
                m_udpSocket->readDatagram(datagram.data(), datagram.size());
                appendBytes(datagram);
            }
        });
        if (!m_udpSocket->bind(address, m_config.port)) {
            if (error) {
                *error = QStringLiteral("UDP bind failed: %1").arg(m_udpSocket->errorString());
            }
            m_udpSocket.reset();
            discardOutputFile();
            return false;
        }
        m_boundPort = m_udpSocket->localPort();
    }

    m_running = true;
    return true;
}

CeyearIqReceiverResult CeyearIqReceiver::waitForFinished(int maxWaitMs)
{
    const bool waitForTarget = m_config.targetSampleCount > 0;
    const int waitMs = maxWaitMs >= 0 ? maxWaitMs : (waitForTarget ? 0 : m_config.timeoutMs);
    if (m_running && (waitForTarget || waitMs > 0)) {
        QEventLoop loop;
        QElapsedTimer waitTimer;
        waitTimer.start();
        QMetaObject::Connection targetConnection;
        QMetaObject::Connection stoppedConnection;
        if (waitForTarget) {
            targetConnection = connect(this, &CeyearIqReceiver::targetSampleCountReached,
                                       &loop, &QEventLoop::quit);
            stoppedConnection = connect(this, &CeyearIqReceiver::stopped,
                                        &loop, &QEventLoop::quit);
        }
        QTimer idleTimer;
        if (waitForTarget && (m_config.dataIdleFinishMs > 0 || m_config.firstByteWaitMs > 0)) {
            idleTimer.setInterval(100);
            connect(&idleTimer, &QTimer::timeout, &loop, [this, &loop, &waitTimer]() {
                if (!m_running
                    || (m_config.targetSampleCount > 0 && m_sampleCount >= m_config.targetSampleCount)) {
                    loop.quit();
                    return;
                }
                if (m_bytesReceived > 0
                    && m_config.dataIdleFinishMs > 0
                    && m_lastByteTimer.isValid()
                    && m_lastByteTimer.elapsed() >= m_config.dataIdleFinishMs) {
                    loop.quit();
                    return;
                }
                if (m_bytesReceived == 0
                    && m_config.firstByteWaitMs > 0
                    && waitTimer.elapsed() >= m_config.firstByteWaitMs) {
                    loop.quit();
                }
            });
            idleTimer.start();
        }
        if (waitMs > 0) {
            QTimer::singleShot(waitMs, &loop, &QEventLoop::quit);
        }
        if (!waitForTarget || m_sampleCount < m_config.targetSampleCount) {
            loop.exec(QEventLoop::AllEvents);
        }
        idleTimer.stop();
        if (targetConnection) {
            disconnect(targetConnection);
        }
        if (stoppedConnection) {
            disconnect(stoppedConnection);
        }
    }

    return stop();
}

CeyearIqReceiverResult CeyearIqReceiver::stop()
{
    const bool wasRunning = m_running;
    if (m_tcpSocket) {
        m_tcpSocket->disconnectFromHost();
        m_tcpSocket.clear();
    }
    if (m_tcpServer) {
        m_tcpServer->close();
        m_tcpServer.reset();
    }
    if (m_udpSocket) {
        m_udpSocket->close();
        m_udpSocket.reset();
    }
    bool fileCommitOk = true;
    if (m_file) {
        fileCommitOk = m_file->flush() && m_file->commit();
        if (!fileCommitOk) {
            m_file->cancelWriting();
        }
        m_file.reset();
    }

    const bool reachedTarget = m_config.targetSampleCount > 0
        && m_sampleCount >= m_config.targetSampleCount;
    const bool hadBytes = m_bytesReceived > 0;
    m_running = false;
    if (wasRunning) {
        emit stopped();
    }
    const bool captureSuccess = (m_config.targetSampleCount > 0 ? reachedTarget : hadBytes)
        && fileCommitOk;
    const QString message = !fileCommitOk
        ? QStringLiteral("Failed to commit PC IQ output file.")
        : reachedTarget
        ? QStringLiteral("3943B target IQ sample count saved to PC.")
        : hadBytes
        ? QStringLiteral("3943B IQ data stream went idle for %1 ms before target sample count was reached.")
              .arg(m_config.dataIdleFinishMs)
        : QStringLiteral("No 3943B IQ bytes were received.");
    return makeResult(captureSuccess, message);
}

quint16 CeyearIqReceiver::boundPort() const
{
    return m_boundPort;
}

qint64 CeyearIqReceiver::bytesReceived() const
{
    return m_bytesReceived;
}

void CeyearIqReceiver::cancel()
{
    stop();
}

bool CeyearIqReceiver::openOutputFile(QString *error)
{
    const QFileInfo info(m_config.outputFilePath);
    QDir dir(info.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("Cannot create output directory: %1").arg(dir.absolutePath());
        }
        return false;
    }

    m_file = std::make_unique<QSaveFile>(m_config.outputFilePath);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Cannot open PC output file: %1").arg(m_file->errorString());
        }
        m_file.reset();
        return false;
    }
    return true;
}

void CeyearIqReceiver::discardOutputFile()
{
    if (!m_file) {
        return;
    }
    m_file->cancelWriting();
    m_file.reset();
}

void CeyearIqReceiver::appendBytes(const QByteArray &bytes)
{
    if (!m_file || bytes.isEmpty()) {
        return;
    }
    const qint64 written = m_file->write(bytes);
    if (written > 0) {
        m_bytesReceived += written;
        m_lastByteTimer.restart();
        parseSampleCount(bytes.left(static_cast<int>(written)));
    }
}

void CeyearIqReceiver::parseSampleCount(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }

    m_parseBuffer.append(bytes);
    while (m_parseBuffer.size() >= kMinimumPrefixLength) {
        const int magicOffset = findMagic(m_parseBuffer);
        if (magicOffset < 0) {
            m_parseBuffer = m_parseBuffer.right(3);
            break;
        }
        if (magicOffset > 0) {
            m_parseBuffer.remove(0, magicOffset);
        }
        if (m_parseBuffer.size() < kMinimumPrefixLength) {
            break;
        }

        const int packetLength = static_cast<int>(readUInt32BE(m_parseBuffer, 12));
        if (packetLength < kMinimumPrefixLength) {
            m_parseBuffer.remove(0, 1);
            continue;
        }
        if (m_parseBuffer.size() < packetLength) {
            break;
        }

        const int packetHeader = static_cast<int>(readUInt16BE(m_parseBuffer, 16));
        if (packetHeader == kIfPacketHeader) {
            const int optionalHeaderLength = static_cast<quint8>(m_parseBuffer.at(23));
            const int payloadLength = packetLength - (kMinimumPrefixLength + optionalHeaderLength);
            if (payloadLength > 0 && payloadLength % 4 == 0) {
                m_sampleCount += payloadLength / 4;
            }
        }
        m_parseBuffer.remove(0, packetLength);
    }

    if (!m_targetReachedEmitted
        && m_config.targetSampleCount > 0
        && m_sampleCount >= m_config.targetSampleCount) {
        m_targetReachedEmitted = true;
        emit targetSampleCountReached();
    }
}

CeyearIqReceiverResult CeyearIqReceiver::makeResult(bool success, const QString &message) const
{
    CeyearIqReceiverResult result;
    result.success = success;
    result.message = message;
    result.outputFilePath = m_config.outputFilePath;
    result.bytesReceived = m_bytesReceived;
    result.sampleCount = m_sampleCount;
    result.boundPort = m_boundPort;
    return result;
}
