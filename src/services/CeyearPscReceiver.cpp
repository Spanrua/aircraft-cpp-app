#include "services/CeyearPscReceiver.h"

#include <QHostAddress>
#include <QUdpSocket>

CeyearPscReceiver::CeyearPscReceiver(QObject *parent)
    : QObject(parent)
{
}

CeyearPscReceiver::~CeyearPscReceiver()
{
    stop();
}

bool CeyearPscReceiver::start(const CeyearPscReceiverConfig &config, QString *error)
{
    stop();
    m_config = config;
    m_bytesReceived = 0;
    m_datagramCount = 0;
    m_boundPort = 0;

    QHostAddress address;
    if (!address.setAddress(m_config.bindAddress.trimmed())) {
        if (error) {
            *error = QStringLiteral("Invalid PSC UDP bind address: %1").arg(m_config.bindAddress);
        }
        return false;
    }

    m_udpSocket = std::make_unique<QUdpSocket>();
    connect(m_udpSocket.get(), &QUdpSocket::readyRead, this, &CeyearPscReceiver::readPendingDatagrams);
    if (!m_udpSocket->bind(address,
                           m_config.port,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        if (error) {
            *error = QStringLiteral("PSC UDP bind failed on %1:%2: %3")
                         .arg(m_config.bindAddress)
                         .arg(m_config.port)
                         .arg(m_udpSocket->errorString());
        }
        m_udpSocket.reset();
        return false;
    }

    m_boundPort = m_udpSocket->localPort();
    return true;
}

void CeyearPscReceiver::stop()
{
    if (m_udpSocket) {
        m_udpSocket->close();
        m_udpSocket.reset();
    }
    m_boundPort = 0;
}

quint16 CeyearPscReceiver::boundPort() const
{
    return m_boundPort;
}

qint64 CeyearPscReceiver::bytesReceived() const
{
    return m_bytesReceived;
}

qint64 CeyearPscReceiver::datagramCount() const
{
    return m_datagramCount;
}

void CeyearPscReceiver::readPendingDatagrams()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<qsizetype>(m_udpSocket->pendingDatagramSize()));
        const qint64 read = m_udpSocket->readDatagram(datagram.data(), datagram.size());
        if (read <= 0) {
            continue;
        }

        m_bytesReceived += read;
        ++m_datagramCount;
        emit datagramReceived(m_bytesReceived, m_datagramCount);

        const QVector<CeyearPscFrame> frames = CeyearPscPacketParser::parse(datagram);
        if (!frames.isEmpty()) {
            emit framesReceived(frames);
        }
    }
}
