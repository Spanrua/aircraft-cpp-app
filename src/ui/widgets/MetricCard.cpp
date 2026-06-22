#include "ui/widgets/MetricCard.h"

#include <QLabel>
#include <QVBoxLayout>

MetricCard::MetricCard(const QString &title, QWidget *parent)
    : QFrame(parent)
{
    setObjectName("MetricCard");
    setMinimumHeight(86);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);
    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setObjectName(QStringLiteral("MetricTitle"));
    m_valueLabel = new QLabel("-", this);
    m_valueLabel->setObjectName(QStringLiteral("MetricValue"));
    layout->addWidget(m_titleLabel);
    layout->addWidget(m_valueLabel);
}

void MetricCard::setValue(const QString &value)
{
    if (m_valueLabel) {
        m_valueLabel->setText(value);
    }
}
