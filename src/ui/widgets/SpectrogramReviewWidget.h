#pragma once

#include <QImage>
#include <QWidget>

class SpectrogramReviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrogramReviewWidget(QWidget *parent = nullptr);

    QSize minimumSizeHint() const override;
    void setImagePath(const QString &path);
    void clearImage(const QString &message = QString());
    bool hasImage() const;
    QString imagePath() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    QString m_imagePath;
    QString m_placeholder;
};
