#include "devicecard.h"
#include "snapshotpreview.h"
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QTimer>
#include <QMouseEvent>
#include <cmath>

Symbol::Symbol(QString kind, QWidget *parent) : QWidget(parent), m_kind(std::move(kind)) {
    setFixedSize(44, 44);
}
void Symbol::setActive(bool active) { m_active = active; update(); }
void Symbol::setKind(const QString &kind) { m_kind = kind; update(); }
void Symbol::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(m_active ? QColor("#dcece1") : QColor("#f0f1ed"));
    p.drawRoundedRect(QRectF(0, 0, 44, 44), 13, 13);
    p.setPen(QPen(m_active ? QColor("#287452") : QColor("#778278"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (m_kind == "light") {
        p.drawEllipse(QRectF(15, 9, 14, 17));
        p.drawLine(18, 27, 26, 27); p.drawLine(19, 31, 25, 31); p.drawLine(20, 34, 24, 34);
        p.drawLine(22, 5, 22, 6); p.drawLine(10, 15, 12, 16); p.drawLine(32, 16, 34, 15);
    } else if (m_kind == "outlet" || m_kind == "switch") {
        p.drawRoundedRect(QRectF(11, 10, 22, 24), 5, 5);
        p.drawLine(18, 17, 18, 21); p.drawLine(26, 17, 26, 21); p.drawArc(QRectF(18, 24, 8, 5), 0, 180 * 16);
    } else if (m_kind == "camera") {
        p.drawRoundedRect(QRectF(9, 13, 26, 20), 4, 4);
        p.drawEllipse(QRectF(17, 17, 11, 11));
        p.drawLine(15, 13, 18, 9); p.drawLine(18, 9, 27, 9); p.drawLine(27, 9, 30, 13);
    } else if (m_kind == "sensor") {
        p.drawEllipse(QRectF(18, 18, 8, 8));
        p.drawArc(QRectF(12, 12, 20, 20), 30 * 16, 120 * 16);
        p.drawArc(QRectF(12, 12, 20, 20), 210 * 16, 120 * 16);
        p.drawArc(QRectF(6, 6, 32, 32), 35 * 16, 110 * 16);
        p.drawArc(QRectF(6, 6, 32, 32), 215 * 16, 110 * 16);
    } else if (m_kind == "lock") {
        p.drawRoundedRect(QRectF(12, 20, 20, 16), 4, 4); p.drawArc(QRectF(16, 9, 12, 20), 0, 180 * 16);
        p.drawLine(22, 26, 22, 30);
    } else if (m_kind == "cover") {
        p.drawRect(QRectF(10, 10, 24, 24)); p.drawLine(14, 10, 14, 33); p.drawLine(30, 10, 30, 33);
        p.drawLine(10, 16, 34, 16); p.drawLine(10, 21, 34, 21); p.drawLine(10, 26, 34, 26);
    } else if (m_kind == "thermostat") {
        p.drawEllipse(QRectF(9, 9, 26, 26)); p.drawText(QRectF(9, 9, 26, 26), Qt::AlignCenter, "°");
    } else if (m_kind == "fan") {
        p.drawEllipse(QRectF(19, 19, 6, 6));
        for (int angle : {0, 120, 240}) { p.save(); p.translate(22, 22); p.rotate(angle); p.drawEllipse(QRectF(-4, -15, 9, 12)); p.restore(); }
    } else {
        QPainterPath path; path.moveTo(10, 22); path.lineTo(22, 11); path.lineTo(34, 22);
        path.moveTo(14, 20); path.lineTo(14, 33); path.lineTo(30, 33); path.lineTo(30, 20);
        path.moveTo(20, 33); path.lineTo(20, 25); path.lineTo(25, 25); path.lineTo(25, 33); p.drawPath(path);
    }
}

DeviceCard::DeviceCard(const MatterDevice &device, QWidget *parent) : QFrame(parent) {
    setObjectName("deviceCard"); setMinimumWidth(240); setMinimumHeight(244);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto *box = new QVBoxLayout(this); box->setContentsMargins(21, 19, 21, 17); box->setSpacing(10);
    auto *top = new QHBoxLayout;
    m_icon = new Symbol(device.kind); top->addWidget(m_icon); top->addStretch();
    m_status = new QLabel; m_status->setObjectName("deviceStatus"); top->addWidget(m_status);
    m_favorite = new QPushButton("☆"); m_favorite->setObjectName("favorite"); m_favorite->setFixedSize(27, 28); m_favorite->setCheckable(true);
    m_favorite->setAccessibleName("收藏配件"); m_favorite->setToolTip("添加或移除收藏"); m_favorite->hide(); top->addWidget(m_favorite);
    auto *details = new QPushButton("···"); details->setObjectName("more"); details->setFixedSize(28, 30);
    details->setToolTip("查看 Matter 端点与属性"); details->setAccessibleName("设备详情");
    top->addWidget(details); box->addLayout(top);
    m_name = new QLabel; m_name->setObjectName("deviceName"); m_name->setTextFormat(Qt::PlainText); m_name->setWordWrap(true); box->addWidget(m_name);
    m_origin = new QLabel; m_origin->setTextFormat(Qt::PlainText); m_origin->setObjectName("mutedSmall"); box->addWidget(m_origin);
    m_origin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_snapshotPanel = new QWidget(this); m_snapshotPanel->setObjectName("snapshotPanel");
    auto *snapshotLayout = new QVBoxLayout(m_snapshotPanel); snapshotLayout->setContentsMargins(0, 0, 0, 0); snapshotLayout->setSpacing(7);
    m_snapshotPreview = new SnapshotPreview(m_snapshotPanel); snapshotLayout->addWidget(m_snapshotPreview);
    auto *snapshotRow = new QHBoxLayout; snapshotRow->setContentsMargins(0, 0, 0, 0); snapshotRow->setSpacing(8);
    m_snapshotTime = new QLabel("尚无快照"); m_snapshotTime->setTextFormat(Qt::PlainText); m_snapshotTime->setObjectName("snapshotTime");
    m_snapshotTime->setStyleSheet("color:#889282; font-size:10px;"); snapshotRow->addWidget(m_snapshotTime, 1);
    m_snapshotRefresh = new QPushButton("↻ 刷新"); m_snapshotRefresh->setObjectName("snapshotRefresh");
    m_snapshotRefresh->setCursor(Qt::PointingHandCursor); m_snapshotRefresh->setAccessibleName("刷新摄像头快照");
    m_snapshotRefresh->setToolTip("获取一张新的摄像头快照"); m_snapshotRefresh->setMaximumWidth(82);
    m_snapshotRefresh->setStyleSheet("QPushButton {color:#56734f; background:#eef3e9; border:0; border-radius:6px; padding:4px 8px; font-size:10px;} QPushButton:disabled {color:#a2aa9a; background:#f0f2eb;}");
    snapshotRow->addWidget(m_snapshotRefresh); snapshotLayout->addLayout(snapshotRow);
    m_snapshotStatus = new QLabel; m_snapshotStatus->setObjectName("snapshotStatus"); m_snapshotStatus->setTextFormat(Qt::PlainText);
    m_snapshotStatus->setWordWrap(true); m_snapshotStatus->setStyleSheet("color:#987848; font-size:10px;"); snapshotLayout->addWidget(m_snapshotStatus);
    box->addWidget(m_snapshotPanel); m_snapshotPanel->hide();
    m_snapshotClock = new QTimer(this); m_snapshotClock->setInterval(5000);
    connect(m_snapshotClock, &QTimer::timeout, this, &DeviceCard::refreshSnapshotState);
    connect(m_snapshotRefresh, &QPushButton::clicked, this, [this] { emit snapshotRequested(m_device); });
    connect(m_snapshotPreview, &QAbstractButton::clicked, this, [this] { emit cameraRequested(m_device); });
    m_readings = new QLabel; m_readings->setTextFormat(Qt::PlainText); m_readings->setObjectName("reading"); m_readings->setWordWrap(true); box->addWidget(m_readings);
    auto *levelRow = new QHBoxLayout;
    m_level = new QSlider(Qt::Horizontal); m_level->setRange(0, 100); m_level->setAccessibleName("亮度百分比");
    m_levelLabel = new QLabel; m_levelLabel->setFixedWidth(44); m_levelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_levelLabel->setObjectName("mutedSmall"); levelRow->addWidget(m_level); levelRow->addWidget(m_levelLabel); box->addLayout(levelRow);
    auto *temperatureRow = new QHBoxLayout;
    m_temperature = new QSlider(Qt::Horizontal); m_temperature->setRange(153, 500); m_temperature->setAccessibleName("色温");
    m_temperature->setToolTip("色温：153–500 mired（约 6500–2000 K）");
    m_temperatureLabel = new QLabel; m_temperatureLabel->setFixedWidth(58); m_temperatureLabel->setObjectName("mutedSmall");
    temperatureRow->addWidget(m_temperature); temperatureRow->addWidget(m_temperatureLabel); box->addLayout(temperatureRow);
    box->addStretch();
    m_power = new QPushButton; m_power->setObjectName("power"); m_power->setMinimumHeight(34); box->addWidget(m_power);
    m_camera = new QPushButton("查看实时画面"); m_camera->setObjectName("cameraOpen"); m_camera->setMinimumHeight(34); box->addWidget(m_camera);
    m_camera->setStyleSheet("QPushButton {background:#e6f0e7; color:#26724c; border:0;} QPushButton:hover:enabled {background:#dbe9dc;} QPushButton:disabled {background:#f0f2ec; color:#a5ad9e;}");
    m_moreControls = new QPushButton("控制"); m_moreControls->setObjectName("accessoryControls"); m_moreControls->hide(); box->addWidget(m_moreControls);
    connect(m_moreControls, &QPushButton::clicked, this, [this] { emit detailsRequested(m_device); });
    connect(m_favorite, &QPushButton::clicked, this, [this](bool checked) { emit favoriteRequested(m_device, checked); });
    connect(m_camera, &QPushButton::clicked, this, [this] { emit cameraRequested(m_device); });
    connect(details, &QPushButton::clicked, this, [this] { emit detailsRequested(m_device); });
    connect(m_power, &QPushButton::clicked, this, [this] { emit powerRequested(m_device, !m_device.on); });
    // Only commit on release; a slider drag must not flood the Matter network.
    connect(m_level, &QSlider::sliderReleased, this, [this] { m_levelTimer->stop(); emit levelRequested(m_device, m_level->value()); });
    connect(m_level, &QSlider::valueChanged, this, [this](int value) { m_levelLabel->setText(QString::number(value) + "%"); });
    connect(m_temperature, &QSlider::sliderReleased, this, [this] { m_temperatureTimer->stop(); emit temperatureRequested(m_device, m_temperature->value()); });
    connect(m_temperature, &QSlider::valueChanged, this, [this](int value) { m_temperatureLabel->setText(QString::number(1000000 / value) + " K"); });
    // Keyboard and accessibility controls also commit after one short pause.
    m_levelTimer = new QTimer(this); m_levelTimer->setSingleShot(true); m_levelTimer->setInterval(350);
    m_temperatureTimer = new QTimer(this); m_temperatureTimer->setSingleShot(true); m_temperatureTimer->setInterval(350);
    connect(m_level, &QSlider::actionTriggered, this, [this](int) { if (!m_level->isSliderDown()) m_levelTimer->start(); });
    connect(m_levelTimer, &QTimer::timeout, this, [this] { if (m_level->isEnabled()) emit levelRequested(m_device, m_level->value()); });
    connect(m_temperature, &QSlider::actionTriggered, this, [this](int) { if (!m_temperature->isSliderDown()) m_temperatureTimer->start(); });
    connect(m_temperatureTimer, &QTimer::timeout, this, [this] { if (m_temperature->isEnabled()) emit temperatureRequested(m_device, m_temperature->value()); });
    updateDevice(device, false);
}

void DeviceCard::updateDevice(const MatterDevice &device, bool connected) {
    m_device = device; m_connected = connected;
    const bool online = device.available && connected;
    m_icon->setKind(device.kind);
    m_icon->setActive(online && (device.on || device.kind == "sensor" || device.kind == "camera"));
    m_name->setText(device.name);
    m_name->setToolTip(device.name);
    m_origin->setText(QString("%1  ·  端点 %2").arg(device.nodeName).arg(device.endpointId));
    m_origin->setToolTip(m_origin->text());
    m_status->setText(online ? "● 在线" : "○ 离线");
    m_status->setStyleSheet(online ? "color:#478364;" : "color:#92998f;");
    m_readings->setText(device.readings.join("\n"));
    m_readings->setVisible(!device.readings.isEmpty());
    m_power->setVisible(device.hasOnOff);
    m_camera->setVisible(device.hasCamera); m_camera->setEnabled(online);
    refreshSnapshotState();
    m_power->setEnabled(online);
    m_power->setText(!device.onKnown ? "状态未知 · 点击开启" : device.on ? "已开启    ●" : "已关闭    ○");
    m_power->setProperty("active", device.on && device.onKnown);
    m_power->setStyleSheet(device.on && device.onKnown ? "background:#e6f0e7; color:#26724c; border:0;" : "background:#f1f2ee; color:#70796f; border:0;");
    m_level->setVisible(device.hasLevel); m_levelLabel->setVisible(device.hasLevel);
    m_level->setEnabled(online);
    if (!m_level->isSliderDown() && !m_levelTimer->isActive()) {
        QSignalBlocker block(m_level);
        m_level->setValue(qRound(device.level * 100.0 / 254));
        m_levelLabel->setText(device.levelKnown ? QString::number(m_level->value()) + "%" : "未知");
    }
    m_temperature->setVisible(device.hasColorTemperature); m_temperatureLabel->setVisible(device.hasColorTemperature);
    m_temperature->setEnabled(online);
    const QString temperaturePrefix = QString::number(device.endpointId) + "/768/";
    const auto minTemperature = device.attributes.value(temperaturePrefix + "16395");
    const auto maxTemperature = device.attributes.value(temperaturePrefix + "16396");
    const int minMired = minTemperature.isDouble() && minTemperature.toInt() > 0 ? minTemperature.toInt() : 153;
    const int maxMired = maxTemperature.isDouble() && maxTemperature.toInt() >= minMired ? maxTemperature.toInt() : qMax(500, minMired);
    if (!m_temperature->isSliderDown() && !m_temperatureTimer->isActive()) {
        QSignalBlocker block(m_temperature); m_temperature->setRange(minMired, maxMired);
        m_temperature->setValue(qBound(minMired, device.colorTemperature, maxMired));
        m_temperature->setToolTip(QString("色温：%1–%2 mired").arg(minMired).arg(maxMired));
        m_temperatureLabel->setText(device.colorTemperature > 0 ? QString::number(1000000 / device.colorTemperature) + " K" : "未知");
    }
    if (m_home) {
        m_power->setVisible(device.controls.contains("power"));
        m_origin->setText(m_room.isEmpty() ? QStringLiteral("未分配房间") : m_room);
        m_level->hide(); m_levelLabel->hide(); m_temperature->hide(); m_temperatureLabel->hide();
        QStringList summary;
        const auto s = device.controlState;
        if (device.kind == "light" && device.hasLevel && device.levelKnown) summary << QString("亮度 %1%").arg(qRound(device.level * 100.0 / 254));
        if (device.kind == "lock") summary << (s.contains("lock") ? s.value("lock").toBool() ? "已上锁" : "已开锁" : "状态未知");
        if (device.kind == "cover") summary << (s.contains("position") ? QString("开启 %1%").arg(s.value("position").toInt()) : "位置未知");
        if (device.kind == "thermostat") {
            if (s.contains("heatSetpoint")) summary << QString("制热 %1°C").arg(s.value("heatSetpoint").toDouble(),0,'f',1);
            if (s.contains("coolSetpoint")) summary << QString("制冷 %1°C").arg(s.value("coolSetpoint").toDouble(),0,'f',1);
        }
        if (device.kind == "fan") summary << (s.contains("fanPercent") ? QString("风速 %1%").arg(s.value("fanPercent").toInt()) : "风扇");
        if (summary.isEmpty()) summary = device.readings.mid(0, 2);
        m_readings->setText(summary.join(" · ")); m_readings->setVisible(!summary.isEmpty());
        m_moreControls->setVisible(!device.hasOnOff && !device.hasCamera && !device.controls.isEmpty());
        m_moreControls->setEnabled(online);
        m_moreControls->setText(device.kind == "lock" ? "门锁控制" : device.kind == "cover" ? "调节窗帘" : device.kind == "thermostat" ? "温度控制" : "调节配件");
        m_power->setStyleSheet(device.on && device.onKnown ? "background:#fff0c9; color:#8f6214; border:0;" : "background:#f0f2f5; color:#64717c; border:0;");
    }
}

void DeviceCard::setHomePresentation(const QString &room, bool favorite) {
    m_home = true; m_room = room;
    m_name->setMinimumWidth(0);m_name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    m_readings->setMinimumWidth(0);m_readings->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    setMinimumWidth(180); setMinimumHeight(m_device.hasCamera ? 280 : 200);setMaximumHeight(m_device.hasCamera ? QWIDGETSIZE_MAX : 200);
    m_name->setMaximumHeight(40);m_readings->setMaximumHeight(44);
    m_favorite->show(); QSignalBlocker block(m_favorite); m_favorite->setChecked(favorite); m_favorite->setText(favorite ? "★" : "☆");
    if (auto *box = qobject_cast<QVBoxLayout *>(layout())) { box->setContentsMargins(17, 15, 17, 15); box->setSpacing(7); }
    updateDevice(m_device, m_connected);
}
void DeviceCard::mouseDoubleClickEvent(QMouseEvent *event) {
    if (m_home && event->button() == Qt::LeftButton) { emit detailsRequested(m_device); event->accept(); return; }
    QFrame::mouseDoubleClickEvent(event);
}

void DeviceCard::setSnapshot(const QImage &image, const QDateTime &capturedAt, const QString &message, bool loading, bool stale) {
    m_snapshotPreview->setImage(image); m_snapshotCapturedAt = capturedAt;
    m_snapshotMessage = message.left(180); m_snapshotLoading = loading; m_snapshotStale = stale;
    refreshSnapshotState();
}

void DeviceCard::refreshSnapshotState() {
    const bool camera = m_device.hasCamera;
    m_snapshotPanel->setVisible(camera);
    if (!camera) { m_snapshotClock->stop(); return; }
    if (!m_snapshotClock->isActive()) m_snapshotClock->start();
    const bool online = m_connected && m_device.available;
    const bool hasImage = m_snapshotPreview->hasImage();
    const bool stale = hasImage && (m_snapshotStale || !online);
    m_snapshotPreview->setEnabled(online);
    m_snapshotPreview->setAccessibleName(m_device.name + QStringLiteral("快照，打开实时画面"));
    m_snapshotRefresh->setEnabled(online && !m_snapshotLoading);
    m_snapshotRefresh->setText(m_snapshotLoading ? QStringLiteral("刷新中…") : QStringLiteral("↻ 刷新"));
    QString time = QStringLiteral("尚无快照");
    if (hasImage && m_snapshotCapturedAt.isValid()) {
        const qint64 seconds = qMax<qint64>(0, m_snapshotCapturedAt.secsTo(QDateTime::currentDateTimeUtc()));
        time = seconds < 5 ? QStringLiteral("刚刚更新") : seconds < 60 ? QStringLiteral("%1 秒前更新").arg(seconds)
            : seconds < 3600 ? QStringLiteral("%1 分钟前更新").arg(seconds / 60)
            : seconds < 86400 ? QStringLiteral("%1 小时前更新").arg(seconds / 3600)
            : QStringLiteral("%1 天前更新").arg(seconds / 86400);
    } else if (hasImage) time = QStringLiteral("快照时间未知");
    m_snapshotTime->setText(time);
    QString status;
    if (!online) status = hasImage ? QStringLiteral("设备离线 · 显示旧快照") : QStringLiteral("设备离线 · 暂无快照");
    else if (m_snapshotLoading) status = hasImage ? QStringLiteral("正在刷新，当前显示上次快照") : QStringLiteral("正在获取快照…");
    else if (!m_snapshotMessage.isEmpty()) status = m_snapshotMessage + (stale ? QStringLiteral(" · 显示旧快照") : QString());
    else if (stale) status = QStringLiteral("显示上次快照，等待更新");
    m_snapshotStatus->setText(status); m_snapshotStatus->setVisible(!status.isEmpty());
    const QString emptyText = !online ? QStringLiteral("设备离线") : m_snapshotLoading ? QStringLiteral("正在获取快照…") : QStringLiteral("尚无快照");
    m_snapshotPreview->setPreviewState(m_snapshotLoading, stale, emptyText);
    m_snapshotPreview->setAccessibleDescription((hasImage ? time : emptyText) + (status.isEmpty() ? QString() : QStringLiteral("，") + status));
}
