#include "devicecard.h"
#include "snapshotpreview.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QtTest>

namespace {
MatterDevice cameraDevice() {
    MatterDevice device; device.key = "700/1"; device.nodeId = 700; device.endpointId = 1;
    device.name = "客厅摄像头"; device.nodeName = "Homebridge"; device.kind = "camera";
    device.hasCamera = true; device.available = true; return device;
}
QImage syntheticRoom() {
    QImage image(960, 540, QImage::Format_RGB32); image.fill(QColor("#b1c4b7"));
    QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient wall(0, 0, 960, 540); wall.setColorAt(0, QColor("#bdd0bf")); wall.setColorAt(1, QColor("#e4dfce"));
    painter.fillRect(image.rect(), wall);
    painter.fillRect(0, 360, 960, 180, QColor("#aa9179"));
    painter.fillRect(610, 65, 230, 255, QColor("#e9eee7")); painter.fillRect(626, 80, 198, 224, QColor("#92b1a4"));
    painter.setPen(QPen(QColor("#e9eee7"), 10)); painter.drawLine(725, 80, 725, 304); painter.drawLine(626, 185, 824, 185);
    painter.setPen(Qt::NoPen); painter.setBrush(QColor("#486958")); painter.drawRoundedRect(105, 255, 425, 170, 25, 25);
    painter.setBrush(QColor("#6c8870")); painter.drawRoundedRect(122, 275, 190, 92, 15, 15); painter.drawRoundedRect(324, 275, 190, 92, 15, 15);
    painter.setBrush(QColor("#d8cbaf")); painter.drawRoundedRect(445, 395, 245, 36, 15, 15);
    painter.setBrush(QColor("#725c47")); painter.drawRect(475, 425, 12, 60); painter.drawRect(650, 425, 12, 60);
    painter.setBrush(QColor("#3d6449")); painter.drawEllipse(850, 230, 45, 95); painter.drawEllipse(880, 200, 37, 105);
    painter.setBrush(QColor("#c5aa8a")); painter.drawRoundedRect(857, 315, 53, 90, 9, 9);
    painter.setPen(QColor("#f6f7f1")); QFont font; font.setPixelSize(21); painter.setFont(font);
    painter.drawText(QRect(25, 490, 800, 35), Qt::AlignLeft | Qt::AlignVCenter, "合成快照 · 仅用于界面测试");
    return image;
}
}

class DeviceCardSnapshotTest : public QObject {
    Q_OBJECT
private slots:
    void nativePreviewCropsToSixteenByNine() {
        const auto device = cameraDevice(); DeviceCard card(device); card.updateDevice(device, true);
        QImage image(800, 200, QImage::Format_RGB32); image.fill(Qt::red);
        { QPainter painter(&image); painter.fillRect(200, 0, 400, 200, QColor("#225beb")); }
        card.setSnapshot(image, QDateTime::currentDateTimeUtc().addSecs(-17), "", false, false);
        card.resize(360, 460); card.show(); QTest::qWait(20);
        auto *preview = card.findChild<QAbstractButton *>("cameraSnapshot"); QVERIFY(preview);
        QVERIFY(preview->isVisible()); QVERIFY(qAbs(preview->height() - qRound(preview->width() * 9.0 / 16.0)) <= 2);
        const auto rendered = preview->grab().toImage();
        const auto color = rendered.pixelColor(qRound(rendered.width() * 0.78), qRound(rendered.height() * 0.25));
        QVERIFY(color.blue() > 140); QVERIFY(color.red() < 80); QVERIFY(color.green() < 140);
        QVERIFY(card.findChild<QLabel *>("snapshotTime")->text().contains("秒前更新"));
        QVERIFY(!card.findChild<QLabel *>("snapshotStatus")->text().contains("旧快照"));
        card.resize(560, 620); QCoreApplication::processEvents();
        QVERIFY(qAbs(preview->height() - qRound(preview->width() * 9.0 / 16.0)) <= 2);
    }
    void fillPreviewCoversEveryCorner_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("landscape") << QSize(640, 360);
        QTest::newRow("portrait") << QSize(360, 640);
        QTest::newRow("square") << QSize(480, 480);
        QTest::newRow("wide") << QSize(640, 160);
    }
    void fillPreviewCoversEveryCorner() {
        QFETCH(QSize, size);
        QWidget window;
        auto *layout = new QVBoxLayout(&window); layout->setContentsMargins(0, 0, 0, 0);
        auto *preview = new SnapshotPreview(&window); layout->addWidget(preview);
        preview->setFillMode(true);
        QImage image(800, 100, QImage::Format_RGB32); image.fill(Qt::red);
        { QPainter painter(&image); painter.fillRect(100, 0, 600, 100, QColor("#225beb")); }
        preview->setImage(image); window.resize(size); window.show(); QCoreApplication::processEvents();
        QCOMPARE(preview->geometry(), QRect(QPoint(0, 0), size));
        QCOMPARE(preview->minimumSizeHint(), QSize(0, 0));
        QCOMPARE(preview->heightForWidth(size.width()), -1);
        QVERIFY(!preview->sizePolicy().hasHeightForWidth());
        QCOMPARE(preview->sizePolicy().verticalPolicy(), QSizePolicy::Expanding);
        const auto rendered = preview->grab().toImage();
        for (const auto &corner : {rendered.rect().topLeft(), rendered.rect().topRight(),
                                   rendered.rect().bottomLeft(), rendered.rect().bottomRight()}) {
            const auto color = rendered.pixelColor(corner);
            QVERIFY2(color.blue() > 140 && color.red() < 80 && color.green() < 140,
                     qPrintable(QString("Corner (%1, %2) has color %3").arg(corner.x()).arg(corner.y()).arg(color.name())));
        }
        const auto badgeArea = rendered.pixelColor(20, 20);
        QVERIFY(badgeArea.blue() > 140 && badgeArea.red() < 80);
    }
    void fillPreviewRestoresCardGeometryAndKeyboard() {
        SnapshotPreview preview;
        QImage image(800, 200, QImage::Format_RGB32); image.fill(QColor("#225beb")); preview.setImage(image);
        QPalette palette = preview.palette(); palette.setColor(QPalette::Window, Qt::magenta);
        preview.setPalette(palette); preview.setAutoFillBackground(true);
        preview.resize(320, 180); preview.show(); QCoreApplication::processEvents();
        QCOMPARE(preview.minimumHeight(), 180); QCOMPARE(preview.maximumHeight(), 180);
        preview.setFillMode(true); preview.resize(420, 600); QCoreApplication::processEvents();
        QCOMPARE(preview.size(), QSize(420, 600));
        QCOMPARE(preview.minimumHeight(), 0); QCOMPARE(preview.maximumHeight(), QWIDGETSIZE_MAX);
        QSignalSpy clicked(&preview, &QAbstractButton::clicked);
        preview.setFocus(); QTest::keyClick(&preview, Qt::Key_Return); QTest::keyClick(&preview, Qt::Key_Space);
        QCOMPARE(clicked.count(), 2);
        preview.clearFocus(); preview.setFillMode(false); QCoreApplication::processEvents();
        QCOMPARE(preview.size(), QSize(420, qRound(420 * 9.0 / 16.0)));
        QCOMPARE(preview.minimumSizeHint(), QSize(160, 90));
        QVERIFY(preview.sizePolicy().hasHeightForWidth());
        QCOMPARE(preview.sizePolicy().verticalPolicy(), QSizePolicy::Fixed);
        QVERIFY(preview.grab().toImage().pixelColor(0, 0).red() > 150);
        preview.resize(560, 600); QCoreApplication::processEvents();
        QCOMPARE(preview.height(), qRound(560 * 9.0 / 16.0));
        preview.setFillMode(true); preview.resize(240, 640); QCoreApplication::processEvents();
        QCOMPARE(preview.size(), QSize(240, 640));
    }
    void keyboardPlayAndLoadingRefreshGate() {
        const auto device = cameraDevice(); DeviceCard card(device); card.updateDevice(device, true); card.show();
        auto *preview = card.findChild<QAbstractButton *>("cameraSnapshot");
        auto *refresh = card.findChild<QPushButton *>("snapshotRefresh"); QVERIFY(preview); QVERIFY(refresh);
        QSignalSpy open(&card, &DeviceCard::cameraRequested), requested(&card, &DeviceCard::snapshotRequested);
        preview->setFocus(); QTest::keyClick(preview, Qt::Key_Return); QCOMPARE(open.count(), 1);
        QTest::keyClick(preview, Qt::Key_Space); QCOMPARE(open.count(), 2);
        QTest::mouseClick(refresh, Qt::LeftButton); QCOMPARE(requested.count(), 1);
        card.setSnapshot({}, {}, "", true, false); QVERIFY(!refresh->isEnabled());
        QTest::mouseClick(refresh, Qt::LeftButton); QCOMPARE(requested.count(), 1);
        QVERIFY(card.findChild<QLabel *>("snapshotStatus")->text().contains("获取快照"));
        auto offline = device; offline.available = false; card.updateDevice(offline, true);
        QVERIFY(!preview->isEnabled()); QVERIFY(!refresh->isEnabled());
    }
    void stalePrivacyAndPlainTextStates() {
        const auto device = cameraDevice(); DeviceCard card(device); card.updateDevice(device, true);
        const auto at = QDateTime::currentDateTimeUtc().addSecs(-125);
        card.setSnapshot(syntheticRoom(), at, "刷新失败 <b>文本</b>", false, true);
        auto *status = card.findChild<QLabel *>("snapshotStatus"); QCOMPARE(status->textFormat(), Qt::PlainText);
        QVERIFY(status->text().contains("<b>文本</b>")); QVERIFY(status->text().contains("旧快照"));
        QVERIFY(card.findChild<QLabel *>("snapshotTime")->text().contains("2 分钟前更新"));
        card.updateDevice(device, false); QVERIFY(status->text().contains("离线")); QVERIFY(status->text().contains("旧快照"));
        card.updateDevice(device, true); card.setSnapshot({}, {}, "隐私模式已开启", false, false);
        QCOMPARE(status->text(), QString("隐私模式已开启")); QCOMPARE(card.findChild<QLabel *>("snapshotTime")->text(), QString("尚无快照"));
        card.setSnapshot(syntheticRoom(), QDateTime::currentDateTimeUtc(), "刚刚更新", false, false);
        QVERIFY(!status->text().contains("旧快照"));
    }
    void regularDeviceHasNoPreviewLayout() {
        const auto ordinary = devicesFromNodes(demoNodes()).first(); DeviceCard card(ordinary); card.updateDevice(ordinary, true);
        card.resize(360, 244); card.show(); QTest::qWait(20);
        QVERIFY(card.findChild<QWidget *>("snapshotPanel")->isHidden());
        QVERIFY(!card.findChild<QAbstractButton *>("cameraSnapshot")->isVisible());
        QVERIFY(card.minimumSizeHint().height() < 300);
    }
    void saveOfflineSyntheticPreviewWhenRequested() {
        const auto path = qEnvironmentVariable("SNAPSHOT_CARD_SCREENSHOT"); if (path.isEmpty()) return;
        QWidget window; window.setWindowTitle("快照卡片合成界面测试"); window.resize(1120, 520);
        window.setStyleSheet("QWidget{font-family:'Noto Sans CJK SC';font-size:13px;color:#304b36;background:#f6f7f2;} QFrame#deviceCard{background:white;border:1px solid #dfe6d8;border-radius:13px;} QLabel{background:transparent;} QLabel#deviceName{font-size:17px;font-weight:600;} QLabel#mutedSmall{font-size:10px;color:#86917e;} QPushButton{border:0;border-radius:7px;padding:8px;background:#edf3e8;}");
        auto *layout = new QGridLayout(&window); layout->setContentsMargins(18, 18, 18, 18); layout->setSpacing(16);
        for (int index = 0; index < 3; ++index) {
            auto device = cameraDevice(); device.name = index == 0 ? "客厅摄像头" : index == 1 ? "阳台摄像头" : "玄关摄像头";
            auto *card = new DeviceCard(device, &window); card->updateDevice(device, index != 2);
            if (index == 0) card->setSnapshot(syntheticRoom(), QDateTime::currentDateTimeUtc(), "", false, false);
            else if (index == 1) card->setSnapshot({}, {}, "", true, false);
            else card->setSnapshot(syntheticRoom(), QDateTime::currentDateTimeUtc().addSecs(-185), "设备离线", false, true);
            layout->addWidget(card, 0, index, Qt::AlignTop);
        }
        window.show(); QTest::qWait(40); QVERIFY(window.grab().save(path));
    }
};
QTEST_MAIN(DeviceCardSnapshotTest)
#include "tst_devicecard_snapshot.moc"
