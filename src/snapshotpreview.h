#pragma once

#include <QAbstractButton>
#include <QImage>

class SnapshotPreview final : public QAbstractButton {
public:
    explicit SnapshotPreview(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int heightForWidth(int width) const override;
    void setImage(const QImage &image);
    bool hasImage() const;
    void setFillMode(bool fill);
    void setPreviewState(bool loading, bool stale, const QString &emptyText);
protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    QImage m_image;
    QString m_emptyText = QStringLiteral("尚无快照");
    bool m_loading = false, m_stale = false;
    bool m_fillMode = false;
};
