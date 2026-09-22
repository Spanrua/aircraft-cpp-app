#pragma once

#include "services/CeyearPscSweep.h"

#include <QObject>
#include <QString>

#include <memory>

class QUdpSocket;

struct CeyearPscReceiverConfig
{
    QString bindAddress = QStringLiteral("0.0.0.0");
    quint16 port = 19712;
};

class CeyearPscReceiver : public QObject
{
    Q_OBJECT

public:
    explicit CeyearPscReceiver(QObject *parent = nullptr);
    ~CeyearPscReceiver() override;

    bool start(const CeyearPscReceiverConfig &config, QString *error = nullptr);
    void stop();
    quint16 boundPort() const;
    qint64 bytesReceived() const;
    qint64 datagramCount() const;

signals:
    void framesReceived(const QVector<CeyearPscFrame> &frames);
    void datagramReceived(qint64 bytesReceived, qint64 datagramCount);

private:
    void readPendingDatagrams();

    CeyearPscReceiverConfig m_config;
    std::unique_ptr<QUdpSocket> m_udpSocket;
    qint64 m_bytesReceived = 0;
    qint64 m_datagramCount = 0;
    quint16 m_boundPort = 0;
};
