#include "ui/widgets/WaterfallWidget.h"

#include <QPainter>
#include <QtMath>
#include <QtGlobal>

namespace
{
constexpr int kPlotLeftMargin = 72;
constexpr int kPlotTopMargin = 12;
constexpr int kPlotRightMargin = 18;
constexpr int kPlotBottomMargin = 42;

QColor levelColor(double levelDbuv)
{
    const double t = qBound(0.0, (levelDbuv + 30.0) / 75.0, 1.0);

    struct ColorStop
    {
        double position;
        QColor color;
    };

    const ColorStop stops[] = {
        {0.0, QColor(8, 90, 64)},
        {0.30, QColor(18, 145, 78)},
        {0.58, QColor(52, 190, 74)},
        {0.78, QColor(220, 210, 55)},
        {1.0, QColor(255, 70, 34)},
    };

    for (int index = 1; index < int(sizeof(stops) / sizeof(stops[0])); ++index) {
        if (t <= stops[index].position) {
            const ColorStop &start = stops[index - 1];
            const ColorStop &end = stops[index];
            const double localT = (t - start.position) / (end.position - start.position);
            const int red = static_cast<int>(start.color.red() + (end.color.red() - start.color.red()) * localT);
            const int green = static_cast<int>(start.color.green() + (end.color.green() - start.color.green()) * localT);
            const int blue = static_cast<int>(start.color.blue() + (end.color.blue() - start.color.blue()) * localT);
            return QColor(red, green, blue);
        }
    }

    return stops[sizeof(stops) / sizeof(stops[0]) - 1].color;
}

double inferredStartHz(const CeyearPscFrame &frame)
{
    if (frame.bins.isEmpty()) {
        return 0.0;
    }

    double startHz = frame.bins.first().frequencyHz;
    for (const CeyearPscBin &bin : frame.bins) {
        startHz = qMin(startHz, bin.frequencyHz);
    }
    return startHz;
}

double inferredStopHz(const CeyearPscFrame &frame)
{
    if (frame.bins.isEmpty()) {
        return 0.0;
    }

    double stopHz = frame.bins.first().frequencyHz;
    for (const CeyearPscBin &bin : frame.bins) {
        stopHz = qMax(stopHz, bin.frequencyHz);
    }
    return stopHz;
}

int frequencyToX(double frequencyHz, double startHz, double stopHz, int width)
{
    if (width <= 0 || stopHz <= startHz) {
        return 0;
    }

    const double normalized = (frequencyHz - startHz) / (stopHz - startHz);
    return qRound(normalized * width);
}

QString frequencyLabel(double frequencyHz)
{
    return QStringLiteral("%1 MHz").arg(frequencyHz / 1000000.0, 0, 'f', 1);
}
} // namespace

WaterfallWidget::WaterfallWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("PscWaterfallWidget"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize WaterfallWidget::minimumSizeHint() const
{
    return QSize(320, 220);
}

QRect WaterfallWidget::plotArea() const
{
    const int left = qMin(kPlotLeftMargin, qMax(0, width() / 3));
    const int top = qMin(kPlotTopMargin, qMax(0, height() / 5));
    const int right = qMin(kPlotRightMargin, qMax(0, width() / 6));
    const int bottom = qMin(kPlotBottomMargin, qMax(0, height() / 3));
    return rect().adjusted(left, top, -right, -bottom);
}

void WaterfallWidget::setMaximumRows(int rows)
{
    m_maximumRows = qMax(1, rows);
    while (m_frames.size() > m_maximumRows) {
        m_frames.removeFirst();
    }
    update();
}

void WaterfallWidget::setFrequencyRange(double startHz, double stopHz)
{
    if (stopHz > startHz) {
        m_startHz = startHz;
        m_stopHz = stopHz;
    } else {
        m_startHz = 0.0;
        m_stopHz = 0.0;
    }
    update();
}

void WaterfallWidget::clearFrames()
{
    m_frames.clear();
    update();
}

void WaterfallWidget::appendFrame(const CeyearPscFrame &frame)
{
    m_frames.append(frame);
    while (m_frames.size() > m_maximumRows) {
        m_frames.removeFirst();
    }
    update();
}

int WaterfallWidget::frameCount() const
{
    return m_frames.size();
}

void WaterfallWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.fillRect(rect(), QColor(7, 25, 29));

    const QRect plot = plotArea();
    painter.fillRect(plot, QColor(8, 90, 64));
    painter.setPen(QColor(30, 132, 91));
    painter.drawRect(plot.adjusted(0, 0, -1, -1));

    if (m_frames.isEmpty()) {
        painter.setPen(QColor(143, 154, 166));
        painter.drawText(plot, Qt::AlignCenter, tr("等待 PSC 时频数据"));
        drawAxes(&painter, plot, 0.0, 0.0);
        return;
    }

    const int rows = m_frames.size();
    const int rowHeight = qMax(1, plot.height() / qMax(1, m_maximumRows));
    double axisStartHz = m_startHz;
    double axisStopHz = m_stopHz;
    for (int row = 0; row < rows; ++row) {
        const CeyearPscFrame &frame = m_frames.at(rows - 1 - row);
        const int y = plot.bottom() - (row + 1) * rowHeight + 1;

        const double startHz = m_stopHz > m_startHz ? m_startHz : inferredStartHz(frame);
        const double stopHz = m_stopHz > m_startHz ? m_stopHz : inferredStopHz(frame);
        if (axisStopHz <= axisStartHz && stopHz > startHz) {
            axisStartHz = startHz;
            axisStopHz = stopHz;
        }
        const double binWidthHz = frame.bins.size() > 1
            ? qAbs(frame.bins.at(1).frequencyHz - frame.bins.at(0).frequencyHz)
            : qMax(1.0, stopHz - startHz);
        for (const CeyearPscBin &bin : frame.bins) {
            const int x0 = plot.left() + frequencyToX(bin.frequencyHz - binWidthHz / 2.0, startHz, stopHz, plot.width());
            const int x1 = plot.left() + frequencyToX(bin.frequencyHz + binWidthHz / 2.0, startHz, stopHz, plot.width());
            if (x1 <= plot.left() || x0 >= plot.right()) {
                continue;
            }
            const int boundedX = qMax(plot.left(), x0);
            painter.fillRect(QRect(boundedX,
                                   y,
                                   qMax(1, qMin(plot.right() + 1, x1) - boundedX),
                                   rowHeight),
                             levelColor(bin.levelDbuv));
        }
    }

    drawAxes(&painter, plot, axisStartHz, axisStopHz);
}

void WaterfallWidget::drawAxes(QPainter *painter, const QRect &plot, double startHz, double stopHz) const
{
    painter->save();
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const QColor axisColor(191, 205, 213);
    const QColor labelColor(213, 223, 229);
    painter->setPen(QPen(axisColor, 1));

    const int xAxisY = plot.bottom() + 2;
    const int yAxisX = plot.left() - 2;
    painter->drawLine(plot.left(), xAxisY, plot.right(), xAxisY);
    painter->drawLine(yAxisX, plot.top(), yAxisX, plot.bottom());

    const int tickLength = 5;
    painter->drawLine(plot.left(), xAxisY, plot.left(), xAxisY + tickLength);
    painter->drawLine(plot.center().x(), xAxisY, plot.center().x(), xAxisY + tickLength);
    painter->drawLine(plot.right(), xAxisY, plot.right(), xAxisY + tickLength);
    painter->drawLine(yAxisX - tickLength, plot.top(), yAxisX, plot.top());
    painter->drawLine(yAxisX - tickLength, plot.bottom(), yAxisX, plot.bottom());

    painter->setPen(labelColor);
    const QFontMetrics metrics(painter->font());
    const int tickLabelBaseline = xAxisY + tickLength + metrics.ascent();
    if (stopHz > startHz) {
        const double centerHz = (startHz + stopHz) / 2.0;
        const QString startLabel = frequencyLabel(startHz);
        const QString centerLabel = frequencyLabel(centerHz);
        const QString stopLabel = frequencyLabel(stopHz);
        painter->drawText(QPoint(plot.left(), tickLabelBaseline), startLabel);
        painter->drawText(QPoint(plot.center().x() - metrics.horizontalAdvance(centerLabel) / 2, tickLabelBaseline),
                          centerLabel);
        painter->drawText(QPoint(plot.right() - metrics.horizontalAdvance(stopLabel), tickLabelBaseline), stopLabel);
    }

    painter->drawText(QRect(plot.left(), plot.bottom() + 22, plot.width(), 18),
                      Qt::AlignCenter,
                      tr("频率 (MHz)"));
    painter->drawText(QRect(4, plot.top(), plot.left() - 12, 18),
                      Qt::AlignRight | Qt::AlignVCenter,
                      tr("新"));
    painter->drawText(QRect(4, plot.bottom() - 18, plot.left() - 12, 18),
                      Qt::AlignRight | Qt::AlignVCenter,
                      tr("旧"));

    painter->translate(16, plot.center().y());
    painter->rotate(-90);
    painter->drawText(QRect(-plot.height() / 2, -8, plot.height(), 18),
                      Qt::AlignCenter,
                      tr("时间 / 扫频帧"));

    painter->restore();
}
