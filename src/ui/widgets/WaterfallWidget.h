#pragma once

#include "services/CeyearPscSweep.h"

#include <QWidget>

class QPainter;

class WaterfallWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WaterfallWidget(QWidget *parent = nullptr);

    QSize minimumSizeHint() const override;

    void setMaximumRows(int rows);
    void setFrequencyRange(double startHz, double stopHz);
    void clearFrames();
    void appendFrame(const CeyearPscFrame &frame);
    int frameCount() const;
    QRect plotArea() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawAxes(QPainter *painter, const QRect &plot, double startHz, double stopHz) const;

    QVector<CeyearPscFrame> m_frames;
    int m_maximumRows = 200;
    double m_startHz = 0.0;
    double m_stopHz = 0.0;
};
