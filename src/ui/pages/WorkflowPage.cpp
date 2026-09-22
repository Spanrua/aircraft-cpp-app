#include "ui/pages/WorkflowPage.h"

#include <QScrollArea>
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>

WorkflowPage::WorkflowPage(const QString &title, const QString &subtitle, QWidget *parent)
    : WorkflowPage(title, subtitle, parent, false)
{
}

WorkflowPage::WorkflowPage(const QString &title, const QString &subtitle, QWidget *parent, bool scrollableContent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 18);
    root->setSpacing(10);

    auto *header = new QVBoxLayout();
    header->setSpacing(4);
    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setObjectName(QStringLiteral("PageTitle"));
    m_subtitleLabel = new QLabel(subtitle, this);
    m_subtitleLabel->setObjectName(QStringLiteral("PageSubtitle"));
    m_subtitleLabel->setWordWrap(true);
    header->addWidget(m_titleLabel);
    header->addWidget(m_subtitleLabel);
    root->addLayout(header);

    m_contentLayout = new QVBoxLayout();
    m_contentLayout->setSpacing(11);

    if (scrollableContent) {
        m_scrollArea = new QScrollArea(this);
        m_scrollArea->setObjectName(QStringLiteral("WorkflowPageScrollArea"));
        m_scrollArea->setWidgetResizable(true);
        m_scrollArea->setMinimumSize(0, 0);
        m_scrollArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_scrollArea->setFrameShape(QFrame::NoFrame);

        m_contentWidget = new QWidget(m_scrollArea);
        m_contentWidget->setObjectName(QStringLiteral("WorkflowPageScrollContent"));
        m_contentWidget->setMinimumWidth(0);
        m_contentWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_contentWidget->setLayout(m_contentLayout);
        m_scrollArea->setWidget(m_contentWidget);
        root->addWidget(m_scrollArea, 1);
    } else {
        root->addLayout(m_contentLayout, 1);
    }
}

QVBoxLayout *WorkflowPage::contentLayout() const
{
    return m_contentLayout;
}

QLabel *WorkflowPage::titleLabel() const
{
    return m_titleLabel;
}

QLabel *WorkflowPage::subtitleLabel() const
{
    return m_subtitleLabel;
}
