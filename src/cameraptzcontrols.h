#pragma once
#include "matterdevice.h"
#include <QPointer>
#include <QWidget>

class MatterClient;
class QLabel;
class QPushButton;
class QShowEvent;
class QHideEvent;

// A native, click-only control. There is no held-button motor operation.
class CameraPtzControls : public QWidget {
    Q_OBJECT
public:
    explicit CameraPtzControls(MatterClient *client, MatterDevice device, QWidget *parent = nullptr);
    void setClosing();
    void setDevice(const MatterDevice &device);
signals:
    void presentationChanged();
    void stepAccepted();
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    void updateControls();
    void readCapabilities();
    void setFeedback(const QString &text);
    QPointer<MatterClient> m_client;
    MatterDevice m_device;
    QPushButton *m_directions[4];
    QPushButton *m_refresh;
    QLabel *m_status;
    QString m_request;
    bool m_closing = false;
};
