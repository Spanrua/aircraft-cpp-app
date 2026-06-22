#pragma once

#include <QWidget>

class QLabel;
class QScrollArea;
class QVBoxLayout;

class WorkflowPage : public QWidget
{
    Q_OBJECT

public:
    explicit WorkflowPage(const QString &title, const QString &subtitle, QWidget *parent = nullptr);
    explicit WorkflowPage(const QString &title, const QString &subtitle, QWidget *parent, bool scrollableContent);

protected:
    QVBoxLayout *contentLayout() const;
    QLabel *titleLabel() const;
    QLabel *subtitleLabel() const;

private:
    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_contentWidget = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
};
