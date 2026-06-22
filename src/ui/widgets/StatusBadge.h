#pragma once

#include <QLabel>

class StatusBadge : public QLabel
{
    Q_OBJECT

public:
    explicit StatusBadge(const QString &text = QString(), QWidget *parent = nullptr);

    void setStatus(const QString &text, const QString &kind);
};

