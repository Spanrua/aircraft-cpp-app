#pragma once

#include "services/CeyearPscSweep.h"
#include "services/PscWidebandAlertDetector.h"

#include <QWidget>

#include <optional>

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
    void setAlertBand(const PscWidebandAlert &alert);
    void clearAlertBand();
    bool hasAlertBand() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawAxes(QPainter *painter, const QRect &plot, double startHz, double stopHz) const;
    void drawAlertBand(QPainter *painter, const QRect &plot, double startHz, double stopHz) const;

    QVector<CeyearPscFrame> m_frames;
    std::optional<PscWidebandAlert> m_alertBand;
    int m_maximumRows = 200;
    double m_startHz = 0.0;
    double m_stopHz = 0.0;
};
