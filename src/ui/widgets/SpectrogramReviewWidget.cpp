#include "ui/widgets/SpectrogramReviewWidget.h"

#include <QPainter>
#include <QPen>

SpectrogramReviewWidget::SpectrogramReviewWidget(QWidget *parent)
    : QWidget(parent),
      m_placeholder(QStringLiteral("选择候选事件后显示框选时频图"))
{
    setObjectName(QStringLiteral("DatasetSpectrogramReviewWidget"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(280);
}

QSize SpectrogramReviewWidget::minimumSizeHint() const
{
    return QSize(480, 280);
}

void SpectrogramReviewWidget::setImagePath(const QString &path)
{
    m_imagePath = path.trimmed();
    m_image = QImage();
    if (m_imagePath.isEmpty()) {
        m_placeholder = QStringLiteral("当前候选没有生成时频图");
    } else {
        QImage image(m_imagePath);
        if (image.isNull()) {
            m_placeholder = QStringLiteral("时频图无法加载，请检查文件路径");
        } else {
            m_image = image;
            m_placeholder.clear();
        }
    }
    update();
}

void SpectrogramReviewWidget::clearImage(const QString &message)
{
    m_image = QImage();
    m_imagePath.clear();
    m_placeholder = message.trimmed().isEmpty()
        ? QStringLiteral("选择候选事件后显示框选时频图")
        : message;
    update();
}

bool SpectrogramReviewWidget::hasImage() const
{
    return !m_image.isNull();
}

QString SpectrogramReviewWidget::imagePath() const
{
    return m_imagePath;
}

void SpectrogramReviewWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor(7, 16, 24));

    const QRect frame = rect().adjusted(12, 12, -12, -12);
    if (m_image.isNull()) {
        painter.setPen(QColor(151, 166, 181));
        painter.drawText(frame, Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        painter.setPen(QPen(QColor(83, 102, 119), 1, Qt::DashLine));
        painter.drawRect(frame.adjusted(0, 0, -1, -1));
        return;
    }

    const QSize scaledSize = m_image.size().scaled(frame.size(), Qt::KeepAspectRatio);
    const QRect target(QPoint(frame.center().x() - scaledSize.width() / 2,
                              frame.center().y() - scaledSize.height() / 2),
                      scaledSize);
    painter.drawImage(target, m_image);
    painter.setPen(QPen(QColor(100, 126, 145), 1));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
}
