#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QLibrary>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWindow>
#include <QtTest>

namespace {
class InputField : public QLineEdit
{
public:
    QStringList preedits;
    QStringList commits;

protected:
    void inputMethodEvent(QInputMethodEvent *event) override
    {
        if (!event->preeditString().isEmpty()) preedits.append(event->preeditString());
        if (!event->commitString().isEmpty()) commits.append(event->commitString());
        QLineEdit::inputMethodEvent(event);
    }
};

bool activatePinyin()
{
    for (const auto &arguments : {QStringList{QStringLiteral("-s"), QStringLiteral("pinyin")},
                                  QStringList{QStringLiteral("-o")}}) {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        // fcitx5-remote belongs to the host daemon, not the AppImage. Keep only
        // the private bus/display routing; do not inject bundled old libraries
        // or Qt plugins into a newer host utility.
        for (const auto &name : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                 "FONTCONFIG_FILE", "FONTCONFIG_PATH", "APPDIR"})
            environment.remove(QString::fromLatin1(name));
        process.setProcessEnvironment(environment);
        process.start(QStringLiteral("fcitx5-remote"), arguments);
        if (!process.waitForFinished(5000) || process.exitCode() != 0) return false;
    }
    return true;
}

class NativeKeyboard
{
public:
    NativeKeyboard() : m_x11(QStringLiteral("libX11.so.6")), m_xtst(QStringLiteral("libXtst.so.6"))
    {
        const auto open = reinterpret_cast<void *(*)(const char *)>(m_x11.resolve("XOpenDisplay"));
        m_close = reinterpret_cast<int (*)(void *)>(m_x11.resolve("XCloseDisplay"));
        m_keysym = reinterpret_cast<unsigned long (*)(const char *)>(m_x11.resolve("XStringToKeysym"));
        m_keycode = reinterpret_cast<unsigned char (*)(void *, unsigned long)>(m_x11.resolve("XKeysymToKeycode"));
        m_flush = reinterpret_cast<int (*)(void *)>(m_x11.resolve("XFlush"));
        m_event = reinterpret_cast<int (*)(void *, unsigned int, int, unsigned long)>(m_xtst.resolve("XTestFakeKeyEvent"));
        if (open && m_close && m_keysym && m_keycode && m_flush && m_event) m_display = open(nullptr);
    }
    ~NativeKeyboard() { if (m_display) m_close(m_display); }
    bool valid() const { return m_display != nullptr; }
    bool click(const QByteArray &name)
    {
        if (!m_display) return false;
        const auto code = m_keycode(m_display, m_keysym(name.constData()));
        if (!code) return false;
        // XTest feeds actual native X11 keycodes to the Xvfb display. Direct
        // QTest/QKeyEvent delivery has nativeVirtualKey=0 and bypasses Pinyin.
        if (!m_event(m_display, code, true, 0) || !m_event(m_display, code, false, 0)) return false;
        m_flush(m_display);
        QTest::qWait(80);
        return true;
    }
    bool type(const QByteArray &syllables)
    {
        for (const auto character : syllables)
            if (!click(QByteArray(1, character))) return false;
        return true;
    }
    bool chord(const QByteArray &modifier, const QByteArray &key)
    {
        if (!m_display) return false;
        const auto modifierCode = m_keycode(m_display, m_keysym(modifier.constData()));
        const auto keyCode = m_keycode(m_display, m_keysym(key.constData()));
        if (!modifierCode || !keyCode) return false;
        const bool sent = m_event(m_display, modifierCode, true, 0)
            && m_event(m_display, keyCode, true, 0)
            && m_event(m_display, keyCode, false, 0)
            && m_event(m_display, modifierCode, false, 0);
        m_flush(m_display);
        QTest::qWait(100);
        return sent;
    }
private:
    QLibrary m_x11, m_xtst;
    void *m_display = nullptr;
    int (*m_close)(void *) = nullptr;
    unsigned long (*m_keysym)(const char *) = nullptr;
    unsigned char (*m_keycode)(void *, unsigned long) = nullptr;
    int (*m_flush)(void *) = nullptr;
    int (*m_event)(void *, unsigned int, int, unsigned long) = nullptr;
};
}

class InputMethodTest : public QObject
{
    Q_OBJECT
private slots:
    void nativeFieldsAcceptPreeditAndChineseCommit()
    {
        InputField field;
        QSignalSpy returnPressed(&field, &QLineEdit::returnPressed);
        QInputMethodEvent preedit(QStringLiteral("nihao"), {});
        QApplication::sendEvent(&field, &preedit);
        QCOMPARE(field.text(), QString());
        QCOMPARE(field.preedits, QStringList{QStringLiteral("nihao")});
        QCOMPARE(returnPressed.count(), 0);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("你好"));
        QApplication::sendEvent(&field, &commit);
        QCOMPARE(field.text(), QStringLiteral("你好"));
        QCOMPARE(field.commits, QStringList{QStringLiteral("你好")});
        QCOMPARE(returnPressed.count(), 0);
    }

    void packagedFcitxPinyinCommitsAfterFocusChanges()
    {
        if (qEnvironmentVariableIntValue("MATTERDESK_REQUIRE_INPUT_METHOD") != 1)
            QSKIP("Real Fcitx integration is run by tests/run-inputmethod-smoke.py in a private D-Bus/Xvfb session");
        QVERIFY2(qEnvironmentVariableIntValue("MATTERDESK_TEST_PRIVATE_DBUS") == 1,
                 "Real IME tests must run in the isolated test session, never the user's desktop session");
        QVERIFY2(!qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS").isEmpty(), "Missing private D-Bus session");
        QCOMPARE(qEnvironmentVariable("QT_IM_MODULE"), QStringLiteral("fcitx"));
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        NativeKeyboard keyboard;
        QVERIFY2(keyboard.valid(), "Missing native XTest keyboard injection in the isolated Xvfb display");

        QFile maps(QStringLiteral("/proc/self/maps"));
        QVERIFY(maps.open(QIODevice::ReadOnly));
        const auto loaded = maps.readAll();
        const auto pluginName = QByteArray("libfcitx5platforminputcontextplugin.so");
        QVERIFY2(loaded.contains(pluginName), "Qt did not load the real Fcitx5 platform input context plugin");
        for (const auto &mapping : loaded.split('\n')) {
            if (!mapping.contains(pluginName)) continue;
            const auto pathStart = mapping.indexOf('/');
            if (pathStart >= 0)
                qInfo().noquote() << "Loaded Qt Fcitx platform input context:" << QString::fromUtf8(mapping.mid(pathStart));
            break;
        }
        const auto pluginRoot = qEnvironmentVariable("MATTERDESK_INPUTMETHOD_PLUGIN_ROOT");
        if (!pluginRoot.isEmpty()) {
            const auto packagedPlugin = QFileInfo(pluginRoot + QLatin1Char('/') + QString::fromLatin1(pluginName)).canonicalFilePath();
            QVERIFY2(!packagedPlugin.isEmpty(), "The AppImage does not contain its required Fcitx5 Qt plugin");
            QVERIFY2(loaded.contains(packagedPlugin.toUtf8()), "Qt loaded a host IME plugin instead of the packaged plugin");
        }

        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        InputField first, second;
        layout->addWidget(&first);
        layout->addWidget(&second);
        QSignalSpy firstReturns(&first, &QLineEdit::returnPressed);
        QSignalSpy secondReturns(&second, &QLineEdit::returnPressed);
        window.resize(480, 120);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        first.setFocus();
        QTRY_VERIFY(first.hasFocus());
        QTest::qWait(300);
        QVERIFY2(activatePinyin(), "Isolated Fcitx5 could not activate the Pinyin input method");

        QVERIFY(keyboard.type("nihao"));
        QTRY_VERIFY_WITH_TIMEOUT(!first.preedits.isEmpty(), 5000);
        QCOMPARE(first.text(), QString());
        QCOMPARE(firstReturns.count(), 0);
        QVERIFY(keyboard.click("space"));
        QTRY_COMPARE_WITH_TIMEOUT(first.text(), QStringLiteral("你好"), 5000);
        QVERIFY(!first.commits.isEmpty());
        QCOMPARE(firstReturns.count(), 0);

        second.setFocus();
        QTRY_VERIFY(second.hasFocus());
        QVERIFY(activatePinyin());
        QVERIFY(keyboard.type("zhongwen"));
        QTRY_VERIFY_WITH_TIMEOUT(!second.preedits.isEmpty(), 5000);
        QCOMPARE(second.text(), QString());
        QVERIFY(keyboard.click("space"));
        QTRY_COMPARE_WITH_TIMEOUT(second.text(), QStringLiteral("中文"), 5000);
        QCOMPARE(first.text(), QStringLiteral("你好"));
        QCOMPARE(secondReturns.count(), 0);

        // Exercise the user's normal input-method hotkey as well as daemon
        // activation: Latin input after Ctrl+Space, then Pinyin after toggling
        // it back on. This display has no desktop/global shortcut handler.
        const auto latinPreeditCount = second.preedits.size();
        QVERIFY(keyboard.chord("Control_L", "space"));
        QVERIFY(keyboard.type("abc"));
        QTRY_COMPARE(second.text(), QStringLiteral("中文abc"));
        QCOMPARE(second.preedits.size(), latinPreeditCount);
        QVERIFY(keyboard.chord("Control_L", "space"));

        // Return completes the current composition; it must not reach the
        // application's returnPressed handlers as a premature form submit.
        const auto preeditCount = second.preedits.size();
        QVERIFY(keyboard.type("ceshi"));
        QTRY_VERIFY(second.preedits.size() > preeditCount);
        QVERIFY(keyboard.click("Return"));
        QTest::qWait(300);
        QCOMPARE(secondReturns.count(), 0);
        QVERIFY(second.commits.size() >= 2);
        qInfo() << "Packaged Fcitx5 plugin completed real Pinyin preedit, Chinese commit, focus switch, Ctrl+Space toggle, and composition Return";
    }
};

QTEST_MAIN(InputMethodTest)
#include "tst_inputmethod.moc"
