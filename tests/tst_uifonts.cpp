#include <QApplication>
#include <QDir>
#include <QFont>
#include <QGlyphRun>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRawFont>
#include <QSet>
#include <QTextLayout>
#include <QtTest>

namespace {
// These are visible characters used by both applications, including the
// navigation, camera overlay, PTZ and sensor-reading controls. A CJK font alone
// covers the text but leaves several of these characters as .notdef boxes.
const QString UiSymbols = QStringLiteral("°³µ·×–…“”₂←↑→↓↻−≡⊡⌁⌂⌑⏻⛶▶▣◇◈◉○◌●◷★☆☼⚙✓");

const QSet<QString> BundledFamilies{
    QStringLiteral("Noto Sans CJK SC"), QStringLiteral("DejaVu Sans"),
    QStringLiteral("Noto Sans Symbols"), QStringLiteral("Noto Sans Symbols2")
};

QFont uiFont(QFont::Weight weight = QFont::Normal)
{
    QFont font;
    // Match the application's stylesheet and initial application font. Let
    // Qt/Fontconfig choose real fallback runs instead of testing a font cmap.
    font.setFamilies({QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans"),
                      QStringLiteral("sans-serif")});
    font.setPixelSize(18);
    font.setWeight(weight);
    return font;
}

QString shapeErrors(const QString &text, const QFont &font, bool requireBundle,
                    QSet<QString> *usedFamilies)
{
    QTextLayout layout(text, font);
    layout.beginLayout();
    auto line = layout.createLine();
    if (line.isValid()) line.setLineWidth(4096);
    layout.endLayout();
    if (!line.isValid()) return QStringLiteral("Text shaping produced no line");
    const auto runs = layout.glyphRuns();
    if (runs.isEmpty()) return QStringLiteral("Text shaping produced no glyph runs");

    QStringList errors;
    qsizetype count = 0;
    for (const auto &run : runs) {
        const auto rawFont = run.rawFont();
        if (!rawFont.isValid()) {
            errors.append(QStringLiteral("Invalid raw font in shaped glyph run"));
            continue;
        }
        const auto family = rawFont.familyName();
        usedFamilies->insert(family);
        if (requireBundle && !BundledFamilies.contains(family))
            errors.append(QStringLiteral("Unbundled fallback font: %1").arg(family));
        const auto glyphs = run.glyphIndexes();
        count += glyphs.size();
        if (glyphs.isEmpty())
            errors.append(QStringLiteral("Empty glyph run from %1").arg(family));
        if (glyphs.size() != run.positions().size())
            errors.append(QStringLiteral("Incomplete glyph positions from %1").arg(family));
        for (const auto glyph : glyphs)
            if (glyph == 0)
                errors.append(QStringLiteral("Missing glyph (.notdef, glyph index 0) from %1").arg(family));
    }
    if (count == 0) errors.append(QStringLiteral("No shaped glyphs"));
    errors.removeDuplicates();
    return errors.join(QStringLiteral("; "));
}
}

class UiFontsTest : public QObject
{
    Q_OBJECT
private slots:
    void visibleCharactersShapeWithoutMissingGlyphs_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("weight");
        const QList<QFont::Weight> weights{QFont::Normal, QFont::Medium, QFont::DemiBold, QFont::Bold};
        for (const auto symbol : UiSymbols) {
            for (const auto weight : weights) {
                const auto name = QStringLiteral("U+%1-weight-%2")
                    .arg(uint(symbol.unicode()), 4, 16, QLatin1Char('0')).arg(int(weight));
                QTest::newRow(qPrintable(name)) << QString(symbol) << int(weight);
            }
        }
        const QStringList labels{
            QStringLiteral("⌂ 设备总览 ☆ 常用配件 ▣ 摄像头 ☼ 灯光"),
            QStringLiteral("⏻ 插座与开关 ⌁ 传感器 ◈ 场景 ◷ 自动化 ⚙ 家庭设置"),
            QStringLiteral("⌑ 北京 ↻ 刷新 ✓ 已连接 ● 在线 ○ 离线"),
            QStringLiteral("米 ＋ 钉 ⛶ ⊡ − × ▶ 直播 ↑ ↓ ← →"),
            QStringLiteral("温度 23.5 °C 湿度 60% CO₂ 450 ppm PM2.5 12 µg/m³"),
            QStringLiteral("MatterDesk MatterCamViewer 0123456789 2026/10/24 21:22:17")
        };
        for (qsizetype i = 0; i < labels.size(); ++i)
            QTest::newRow(qPrintable(QStringLiteral("mixed-ui-label-%1").arg(i)))
                << labels.at(i) << int(QFont::Medium);
    }

    void visibleCharactersShapeWithoutMissingGlyphs()
    {
        QFETCH(QString, text);
        QFETCH(int, weight);
        const bool requireBundle = qEnvironmentVariableIntValue("MATTERDESK_REQUIRE_BUNDLED_FONTS") == 1;
        QSet<QString> families;
        const auto errors = shapeErrors(text, uiFont(QFont::Weight(weight)), requireBundle, &families);
        QStringList sortedFamilies(families.begin(), families.end());
        sortedFamilies.sort();
        const auto diagnostic = QStringLiteral("%1; text=%2; shaped fonts=%3; FONTCONFIG_FILE=%4")
            .arg(errors, text, sortedFamilies.join(QStringLiteral(", ")),
                 qEnvironmentVariable("FONTCONFIG_FILE"));
        // Developers can run ordinary CTest on a host without the optional
        // symbols fonts. AppImage smoke tests always enable the strict gate.
        if (!errors.isEmpty() && !requireBundle)
            QSKIP(qPrintable(QStringLiteral("Host font coverage unavailable: %1. AppImage release gate is mandatory.").arg(diagnostic)));
        QVERIFY2(errors.isEmpty(), qPrintable(diagnostic));
        m_usedFamilies.unite(families);
    }

    void optionalFontGallery()
    {
        const auto directory = qEnvironmentVariable("MATTERDESK_FONT_SCREENSHOTS_DIR");
        if (directory.isEmpty()) return;
        QDir output(directory);
        QVERIFY2(output.exists() || QDir().mkpath(directory), "Could not create font gallery directory");

        QWidget gallery;
        gallery.setWindowTitle(QStringLiteral("MatterDesk / MatterCamViewer — packaged font regression"));
        gallery.setFont(uiFont());
        gallery.setStyleSheet(QStringLiteral(
            "QWidget { background:#eeedf1; color:#34343a; }"
            "QLabel { padding:8px; }"
            "QPushButton { padding:10px; border:1px solid #c9c4ce; border-radius:8px; background:#faf8fc; }"));
        auto *grid = new QGridLayout(&gallery);
        auto *title = new QLabel(QStringLiteral("MatterDesk / MatterCamViewer 字体与图标回归"));
        grid->addWidget(title, 0, 0, 1, 8);
        for (qsizetype i = 0; i < UiSymbols.size(); ++i) {
            const auto symbol = UiSymbols.at(i);
            const auto text = QString(symbol) + QStringLiteral("  U+%1").arg(uint(symbol.unicode()), 4, 16, QLatin1Char('0'));
            auto *button = new QPushButton(text);
            button->setFont(uiFont(QFont::Medium));
            grid->addWidget(button, int(i / 8) + 1, int(i % 8));
        }
        grid->addWidget(new QLabel(QStringLiteral("⌂ 家庭  ↻ 刷新  ⚙ 设置  ⏻ 电源  ⛶ 全屏  ⊡ 退出全屏")), 6, 0, 1, 8);
        grid->addWidget(new QLabel(QStringLiteral("中文：摄像头、灯光、传感器　23.5 °C　CO₂ 450 ppm　12 µg/m³")), 7, 0, 1, 8);
        gallery.resize(1280, 520);
        gallery.show();
        QApplication::processEvents();
        QVERIFY2(gallery.grab().save(output.filePath(QStringLiteral("ui-font-gallery.png"))), "Could not save font gallery");
    }

    void cleanupTestCase()
    {
        QStringList families(m_usedFamilies.begin(), m_usedFamilies.end());
        families.sort();
        qInfo().noquote() << "UI glyph shaping used:" << families.join(QStringLiteral(", "));
    }

private:
    QSet<QString> m_usedFamilies;
};

QTEST_MAIN(UiFontsTest)
#include "tst_uifonts.moc"
