#pragma once

#include <QFrame>

class QLabel;

class MetricCard : public QFrame
{
    Q_OBJECT

public:
    explicit MetricCard(const QString &title, QWidget *parent = nullptr);
    void setValue(const QString &value);

private:
    QLabel *m_titleLabel = nullptr;
    QLabel *m_valueLabel = nullptr;
};
