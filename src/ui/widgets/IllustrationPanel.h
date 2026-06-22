#pragma once

#include <QWidget>

class QPainter;
class QRectF;

class IllustrationPanel : public QWidget
{
    Q_OBJECT

public:
    enum class Kind
    {
        SignalCapture,
        Preprocess,
        Dataset,
        Training,
        Recognition,
    };

    explicit IllustrationPanel(Kind kind, const QString &title, const QString &subtitle, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawBackground(QPainter &painter, const QRectF &rect) const;
    void drawTextBlock(QPainter &painter, const QRectF &rect) const;
    void drawCapture(QPainter &painter, const QRectF &rect) const;
    void drawPreprocess(QPainter &painter, const QRectF &rect) const;
    void drawDataset(QPainter &painter, const QRectF &rect) const;
    void drawTraining(QPainter &painter, const QRectF &rect) const;
    void drawRecognition(QPainter &painter, const QRectF &rect) const;

    Kind m_kind;
    QString m_title;
    QString m_subtitle;
};
