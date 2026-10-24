#include "cameraptzcontrols.h"
#include "matterclient.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>

CameraPtzControls::CameraPtzControls(MatterClient *client, MatterDevice device, QWidget *parent)
    : QWidget(parent), m_client(client), m_device(std::move(device)) {
    setObjectName("cameraPtzControls");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 4, 0, 0); layout->setSpacing(4);
    auto *row = new QHBoxLayout; row->setSpacing(6);
    auto *title = new QLabel(QStringLiteral("云台"), this); title->setTextFormat(Qt::PlainText); row->addWidget(title);
    row->addStretch();
    setStyleSheet(QStringLiteral("QPushButton { padding:0; min-width:0; border-radius:8px; font-size:17px; } QLabel { font-size:11px; }"));
    static const QStringList labels{QStringLiteral("↑"), QStringLiteral("↓"), QStringLiteral("←"), QStringLiteral("→")};
    static const QStringList accessible{QStringLiteral("向上微调"), QStringLiteral("向下微调"), QStringLiteral("向左微调"), QStringLiteral("向右微调")};
    static const char *names[]{"ptzUp", "ptzDown", "ptzLeft", "ptzRight"};
    for (int direction = 0; direction < 4; ++direction) {
        auto *button = new QPushButton(labels.at(direction), this);
        m_directions[direction] = button;
        button->setObjectName(names[direction]); button->setAutoRepeat(false); button->setFixedSize(32, 30);
        button->setAccessibleName(accessible.at(direction)); button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QStringLiteral("点击一次移动一小步；至少间隔 1.8 秒。"));
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, direction] {
            if (m_closing || !isVisible() || !m_client || !m_client->canMoveCameraStep(m_device, direction)) return;
            m_request = m_client->moveCameraStep(m_device, direction);
            setFeedback(QStringLiteral("正在发送方向请求…"));
            updateControls();
        });
    }
    m_refresh = new QPushButton(QStringLiteral("↻"), this);
    m_refresh->setObjectName("ptzRefresh");
    m_refresh->setFixedSize(32, 30); m_refresh->setAccessibleName(QStringLiteral("重新读取云台能力"));
    m_refresh->setToolTip(QStringLiteral("读取云台能力")); m_refresh->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_refresh); layout->addLayout(row);
    m_status = new QLabel(QStringLiteral("云台能力未知"), this);
    m_status->setObjectName("ptzStatus"); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(m_refresh, &QPushButton::clicked, this, &CameraPtzControls::readCapabilities);
    if (client) {
        connect(client, &MatterClient::devicesChanged, this, &CameraPtzControls::updateControls);
        connect(client, &MatterClient::connectionChanged, this, &CameraPtzControls::updateControls);
        connect(client, &MatterClient::cameraPtzStateChanged, this, &CameraPtzControls::updateControls);
        connect(client, &MatterClient::cameraPtzResponse, this,
            [this](const QString &id, const QString &operation, bool success, const QString &detail) {
                if (m_closing || id != m_request) return;
                m_request.clear(); setFeedback(detail); updateControls();
                if (operation == QStringLiteral("move") && success) emit stepAccepted();
            });
        connect(client, &QObject::destroyed, this, [this] { updateControls(); });
    }
    setVisible(m_device.hasCameraPtz); updateControls();
}

void CameraPtzControls::setClosing() {
    m_closing = true; m_request.clear(); updateControls();
}

void CameraPtzControls::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    updateControls();
    QTimer::singleShot(0, this, [this] {
        if (!m_closing && isVisible()) readCapabilities();
    });
}

void CameraPtzControls::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event); updateControls();
}

void CameraPtzControls::readCapabilities() {
    if (m_closing || !isVisible() || !m_client || !m_refresh->isEnabled() || m_client->cameraPtzBusy()) return;
    m_request = m_client->readCameraPtz(m_device);
    setFeedback(QStringLiteral("正在读取云台能力…")); updateControls();
}

void CameraPtzControls::updateControls() {
    bool refresh = false;
    if (!m_closing && isVisible() && m_client && m_client->isConnected() && !m_client->isDemoMode()) {
        for (const auto &current : m_client->devices())
            if (current.key == m_device.key && current.nodeId == m_device.nodeId && current.endpointId == m_device.endpointId
                && current.controllerGeneration == m_device.controllerGeneration)
                refresh = current.available && current.hasCameraPtz && !m_client->cameraPtzBusy();
    }
    m_refresh->setEnabled(refresh);
    for (int direction = 0; direction < 4; ++direction)
        m_directions[direction]->setEnabled(!m_closing && isVisible() && m_client
            && m_client->canMoveCameraStep(m_device, direction));
    if (m_closing) setFeedback(QStringLiteral("云台控制已关闭"));
    else if (!m_client || !m_client->isConnected() || m_client->isDemoMode()) setFeedback(QStringLiteral("连接摄像头后可使用云台"));
}

void CameraPtzControls::setFeedback(const QString &text) {
    if (m_status->text() == text) return;
    m_status->setText(text); emit presentationChanged();
}

void CameraPtzControls::setDevice(const MatterDevice &device) {
    const bool changed = m_device.key != device.key || m_device.controllerGeneration != device.controllerGeneration;
    m_device = device;
    if (changed) { m_request.clear(); setFeedback(QStringLiteral("云台能力未知")); }
    setVisible(device.hasCameraPtz); updateControls();
    if (changed && isVisible()) QTimer::singleShot(0, this, [this] { readCapabilities(); });
}
