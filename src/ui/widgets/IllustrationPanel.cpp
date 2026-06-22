#include "ui/widgets/IllustrationPanel.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QtMath>

IllustrationPanel::IllustrationPanel(Kind kind, const QString &title, const QString &subtitle, QWidget *parent)
    : QWidget(parent)
    , m_kind(kind)
    , m_title(title)
    , m_subtitle(subtitle)
{
    setObjectName(QStringLiteral("IllustrationPanel"));
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize IllustrationPanel::sizeHint() const
{
    return {720, 172};
}

QSize IllustrationPanel::minimumSizeHint() const
{
    return {420, 142};
}

void IllustrationPanel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF rect = QRectF(0.5, 0.5, width() - 1.0, height() - 1.0);
    drawBackground(painter, rect);

    const QRectF artRect(rect.left() + rect.width() * 0.46, rect.top() + 18, rect.width() * 0.48, rect.height() - 36);
    switch (m_kind) {
    case Kind::SignalCapture:
        drawCapture(painter, artRect);
        break;
    case Kind::Preprocess:
        drawPreprocess(painter, artRect);
        break;
    case Kind::Dataset:
        drawDataset(painter, artRect);
        break;
    case Kind::Training:
        drawTraining(painter, artRect);
        break;
    case Kind::Recognition:
        drawRecognition(painter, artRect);
        break;
    }

    drawTextBlock(painter, rect);
}

void IllustrationPanel::drawBackground(QPainter &painter, const QRectF &rect) const
{
    QLinearGradient fill(rect.topLeft(), rect.bottomRight());
    fill.setColorAt(0.0, QColor(31, 45, 60));
    fill.setColorAt(0.55, QColor(21, 31, 42));
    fill.setColorAt(1.0, QColor(18, 27, 37));
    painter.setPen(QPen(QColor(78, 96, 116, 140), 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, 14, 14);

    painter.setPen(QPen(QColor(96, 165, 250, 30), 1));
    for (int i = 0; i < 6; ++i) {
        const qreal y = rect.top() + 24 + i * 22;
        painter.drawLine(QPointF(rect.left() + 18, y), QPointF(rect.right() - 18, y));
    }
}

void IllustrationPanel::drawTextBlock(QPainter &painter, const QRectF &rect) const
{
    const QRectF textRect(rect.left() + 24, rect.top() + 24, rect.width() * 0.4, rect.height() - 48);

    QFont titleFont = painter.font();
    titleFont.setPointSize(14);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(QColor(248, 250, 252));
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignTop, m_title);

    QFont subtitleFont = painter.font();
    subtitleFont.setPointSize(9);
    subtitleFont.setWeight(QFont::Normal);
    painter.setFont(subtitleFont);
    painter.setPen(QColor(158, 174, 190));
    painter.drawText(textRect.adjusted(0, 34, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_subtitle);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(96, 165, 250));
    painter.drawRoundedRect(QRectF(textRect.left(), rect.bottom() - 34, 58, 4), 2, 2);
    painter.setBrush(QColor(20, 184, 166));
    painter.drawRoundedRect(QRectF(textRect.left() + 66, rect.bottom() - 34, 34, 4), 2, 2);
}

void IllustrationPanel::drawCapture(QPainter &painter, const QRectF &rect) const
{
    const QPointF tower(rect.left() + rect.width() * 0.22, rect.center().y() + 34);
    painter.setPen(QPen(QColor(148, 163, 184), 3));
    painter.drawLine(tower, QPointF(tower.x(), tower.y() - 76));
    painter.drawLine(QPointF(tower.x() - 20, tower.y()), QPointF(tower.x(), tower.y() - 76));
    painter.drawLine(QPointF(tower.x() + 20, tower.y()), QPointF(tower.x(), tower.y() - 76));

    painter.setPen(QPen(QColor(45, 212, 191, 150), 2));
    for (int i = 0; i < 3; ++i) {
        painter.drawArc(QRectF(tower.x() - 34 - i * 22, tower.y() - 92 - i * 22, 68 + i * 44, 68 + i * 44), 20 * 16, 70 * 16);
    }

    QPainterPath aircraft;
    const QPointF nose(rect.right() - 38, rect.center().y() - 20);
    aircraft.moveTo(nose);
    aircraft.lineTo(nose.x() - 84, nose.y() - 22);
    aircraft.lineTo(nose.x() - 58, nose.y() + 2);
    aircraft.lineTo(nose.x() - 92, nose.y() + 22);
    aircraft.closeSubpath();
    painter.setPen(QPen(QColor(147, 197, 253), 1.5));
    painter.setBrush(QColor(59, 130, 246, 95));
    painter.drawPath(aircraft);
}

void IllustrationPanel::drawPreprocess(QPainter &painter, const QRectF &rect) const
{
    painter.setPen(QPen(QColor(96, 165, 250), 2));
    QPainterPath wave;
    for (int i = 0; i < 160; ++i) {
        const qreal x = rect.left() + i * rect.width() / 159.0;
        const qreal y = rect.center().y() + qSin(i / 8.0) * 22 + qSin(i / 2.7) * 6;
        if (i == 0) {
            wave.moveTo(x, y);
        } else {
            wave.lineTo(x, y);
        }
    }
    painter.drawPath(wave);

    painter.setPen(QPen(QColor(45, 212, 191, 170), 2));
    for (int i = 0; i < 5; ++i) {
        const QRectF chip(rect.left() + 22 + i * 52, rect.center().y() + 38, 34, 22);
        painter.drawRoundedRect(chip, 5, 5);
    }
}

void IllustrationPanel::drawDataset(QPainter &painter, const QRectF &rect) const
{
    const int columns = 4;
    const int rows = 3;
    const QSizeF card((rect.width() - 30) / columns, (rect.height() - 24) / rows);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            QRectF cell(rect.left() + col * (card.width() + 10), rect.top() + row * (card.height() + 12), card.width(), card.height());
            painter.setPen(QPen(QColor(84, 105, 128), 1));
            painter.setBrush((row + col) % 3 == 0 ? QColor(20, 184, 166, 70) : QColor(96, 165, 250, 55));
            painter.drawRoundedRect(cell, 8, 8);
            painter.setPen(QPen(QColor(226, 232, 240, 130), 2));
            painter.drawLine(cell.left() + 10, cell.center().y(), cell.right() - 10, cell.center().y());
        }
    }
}

void IllustrationPanel::drawTraining(QPainter &painter, const QRectF &rect) const
{
    QVector<QPointF> nodes = {
        {rect.left() + 30, rect.center().y() - 38},
        {rect.left() + 30, rect.center().y() + 38},
        {rect.center().x(), rect.center().y() - 52},
        {rect.center().x(), rect.center().y()},
        {rect.center().x(), rect.center().y() + 52},
        {rect.right() - 32, rect.center().y()},
    };

    painter.setPen(QPen(QColor(74, 96, 119), 1.5));
    for (int i = 0; i < 2; ++i) {
        for (int j = 2; j < 5; ++j) {
            painter.drawLine(nodes.at(i), nodes.at(j));
        }
    }
    for (int j = 2; j < 5; ++j) {
        painter.drawLine(nodes.at(j), nodes.last());
    }

    painter.setPen(QPen(QColor(147, 197, 253), 2));
    painter.setBrush(QColor(59, 130, 246, 120));
    for (const QPointF &node : nodes) {
        painter.drawEllipse(node, 8, 8);
    }
    painter.setBrush(QColor(45, 212, 191, 130));
    painter.drawEllipse(nodes.last(), 11, 11);
}

void IllustrationPanel::drawRecognition(QPainter &painter, const QRectF &rect) const
{
    const QRectF scope(rect.left() + 16, rect.top() + 14, rect.height() - 28, rect.height() - 28);
    painter.setPen(QPen(QColor(45, 212, 191, 155), 2));
    painter.setBrush(QColor(20, 184, 166, 28));
    painter.drawEllipse(scope);
    painter.drawLine(scope.center().x(), scope.top() + 10, scope.center().x(), scope.bottom() - 10);
    painter.drawLine(scope.left() + 10, scope.center().y(), scope.right() - 10, scope.center().y());
    painter.drawEllipse(scope.center(), 24, 24);

    const QRectF result(rect.left() + rect.height() + 16, rect.top() + 24, rect.width() - rect.height() - 28, rect.height() - 48);
    painter.setPen(QPen(QColor(96, 165, 250, 145), 1.5));
    painter.setBrush(QColor(59, 130, 246, 52));
    painter.drawRoundedRect(result, 10, 10);
    painter.setBrush(QColor(45, 212, 191, 130));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(result.left() + 16, result.top() + 18, result.width() * 0.72, 9), 4, 4);
    painter.setBrush(QColor(148, 163, 184, 110));
    painter.drawRoundedRect(QRectF(result.left() + 16, result.top() + 42, result.width() * 0.52, 8), 4, 4);
    painter.drawRoundedRect(QRectF(result.left() + 16, result.top() + 64, result.width() * 0.38, 8), 4, 4);
}
