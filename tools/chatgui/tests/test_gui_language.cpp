#include "../src/gui_language.h"
#include "../src/gui_theme.h"
#include "../src/addressprofiletext.h"
#include "../src/networksettingspage.h"
#include "../transport/node_config.h"
#include "../transport/config_updater.h"
#include "../../../lib/debug_config.h"

#include <QtTest>
#include <cmath>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QFile>
#include <QTemporaryDir>
#include <QTranslator>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QToolTip>

class GuiLanguageTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { debug_config_init(); }

    void themePersistence() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("theme.cfg");
        NodeConfig config(path); QVERIFY(config.create("Theme test"));
        QCOMPARE(config.theme(), QString("dark"));
        for (const QString& theme : {QString("light"), QString("dark")}) {
            QVERIFY(config_update_option(path, "gui", "theme", theme));
            QVERIFY(config.load()); QCOMPARE(config.theme(), theme);
            QVERIFY(config.save());
            NodeConfig restored(path); QVERIFY(restored.load()); QCOMPARE(restored.theme(), theme);
        }
        config.setTheme("invalid"); QVERIFY(!config.save());
        QVERIFY(config_update_option(path, "gui", "theme", "invalid"));
        QVERIFY(config.load()); QCOMPARE(config.theme(), QString("dark"));
        QVERIFY(config_update_option(path, "gui", "theme", "light"));
        QVERIFY(config.load()); QCOMPARE(config.theme(), QString("light"));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write("[gui]\n") > 0); file.close();
        QVERIFY(config.load()); QCOMPARE(config.theme(), QString("dark"));
    }

    void themeContrast() {
        auto luminance = [](const QColor& color) {
            auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
            return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
        };
        auto contrast = [&](const QColor& a, const QColor& b) {
            const double x = luminance(a), y = luminance(b);
            return (qMax(x, y) + .05) / (qMin(x, y) + .05);
        };
        for (const QString& theme : {QString("light"), QString("dark")}) {
            const auto p = guiThemePalette(theme);
            for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
                for (const auto pair : {qMakePair(QPalette::Text, QPalette::Base),
                     qMakePair(QPalette::WindowText, QPalette::Window), qMakePair(QPalette::ButtonText, QPalette::Button),
                     qMakePair(QPalette::HighlightedText, QPalette::Highlight), qMakePair(QPalette::ToolTipText, QPalette::ToolTipBase)}) {
                    QVERIFY2(contrast(p.color(group, pair.first), p.color(group, pair.second)) >= 4.5, qPrintable(theme));
                }
            }
            for (auto background : {QPalette::Window, QPalette::Base, QPalette::AlternateBase, QPalette::Light}) {
                QVERIFY(contrast(guiSecondaryText(p), p.color(background)) >= 4.5);
                QVERIFY(contrast(p.link().color(), p.color(background)) >= 4.5);
                QVERIFY(contrast(p.brightText().color(), p.color(background)) >= 4.5);
            }
            if (theme == "light") {
                QCOMPARE(p.text().color(), QColor(Qt::black)); QCOMPARE(p.windowText().color(), QColor(Qt::black));
                QCOMPARE(p.base().color(), QColor(Qt::white)); QCOMPARE(p.window().color(), QColor(Qt::white));
                QVERIFY(p.mid().color().lightness() >= 215);
            }
        }
    }

    void themeTooltips() {
        const auto previousLevel = g_debug_config.category_levels[DEBUG_CATEGORY_DEBUG];
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
        const auto previousPalette = QApplication::palette();
        const auto previousStyle = qApp->styleSheet();
        auto restore = qScopeGuard([&] {
            QToolTip::hideText();
            QApplication::setPalette(previousPalette); qApp->setStyleSheet(previousStyle);
            debug_set_category_level(DEBUG_CATEGORY_DEBUG, previousLevel);
        });
        applyGuiTheme("dark");
        QWidget header;
        header.setObjectName("messageHeader");
        header.setStyleSheet("QWidget#messageHeader { background: palette(window); }");
        QPushButton button("Header action", &header);
        button.setStyleSheet("QPushButton { border: none; background: transparent; }"
            "QPushButton:hover { background: palette(light); }");
        header.show();
        for (const QString& theme : {QString("dark"), QString("light"), QString("dark")}) {
            applyGuiTheme(theme);
            QToolTip::showText(button.mapToGlobal(QPoint(0, button.height())), "Header action " + theme, &button);
            QWidget* tooltip = nullptr;
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (widget->windowType() == Qt::ToolTip && widget->isVisible()) tooltip = widget;
            }
            QVERIFY(tooltip);
            const auto palette = tooltip->palette();
            const auto foreground = palette.color(QPalette::Inactive, tooltip->foregroundRole());
            const auto background = palette.color(QPalette::Inactive, tooltip->backgroundRole());
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "GUI tooltip: theme=%s foreground=%s background=%s roles=%d/%d",
                qPrintable(theme), qPrintable(foreground.name()), qPrintable(background.name()),
                int(tooltip->foregroundRole()), int(tooltip->backgroundRole()));
            const QString screenshots = qEnvironmentVariable("UTUN_THEME_SCREENSHOTS");
            if (!screenshots.isEmpty()) QVERIFY(tooltip->grab().save(screenshots + "/tooltip-" + theme + ".png"));
            const auto expected = guiThemePalette(theme);
            QCOMPARE(foreground, expected.color(QPalette::Inactive, QPalette::ToolTipText));
            QCOMPARE(background, expected.color(QPalette::Inactive, QPalette::ToolTipBase));
            QToolTip::hideText();
            QTRY_VERIFY(!QToolTip::isVisible());
        }
    }

    void utunPolicyPersistence() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("utun.cfg");
        NodeConfig config(path); QVERIFY(config.create("Chat only"));
        QVERIFY(!config.utunEnabled());
        config.setUtunEnabled(true); QVERIFY(config.save());
        NodeConfig restored(path); QVERIFY(restored.load());
        QVERIFY(!restored.hasErrors() && restored.utunEnabled());
        QVERIFY(restored.save() && restored.load() && restored.utunEnabled());
        QVERIFY(config_update_option(path, "gui", "utun_enabled", "no"));
        QVERIFY(restored.load() && !restored.utunEnabled());
        QVERIFY(config_update_option(path, "gui", "utun_enabled", "invalid"));
        QVERIFY(restored.load() && restored.hasErrors());
    }

    void localNetworkPermissions() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("local.cfg");
        QVERIFY(config_rewrite_servers(path, {}));
        NetworkSettingsPage page; page.loadFromConfig(path);
        auto* receive = page.findChild<QCheckBox*>("localInvitesEnabled");
        auto* broadcasts = page.findChild<QCheckBox*>("localBroadcastsEnabled");
        QVERIFY(receive && broadcasts);
        QVERIFY(page.localInvitesEnabled() && page.localBroadcastsEnabled());
        receive->setChecked(false); broadcasts->setChecked(false);
        QVERIFY(page.saveToConfig(path));
        NetworkSettingsPage restored; restored.loadFromConfig(path);
        QVERIFY(!restored.localInvitesEnabled() && !restored.localBroadcastsEnabled());
        receive->setChecked(true); QVERIFY(page.saveToConfig(path));
        restored.loadFromConfig(path);
        QVERIFY(restored.localInvitesEnabled() && !restored.localBroadcastsEnabled());
        QVERIFY(config_update_option(path, "chatserver", "local_broadcasts_enabled", "yes"));
        restored.loadFromConfig(path); QVERIFY(restored.localBroadcastsEnabled());
    }

    void systemPreference() {
        QCOMPARE(resolveGuiLanguage("system", {"ru-RU", "en-US"}), QString("ru"));
        QCOMPARE(resolveGuiLanguage("system", {"en-GB", "ru-RU"}), QString("en"));
        QCOMPARE(resolveGuiLanguage("system", {"de-DE", "ru_RU"}), QString("ru"));
        QCOMPARE(resolveGuiLanguage("system", {"fr-FR"}), QString("en"));
        QCOMPARE(resolveGuiLanguage("system", {}), QString("en"));
        QCOMPARE(resolveGuiLanguage("ru", {"en-US"}), QString("ru"));
        QCOMPARE(resolveGuiLanguage("en", {"ru-RU"}), QString("en"));
    }

    void addressProfilesKeepIndependentFields_data() {
        QTest::addColumn<int>("mapping"); QTest::addColumn<int>("inbound"); QTest::addColumn<int>("source");
        QTest::addColumn<QString>("expected");
        QTest::newRow("unmeasured") << int(ADDRESS_MAP_UNKNOWN) << int(ADDRESS_IN_UNKNOWN) << int(ADDRESS_SOURCE_NONE)
            << QStringLiteral("NAT not determined");
        QTest::newRow("unchanged-untested") << int(ADDRESS_MAP_REAL_IP) << int(ADDRESS_IN_UNKNOWN) << int(ADDRESS_SOURCE_ETCP)
            << QStringLiteral("No NAT; inbound untested");
        QTest::newRow("unchanged-open") << int(ADDRESS_MAP_REAL_IP) << int(ADDRESS_IN_OPEN) << int(ADDRESS_SOURCE_ETCP)
            << QStringLiteral("No NAT; inbound open");
        QTest::newRow("unchanged-filtered-stun") << int(ADDRESS_MAP_REAL_IP) << int(ADDRESS_IN_CONTACT) << int(ADDRESS_SOURCE_STUN)
            << QStringLiteral("No NAT; inbound filtered (STUN)");
        QTest::newRow("eim-untested-stun") << int(ADDRESS_MAP_EIM) << int(ADDRESS_IN_UNKNOWN) << int(ADDRESS_SOURCE_STUN)
            << QStringLiteral("EIM? (STUN)");
        QTest::newRow("eim-open") << int(ADDRESS_MAP_EIM) << int(ADDRESS_IN_OPEN) << int(ADDRESS_SOURCE_ETCP)
            << QStringLiteral("EIM; inbound open");
        QTest::newRow("eim-filtered") << int(ADDRESS_MAP_EIM) << int(ADDRESS_IN_CONTACT) << int(ADDRESS_SOURCE_ETCP)
            << QStringLiteral("EIM; inbound filtered");
        QTest::newRow("dependent-untested") << int(ADDRESS_MAP_DEPENDENT) << int(ADDRESS_IN_UNKNOWN) << int(ADDRESS_SOURCE_ETCP)
            << QStringLiteral("DEPENDENT");
        QTest::newRow("dependent-open") << int(ADDRESS_MAP_DEPENDENT) << int(ADDRESS_IN_OPEN) << int(ADDRESS_SOURCE_ETCP)
            << QStringLiteral("DEPENDENT; inbound open");
        QTest::newRow("dependent-filtered-stun") << int(ADDRESS_MAP_DEPENDENT) << int(ADDRESS_IN_CONTACT) << int(ADDRESS_SOURCE_STUN)
            << QStringLiteral("DEPENDENT; inbound filtered (STUN)");
        QTest::newRow("unknown-open-stun") << int(ADDRESS_MAP_UNKNOWN) << int(ADDRESS_IN_OPEN) << int(ADDRESS_SOURCE_STUN)
            << QStringLiteral("NAT?; inbound open (STUN)");
        QTest::newRow("unknown-filtered") << int(ADDRESS_MAP_UNKNOWN) << int(ADDRESS_IN_CONTACT) << int(ADDRESS_SOURCE_ETCP)
            << QStringLiteral("NAT?; inbound filtered");
    }

    void addressProfilesKeepIndependentFields() {
        QFETCH(int, mapping); QFETCH(int, inbound); QFETCH(int, source); QFETCH(QString, expected);
        const address_profile profile = {uint8_t(mapping), uint8_t(inbound), uint8_t(source)};
        QVERIFY(address_profile_valid(&profile));
        QCOMPARE(addressProfileText(mapping, inbound, source), expected);
    }

    void autoSocketProtocols() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("protocols.cfg");
        NodeConfig config(path);
        QVERIFY(config.create("Protocol test"));
        NetworkSettingsPage page; page.loadFromConfig(path);
        auto* protocols = page.findChild<QComboBox*>("autoSocketProtocols");
        QVERIFY(protocols && protocols->isEnabled());
        QCOMPARE(protocols->currentData().toString(), QString("tcp+udp"));
        for (const QString& value : {QString("tcp"), QString("udp"), QString("tcp+udp")}) {
            protocols->setCurrentIndex(protocols->findData(value));
            QVERIFY(page.saveToConfig(path));
            QVERIFY(config.load());
            QVERIFY2(!config.hasErrors(), qPrintable(config.errors().join('\n')));
            QCOMPARE(config.autoSocketProtocols(), value);
            QVERIFY(config.save());
            QVERIFY(config.load()); QCOMPARE(config.autoSocketProtocols(), value);
            QVERIFY(config.save());
            QVERIFY(config.load()); QCOMPARE(config.autoSocketProtocols(), value);
            page.loadFromConfig(path);
            QCOMPARE(protocols->currentData().toString(), value);
        }
        QVERIFY(config_update_option(path, "global", "auto_sockets", "no"));
        page.loadFromConfig(path); QVERIFY(!protocols->isEnabled());
        QVERIFY(config_update_option(path, "global", "auto_socket_protocols", "TCP # comment"));
        QVERIFY(config.load()); QVERIFY(!config.hasErrors());
        QCOMPARE(config.autoSocketProtocols(), QString("tcp"));
        page.loadFromConfig(path); QCOMPARE(protocols->currentData().toString(), QString("tcp"));
        QVERIFY(config_update_option(path, "global", "auto_socket_protocols", "invalid"));
        QVERIFY(config.load()); QVERIFY(config.hasErrors());
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray minimal("[global]\nauto_sockets=yes\n");
        QCOMPARE(file.write(minimal), qint64(minimal.size())); file.close();
        QVERIFY(config.load()); QCOMPARE(config.autoSocketProtocols(), QString("tcp+udp"));
        page.loadFromConfig(path); QCOMPARE(protocols->currentData().toString(), QString("tcp+udp"));
    }

    void preferencePersistence() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("vibechat.cfg");
        NodeConfig config(path);
        QVERIFY(config.create("Language test"));
        QCOMPARE(config.language(), QString("system"));
        for (const QString& language : {QString("ru"), QString("en"), QString("system")}) {
            QVERIFY(config_update_option(path, "gui", "language", language));
            QVERIFY(config.load());
            QVERIFY2(!config.hasErrors(), qPrintable(config.errors().join('\n')));
            QCOMPARE(config.language(), language);
            QVERIFY(config.save());
            NodeConfig reloaded(path);
            QVERIFY(reloaded.load());
            QCOMPARE(reloaded.language(), language);
            QVERIFY(reloaded.save());
            QVERIFY(reloaded.load());
            QCOMPARE(reloaded.language(), language);
        }
        QVERIFY(config_update_option(path, "gui", "language", "invalid"));
        QVERIFY(config.load());
        QCOMPARE(config.language(), QString("system"));
        QVERIFY(config_update_option(path, "gui", "language", "ru"));
        QVERIFY(config.load());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray minimalConfig("[gui]\ndb_path=chat_data\n");
        QCOMPARE(file.write(minimalConfig), qint64(minimalConfig.size()));
        file.close();
        QVERIFY(config.load());
        QCOMPARE(config.language(), QString("system"));
    }

    void embeddedCatalogs() {
        for (const QString& language : {QString("en"), QString("ru")}) {
            QTranslator translator;
            QVERIFY(translator.load(":/translations/chatgui_" + language + ".qm"));
            QVERIFY(qApp->installTranslator(&translator));
            QCOMPARE(QCoreApplication::translate("ChatGui", "Settings"), language == "ru" ? QString("Настройки") : QString("Settings"));
            QFile file(QStringLiteral(GUI_TS_DIR) + "/chatgui_" + language + ".ts");
            QVERIFY(file.open(QIODevice::ReadOnly));
            QXmlStreamReader xml(&file);
            QString source, context;
            int count = 0;
            const QRegularExpression placeholders("%(?:L?[1-9][0-9]?|n)");
            auto parameters = [&](const QString& text) {
                QStringList list;
                auto matches = placeholders.globalMatch(text);
                while (matches.hasNext()) list.append(matches.next().captured());
                list.sort();
                return list;
            };
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                if (xml.name() == QLatin1String("name")) context = xml.readElementText();
                if (xml.name() == QLatin1String("source")) source = xml.readElementText();
                if (xml.name() != QLatin1String("translation")) continue;
                QVERIFY(!xml.attributes().hasAttribute("type"));
                const QString translation = xml.readElementText();
                QVERIFY2(!translation.isEmpty(), qPrintable(source));
                QCOMPARE(parameters(translation), parameters(source));
                QVERIFY(!context.isEmpty());
                QCOMPARE(QCoreApplication::translate(context.toUtf8().constData(), source.toUtf8().constData()), translation);
                if (language == "en") QCOMPARE(translation, source);
                count++;
            }
            QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
            QVERIFY(count > 0);
            DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "embedded catalog verified language=%s messages=%d", qPrintable(language), count);
            qApp->removeTranslator(&translator);
        }
    }

    void standardButtons() {
        QTranslator translator;
        QVERIFY(translator.load(":/translations/qtbase_ru.qm"));
        QVERIFY(qApp->installTranslator(&translator));
        QDialogButtonBox buttons(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
        QCOMPARE(buttons.button(QDialogButtonBox::Cancel)->text().remove('&'), QString("Отмена"));
        QCOMPARE(buttons.button(QDialogButtonBox::Save)->text().remove('&'), QString("Сохранить"));
        qApp->removeTranslator(&translator);
    }

    void translatedNetworkValues_data() {
        QTest::addColumn<QString>("language");
        QTest::newRow("English") << QString("en");
        QTest::newRow("Russian") << QString("ru");
    }

    void translatedNetworkValues() {
        QFETCH(QString, language);
        QTranslator translator;
        QVERIFY(translator.load(":/translations/chatgui_" + language + ".qm"));
        QVERIFY(qApp->installTranslator(&translator));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("vibechat.cfg");
        QVERIFY(config_rewrite_servers(path, {{"test", "0.0.0.0:1333", "tcp", "private", ""}}));
        NetworkSettingsPage page;
        page.loadFromConfig(path);
        QComboBox* type = nullptr;
        for (auto* combo : page.findChildren<QComboBox*>()) {
            if (combo->findData(QString("public")) >= 0) type = combo;
        }
        QVERIFY(type);
        QCOMPARE(type->currentData().toString(), QString("private"));
        QCOMPARE(type->currentText(), language == "ru" ? QString("Частный") : QString("Private"));
        type->setCurrentIndex(type->findData(QString("local")));
        page.saveToConfig(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QVERIFY(content.contains("type=local\n"));
        QVERIFY(content.contains("addr=0.0.0.0:1333\n"));
        QVERIFY(content.contains("transport=tcp\n"));
        QVERIFY(!content.contains("netif="));
        qApp->removeTranslator(&translator);
    }
};

QTEST_MAIN(GuiLanguageTest)
#include "test_gui_language.moc"
