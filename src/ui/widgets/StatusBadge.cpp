#include "ui/widgets/StatusBadge.h"

#include <QStyle>

StatusBadge::StatusBadge(const QString &text, QWidget *parent)
    : QLabel(text, parent)
{
    setObjectName("StatusBadge");
}

void StatusBadge::setStatus(const QString &text, const QString &kind)
{
    setText(text);
    setProperty("kind", kind);
    style()->unpolish(this);
    style()->polish(this);
}
