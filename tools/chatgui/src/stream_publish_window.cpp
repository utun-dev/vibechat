#include "stream_publish_window.h"
#include "stream_stats_window.h"
#include <QPointer>
#include "videoplayer_window.h"
#include "capture_controls.h"
#include "sound_manager.h"
#include "debug_config.h"
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QWidgetAction>
#include <QToolButton>
#include <QPainter>
#include <QSignalBlocker>
#include <QJsonDocument>
#include <QScopedValueRollback>
#include <QCloseEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QShortcut>
#include <algorithm>
#include <cmath>
#ifdef HAVE_STREAM_CAPTURE
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#endif

namespace {
constexpr int MaximumManualBitrate = 12000000;
QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }
QString bytes(uint64_t value) { return QString::number(value / 1024.0, 'f', 1) + " KiB"; }

using namespace CaptureControls;
}

/* Логарифмическая шкала непереданного времени 0..30 с; одна линия объединяет субпотоки получателя. */
class StreamBufferIndicator : public QWidget {
public:
    explicit StreamBufferIndicator(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("streamBufferIndicator"));
        auto* layout = new QHBoxLayout(this); layout->setContentsMargins(0, 4, 4, 23); layout->addStretch();
        m_value = new QLabel(this); m_value->setObjectName(QStringLiteral("streamBufferValue"));
        m_value->setFixedWidth(64); m_value->setAlignment(Qt::AlignCenter); layout->addWidget(m_value);
        setPositions({});
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop send buffer scale logarithmic max_s=30 detail_ms=50 live=right");
    }
    void setPositions(const stream_buffer_positions& positions) {
        m_positions = positions;
        QString description = text("Send buffer: %1 receivers. Logarithmic scale: 30 s on the left, live on the right.")
            .arg(positions.total);
        uint64_t maximum = 0; bool unknown = false;
        for (unsigned i = 0; i < positions.count; i++) {
            const auto& peer = positions.peers[i];
            if (peer.worst_us == UINT64_MAX) unknown = true;
            else maximum = std::max(maximum, peer.worst_us);
            QString duration = peer.worst_us == UINT64_MAX ? text("unknown")
                : text("%1–%2 s").arg(peer.best_us / 1000000.0, 0, 'f', 3).arg(peer.worst_us / 1000000.0, 0, 'f', 3);
            description += QStringLiteral("\n%1: %2 (%3)").arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(peer.peer.ed), 32).toHex())).arg(duration).arg(peer.subflows);
        }
        double seconds = maximum / 1000000.0;
        m_value->setText(!positions.count ? QStringLiteral("—") : unknown ? QStringLiteral("?")
            : text("%1 s").arg(locale().toString(seconds, 'f', seconds >= 10 ? 0 : 1)));
        m_value->setToolTip(text("Largest send buffer among receivers"));
        setToolTip(description); setAccessibleName(description); update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        QColor background = palette().window().color();
        painter.setPen(Qt::NoPen); painter.setBrush(background.lightness() < 128 ? background.lighter(135) : background.darker(106));
        painter.drawRoundedRect(rect(), 5, 5);
        QRectF area = QRectF(rect()).adjusted(16, 4, -76, -23);
        QFont scaleFont = painter.font(); scaleFont.setPixelSize(9); painter.setFont(scaleFont);
        painter.setPen(palette().windowText().color());
        for (double seconds : {30.0, 10.0, 3.0, 1.0, 0.3, 0.1, 0.0}) {
            double x = area.left() + area.width() * fraction(seconds * 1000000);
            painter.drawLine(QPointF(x, area.bottom() + 2), QPointF(x, area.bottom() + 6));
            QString label = QString::number(seconds) + (seconds == 30 ? QStringLiteral("s") : QString());
            double labelWidth = painter.fontMetrics().horizontalAdvance(label) + 2;
            double left = std::clamp(x - labelWidth / 2, 1.0, width() - labelWidth - 1);
            painter.drawText(QRectF(left, area.bottom() + 7, labelWidth, 13), Qt::AlignCenter, label);
        }
        if (!m_positions.count) {
            QColor color = palette().windowText().color(); color.setAlphaF(0.3);
            painter.setPen(QPen(color, 2)); painter.drawLine(QPointF(area.left(), area.center().y()), QPointF(area.right(), area.center().y()));
            return;
        }
        const QColor colors[] = {QColor(225, 91, 85, 160), QColor(225, 173, 54, 160), QColor(64, 184, 120, 160)};
        double pitch = area.height() / m_positions.count, thickness = std::min(4.0, pitch * 0.65);
        for (unsigned i = 0; i < m_positions.count; i++) {
            double y = area.top() + pitch * (i + 0.5);
            const double boundaries[] = {30, 3, 0.3, 0};
            for (unsigned zone = 0; zone < 3; zone++) {
                double left = area.left() + area.width() * fraction(boundaries[zone] * 1000000);
                double right = area.left() + area.width() * fraction(boundaries[zone + 1] * 1000000);
                painter.fillRect(QRectF(left, y - thickness / 2, right - left, thickness), colors[zone]);
            }
            if (m_positions.peers[i].worst_us == UINT64_MAX) {
                painter.setPen(palette().windowText().color());
                painter.drawText(area, Qt::AlignCenter, QStringLiteral("?")); continue;
            }
            double worst = area.left() + area.width() * fraction(m_positions.peers[i].worst_us);
            double best = area.left() + area.width() * fraction(m_positions.peers[i].best_us);
            painter.setPen(QPen(palette().windowText(), thickness)); painter.drawLine(QPointF(worst, y), QPointF(best, y));
            painter.setPen(QPen(palette().windowText(), 1));
            painter.drawLine(QPointF(worst, y - thickness), QPointF(worst, y + thickness));
            painter.drawLine(QPointF(best, y - thickness), QPointF(best, y + thickness));
        }
    }
private:
    QLabel* m_value;
    stream_buffer_positions m_positions{};
    double fraction(double duration_us) const {
        return 1 - std::log1p(std::clamp(duration_us, 0.0, 30000000.0) / 50000.0) / std::log1p(600.0);
    }
};

StreamPublishWindow::StreamPublishWindow(StreamPublishEngine* engine, const QString& group, QWidget* parent, const QString& saved)
    : QWidget(parent, Qt::Window), m_engine(engine), m_group(group) {
    if (!saved.isEmpty()) {
        QJsonParseError error; auto document = QJsonDocument::fromJson(saved.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop capture settings invalid: %s", qPrintable(error.errorString()));
        else m_settings = document.object();
    }
    m_audioMuted = m_settings.value("muted").toBool();
    setWindowTitle(text("Broadcast stream")); resize(900, 620); setMinimumSize(780, 360);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this); escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &QWidget::close);
    auto* layout = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout; controls->setSpacing(4);
    m_menuBar = new QMenuBar(this); m_menuBar->setNativeMenuBar(false); m_menuBar->setMouseTracking(true);
    auto* menuStyle = new MenuStyle; menuStyle->setParent(this); m_menuBar->setStyle(menuStyle);
    m_menuBar->setObjectName(QStringLiteral("streamControls"));
    auto makeMenu = [this](const char* heading, const char* name) {
        auto* menu = m_menuBar->addMenu(QString()); menu->setObjectName(QString::fromLatin1(name));
        menu->menuAction()->setToolTip(text(heading)); menu->setAccessibleName(text(heading));
        new QActionGroup(menu); menu->addSection(text(heading)); return menu;
    };
    m_source = makeMenu("Video source", "streamVideoSource");
    m_codec = makeMenu("Video codec", "streamVideoCodec");
    m_bitrate = makeMenu("Video bitrate", "streamVideoBitrate");
    m_resolution = makeMenu("Video resolution", "streamVideoResolution");
    m_fps = makeMenu("Frame rate", "streamVideoFps");
    m_rotation = makeMenu("Rotation", "streamVideoRotation");
    m_maxQp = makeMenu("Maximum QP", "streamMaxQp");
    m_microphone = makeMenu("Audio source", "streamAudioSource");
    auto* settingsMenu = makeMenu("Settings", "streamExtraSettings");
    settingsMenu->menuAction()->setIcon(controlIcon(palette(), "settings"));
    controls->addWidget(m_menuBar);
    m_sound = new QToolButton(this); m_sound->setCheckable(true); m_sound->setIconSize(QSize(40, 36)); m_sound->setFixedSize(48, 48);
    m_sound->setObjectName(QStringLiteral("streamSound")); m_sound->setAutoRaise(true); controls->addWidget(m_sound);
    controls->addStretch();
    auto* statisticsButton = new QPushButton(this);
    statisticsButton->setIcon(controlIcon(palette(), "statistics"));
    statisticsButton->setIconSize(QSize(40, 36)); statisticsButton->setFixedSize(48, 48); statisticsButton->setFlat(true);
    statisticsButton->setToolTip(QStringLiteral("Статистика трансляции")); statisticsButton->setAccessibleName(statisticsButton->toolTip());
    statisticsButton->setObjectName("streamStatistics"); statisticsButton->setEnabled(false); controls->addWidget(statisticsButton);
    auto statisticsWindow = std::make_shared<QPointer<StreamStatsWindow>>();
    auto statisticsEpoch = std::make_shared<uint64_t>(engine->epoch());
    statisticsButton->setEnabled(*statisticsEpoch != 0);
    connect(engine, &StreamPublishEngine::telemetryChanged, this, [statisticsEpoch,statisticsButton](const stream_publisher_status& status) {
        *statisticsEpoch = status.epoch; statisticsButton->setEnabled(status.epoch != 0);
    });
    connect(statisticsButton, &QPushButton::clicked, this, [this,statisticsWindow,statisticsEpoch] {
        if (!*statisticsWindow || (*statisticsWindow)->property("epoch").toULongLong() != *statisticsEpoch) {
            if (*statisticsWindow) delete statisticsWindow->data();
            *statisticsWindow = new StreamStatsWindow(!m_engine->group().isEmpty() ? m_engine->group() : m_group, *statisticsEpoch, this);
            (*statisticsWindow)->setProperty("epoch", QVariant::fromValue<qulonglong>(*statisticsEpoch));
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop stream statistics opened group=%s epoch=%llx",
            qPrintable(!m_engine->group().isEmpty() ? m_engine->group() : m_group), (unsigned long long)*statisticsEpoch);
        (*statisticsWindow)->show(); (*statisticsWindow)->raise(); (*statisticsWindow)->activateWindow();
    });
    m_start = new QPushButton(text("Start stream"), this); m_start->setMinimumHeight(40);
    m_start->setObjectName(QStringLiteral("streamStart"));
    m_buffer = new StreamBufferIndicator(this); m_buffer->setFixedSize(306, 48);
    controls->addWidget(m_buffer); controls->addWidget(m_start); layout->addLayout(controls);
#ifdef HAVE_STREAM_CAPTURE
    auto* preview = new VideoRenderWidget(this); m_preview = preview;
    preview->setMinimumHeight(160);
    preview->setFrame(engine->previewFrame().value<QImage>());
    connect(engine, &StreamPublishEngine::previewVideoChanged, this, [this, preview](const QVariant& frame) {
        auto image = frame.value<QImage>();
        if (image.isNull() || isVisible()) preview->setFrame(image);
    });
#else
    m_preview = new QLabel(text("Video capture requires Qt Multimedia and FFmpeg"), this);
#endif
    layout->addWidget(m_preview, 1);
    auto* settings = new QWidget(settingsMenu); auto* form = new QFormLayout(settings);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* settingsAction = new QWidgetAction(settingsMenu); settingsAction->setDefaultWidget(settings); settingsMenu->addAction(settingsAction);
    addOption(m_bitrate, text("Automatic"), 0);
    for (int bitrate : {250000, 500000, 1000000, 2000000, 4000000, 8000000, MaximumManualBitrate})
        addOption(m_bitrate, QStringLiteral("%1 Mbit/s").arg(bitrate / 1000000.0, 0, 'f', 2), bitrate);
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop manual stream bitrate selection maximum_bps=%d", MaximumManualBitrate);
    for (int angle : {0, 90, 270}) {
        auto* action = addOption(m_rotation, QStringLiteral("%1°").arg(angle), angle);
        action->setIcon(controlIcon(palette(), "rotation", action->text(), {}, angle));
    }
    m_cursor = new QCheckBox(text("Show mouse cursor"), settings); m_cursor->setChecked(true);
    m_cursor->setObjectName(QStringLiteral("streamShowCursor")); form->addRow(m_cursor);
    m_noAudio = addOption(m_microphone, text("No audio"), QVariant());
    addOption(m_microphone, text("Default microphone"), QString()); select(m_microphone, QString());
    refreshMicrophones();
    connect(m_microphone, &QMenu::aboutToShow, this, &StreamPublishWindow::refreshMicrophones);
    connect(SoundManager::instance(), &SoundManager::devicesChanged, this, [this] { refreshMicrophones(); synchronize(); });
    m_inputChannels = new QLabel(settings); form->addRow(text("Audio input"), m_inputChannels);
    for (int qp : {0, 18, 23, 28, 35, 40}) addOption(m_maxQp, qp ? QString::number(qp) : text("Automatic"), qp);
    select(m_maxQp, 28);
    m_maxQp->setToolTip(text("Lower QP preserves more detail and may require a higher bitrate. Applies to H.264 and H.265."));
    m_adaptiveQp = new QCheckBox(text("Adapt QP to the target bitrate"), settings);
    m_adaptiveQp->setObjectName(QStringLiteral("streamAdaptiveQp"));
    m_adaptiveQp->setToolTip(text(
        "Gradually raise the QP ceiling above the selected value when bitrate is too high, then return it. Requires live QP adjustment."));
    form->addRow(m_adaptiveQp);
    connect(m_sound, &QToolButton::toggled, this, [this](bool enabled) {
        m_audioMuted = !enabled;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream audio enabled=%d active=%d", enabled, m_engine->active());
        if (m_engine->active()) m_engine->setMuted(!enabled);
        else { updateMenuButtons(); saveSettings(); }
    });
    m_status = new QLabel(this); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_stats = new QLabel(this); m_stats->setObjectName(QStringLiteral("streamPublishStats"));
    m_stats->setWordWrap(true); m_stats->setMinimumHeight(66); layout->addWidget(m_stats);
    connect(m_start, &QPushButton::clicked, this, [this] {
        if (!m_engine->active()) m_status->setProperty("streamError", QString());
        if (m_engine->active()) m_engine->stop(); else m_engine->start(m_group, selection()); synchronize();
    });
    connect(m_source, &QMenu::triggered, this, [this](QAction* action) {
        if (!action->isCheckable()) return;
        if (m_updating) return;
        auto previous = selection();
        refreshModes(previous, previous.source.kind == StreamVideoSource::Screen); apply();
        if (!m_engine->active()) synchronize();
    });
    connect(m_resolution, &QMenu::triggered, this, [this] {
        populateFrameRates(m_fps, m_modes, value(m_resolution).toSize(), value(m_fps).toInt()); apply();
    });
    for (auto* menu : {m_fps, m_codec, m_bitrate, m_maxQp, m_rotation})
        connect(menu, &QMenu::triggered, this, &StreamPublishWindow::apply);
    connect(m_microphone, &QMenu::triggered, this, [this](QAction* action) {
        if (m_updating || !action->isCheckable()) return;
        const bool audio = action != m_noAudio;
        const bool audioChanged = m_engine->running() && audio != m_engine->config().audio;
        if (audio) {
            auto* sound = SoundManager::instance();
            const QString id = action->data().toString();
            if (id != sound->captureDevice()) {
                sound->setDevices(sound->playbackDevice(), id);
                emit microphoneChanged(id);
            }
        }
        if (audioChanged) m_engine->setAudioEnabled(audio);
        synchronize(); saveSettings();
    });
    for (auto* menu : {m_codec, m_maxQp}) connect(menu, &QMenu::triggered, this, [this] { if (!m_updating) synchronize(); });
    connect(m_cursor, &QCheckBox::toggled, this, &StreamPublishWindow::apply);
    connect(m_adaptiveQp, &QCheckBox::toggled, this, &StreamPublishWindow::apply);
    connect(engine, &StreamPublishEngine::codecsChanged, this, &StreamPublishWindow::refreshCodecs);
    connect(engine, &StreamPublishEngine::sourcesChanged, this, &StreamPublishWindow::refreshSources);
    connect(engine, &StreamPublishEngine::configurationChanged, this, [this] { synchronize(); saveSettings(); });
    connect(engine, &StreamPublishEngine::stateChanged, this, [this](const QString& value) {
        auto error = m_status->property("streamError").toString();
        m_status->setText(error.isEmpty() ? value : value + "\n" + error); synchronize();
    });
    connect(engine, &StreamPublishEngine::errorOccurred, this, [this](const QString& value) {
        m_status->setProperty("streamError", value); m_status->setText(value);
    });
    connect(engine, &StreamPublishEngine::softwareEncodingRequested, this,
        [this](uint64_t generation, const QString& codec, const QString& details) {
            auto* question = new QMessageBox(QMessageBox::Warning, text("Hardware encoding is unavailable"),
                text("Hardware encoding for %1 is unavailable in the selected video mode. Check the graphics driver or choose another codec.\n\n"
                     "Use CPU encoding instead? This may cause high CPU usage and slow down the computer.").arg(codec),
                QMessageBox::Yes | QMessageBox::No, this);
            question->setObjectName(QStringLiteral("streamCpuConfirmation"));
            question->setTextFormat(Qt::PlainText);
            question->setInformativeText(details.section("\n\n", 0, 0));
            question->setDefaultButton(QMessageBox::No); question->setEscapeButton(QMessageBox::No);
            question->setDetailedText(details); question->setAttribute(Qt::WA_DeleteOnClose);
            auto* engine = m_engine;
            connect(question, &QMessageBox::finished, engine, [engine, generation](int result) {
                engine->answerSoftwareEncoding(generation, result == QMessageBox::Yes);
            });
            connect(question, &QObject::destroyed, engine,
                [engine, generation] { engine->answerSoftwareEncoding(generation, false); }, Qt::QueuedConnection);
            connect(engine, &StreamPublishEngine::stateChanged, question, [engine, generation, question] {
                if (engine->softwareEncodingRequest() != generation) question->reject();
            });
            question->open();
        });
    connect(engine, &StreamPublishEngine::audioChannelsChanged, this, [this](int channels) { m_inputChannels->setText(channels == 2 ? text("Stereo") : text("Mono")); });
    connect(engine, &StreamPublishEngine::telemetryChanged, this, [this](const stream_publisher_status& stats) {
        m_buffer->setPositions(stats.buffer);
        m_stats->setText(text("Current video bitrate: %1 Mbit/s").arg(m_engine->videoBitrate() / 1000000.0, 0, 'f', 2) + '\n' +
            (m_engine->videoMaxQp() >= 0 ? text("Current QP ceiling: %1").arg(m_engine->videoMaxQp()) + '\n' : QString()) +
            (stats.source_overloaded ? text("CPU overloaded: publication is losing frames. Use Auto bitrate or lower video quality.") + '\n' : QString()) +
            text("Video queue: %1 · Inflight: %2 · Send buffer: %3\nACK, all connections, last second: %4")
            .arg(bytes(stats.video_bytes), bytes(stats.inflight_bytes), bytes(stats.total_bytes), bytes(stats.acked_1s)));
    });
    refreshSources(); refreshCodecs(); restoreSettings(); synchronize();
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop publisher control row rendered_icon=40x36 button=48x48 spacing=4 minimum_width=%d", minimumWidth());
}

void StreamPublishWindow::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
    if (event->isAccepted()) {
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop publisher controls closed group=%s active=%d",
            qPrintable(m_group), m_engine->active());
        emit closed();
    }
}

StreamPublishWindow::~StreamPublishWindow() { m_engine->setPreview(false); }

void StreamPublishWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event); updatePreview();
}

void StreamPublishWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event); m_engine->setPreview(false);
}

void StreamPublishWindow::updatePreview() {
    const bool enabled = isVisible() && selected(m_source) && selected(m_resolution) && selected(m_fps);
    m_engine->setPreview(enabled, enabled ? selection() : StreamPublishConfig{});
}

/* В БД храним пользовательский выбор, а не runtime handles источника и рассчитанный auto-битрейт. */
void StreamPublishWindow::restoreSettings() {
    if (m_settings.isEmpty() || m_engine->active()) return;
    m_updating = true;
    auto restore = [](QMenu* menu, const QVariant& data, const char* field) {
        if (!select(menu, data))
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop capture setting unavailable field=%s value=%s", field, qPrintable(data.toString()));
    };
    int sourceIndex = -1;
    for (int i = 0; i < m_sources.size(); i++) {
        const auto& source = m_sources[i];
        if (source.kind != m_settings.value("source_kind").toInt()) continue;
        bool matches = source.kind == StreamVideoSource::Window ? source.name == m_settings.value("source_name").toString()
            : source.id == QByteArray::fromBase64(m_settings.value("source_id").toString().toLatin1());
        if (matches) { sourceIndex = i; break; }
    }
    if (sourceIndex >= 0) select(m_source, sourceIndex);
    else DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop saved capture source unavailable; using current source");
    auto restored = selection();
    restored.video.width = m_settings.value("width").toInt(1280); restored.video.height = m_settings.value("height").toInt(720);
    restored.video.fps = m_settings.value("fps").toInt(30); restored.nativeSize = m_settings.value("native_size").toBool();
    refreshModes(restored, sourceIndex < 0 && restored.source.kind == StreamVideoSource::Screen);
    int bitrate = m_settings.value("bitrate").toInt();
    if (bitrate > MaximumManualBitrate) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop saved stream bitrate adjusted old_bps=%d maximum_bps=%d", bitrate, MaximumManualBitrate);
        bitrate = MaximumManualBitrate;
    }
    if (bitrate > 0 && !option(m_bitrate, bitrate))
        addOption(m_bitrate, QStringLiteral("%1 Mbit/s").arg(bitrate / 1000000.0, 0, 'f', 2), bitrate);
    restore(m_bitrate, bitrate, "bitrate");
    int maxQp = m_settings.value("max_qp").toInt(28);
    if (maxQp < 0 || maxQp > 51) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop saved QP ceiling invalid value=%d", maxQp);
    restore(m_maxQp, maxQp, "max_qp"); m_adaptiveQp->setChecked(m_settings.value("adaptive_qp").toBool());
    restore(m_rotation, m_settings.value("rotation").toInt(), "rotation");
    m_cursor->setChecked(m_settings.value("show_cursor").toBool(true));
    if (!m_settings.value("audio").toBool(true)) m_noAudio->setChecked(true);
    m_updating = false;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop capture settings restored: %s", QJsonDocument(m_settings).toJson(QJsonDocument::Compact).constData());
}

void StreamPublishWindow::saveSettings() {
    if (m_updating) return;
    auto config = selection(); auto saved = m_settings;
    if (selected(m_source)) {
        saved["source_kind"] = config.source.kind;
        saved["source_id"] = QString::fromLatin1(config.source.id.toBase64());
        saved["source_name"] = config.source.kind == StreamVideoSource::Window ? config.source.name : QString();
    }
    if (selected(m_resolution) && selected(m_fps)) {
        saved["width"] = config.video.width; saved["height"] = config.video.height; saved["fps"] = config.video.fps;
        saved["native_size"] = config.nativeSize;
    }
    if (selected(m_codec)) saved["codec"] = config.video.codec;
    saved["bitrate"] = value(m_bitrate).toInt(); saved["rotation"] = config.video.rotation;
    saved["audio"] = config.audio; saved.remove("microphone");
    saved["muted"] = m_audioMuted;
    saved["show_cursor"] = config.showCursor;
    saved["max_qp"] = value(m_maxQp).toInt(); saved["adaptive_qp"] = m_adaptiveQp->isChecked();
    if (saved == m_settings) return;
    m_settings = saved;
    auto json = QJsonDocument(saved).toJson(QJsonDocument::Compact);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop capture settings selected: %s", json.constData());
    emit settingsChanged(QString::fromUtf8(json));
}

void StreamPublishWindow::refreshSources() {
    QScopedValueRollback<bool> updating(m_updating, true);
    auto config = m_engine->active() ? m_engine->config() : selection();
    m_sources = m_engine->sources(); clearOptions(m_source, "Video source");
    int selected = -1;
    for (int i = 0; i < m_sources.size(); i++) {
        auto& source = m_sources[i];
        if (source.sameSource(config.source)) {
            selected = i;
            if (m_engine->active() && !config.source.size.isEmpty()) source.size = config.source.size;
        }
        QString kind = source.kind == StreamVideoSource::Camera ? text("Camera") : source.kind == StreamVideoSource::Screen ? text("Screen") : text("Window");
        QString label = kind + ": " + source.name;
        if (source.kind == StreamVideoSource::Screen && !source.size.isEmpty())
            label += QStringLiteral(" (%1 × %2)").arg(source.size.width()).arg(source.size.height());
        addOption(m_source, label, i);
    }
    bool changed = selected < 0;
    if (changed && !m_engine->active() && !m_sources.isEmpty()) selected = 0;
    if (selected >= 0) select(m_source, selected);
    else if (auto* action = ::selected(m_source)) {
        auto* group = m_source->findChild<QActionGroup*>(); group->setExclusive(false); action->setChecked(false); group->setExclusive(true);
    }
    m_source->addSeparator();
    connect(m_source->addAction(text("Refresh")), &QAction::triggered, m_engine, &StreamPublishEngine::refreshSources);
    refreshModes(config, changed && selected >= 0 && m_sources[selected].kind == StreamVideoSource::Screen);
    synchronize();
}

/* Перестраиваем весь список под блокировкой, чтобы в engine не ушёл промежуточный режим другого источника. */
void StreamPublishWindow::refreshModes(const StreamPublishConfig& previous, bool maximum) {
    QScopedValueRollback<bool> updating(m_updating, true);
    clearOptions(m_resolution, "Video resolution"); clearOptions(m_fps, "Frame rate"); m_modes.clear();
    int sourceIndex = selected(m_source) ? value(m_source).toInt() : -1;
    if (sourceIndex < 0 || sourceIndex >= m_sources.size()) return;
    const auto& source = m_sources[sourceIndex];
    if (source.kind == StreamVideoSource::Camera) m_modes = source.modes;
    else if (source.kind == StreamVideoSource::Screen) m_modes = stream_video_screen_modes(source.size);
    else {
        for (QSize size : {QSize(1920, 1080), QSize(1280, 720), QSize(640, 360)})
            for (int fps : {15, 24, 25, 30, 50, 60}) m_modes.append({size, fps});
    }
    for (const auto& mode : m_modes) {
        if (option(m_resolution, mode.size)) continue;
        QString label = mode.size.isEmpty() ? text("Original resolution")
            : QStringLiteral("%1 × %2").arg(mode.size.width()).arg(mode.size.height());
        addOption(m_resolution, label, mode.size);
    }
    maximum |= source.kind == StreamVideoSource::Screen && previous.nativeSize;
    StreamVideoMode wanted{QSize(previous.video.width, previous.video.height), previous.video.fps};
    int selected = stream_video_select_mode(m_modes, wanted, maximum);
    if (selected < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop source has no selectable modes source=%s", qPrintable(source.name));
        m_resolution->menuAction()->setToolTip(text("No supported video modes")); return;
    }
    const auto& mode = m_modes[selected];
    select(m_resolution, mode.size); populateFrameRates(m_fps, m_modes, mode.size, mode.fps);
    if (!maximum && wanted != mode) DEBUG_WARN(DEBUG_CATEGORY_VIDEO,
        "desktop video mode adjusted source=%s requested=%dx%d/%d selected=%dx%d/%d",
        qPrintable(source.name), wanted.size.width(), wanted.size.height(), wanted.fps, mode.size.width(), mode.size.height(), mode.fps);
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop video modes source=%s count=%lld selected=%dx%d/%d native=%d",
        qPrintable(source.name), (long long)m_modes.size(), mode.size.width(), mode.size.height(), mode.fps,
        source.kind == StreamVideoSource::Screen && mode.size == m_modes.front().size);
}

void StreamPublishWindow::refreshMicrophones() {
    QScopedValueRollback<bool> updating(m_updating, true);
    const bool audio = m_engine->active() ? m_engine->config().audio : selected(m_microphone) != m_noAudio;
    const QString input = SoundManager::instance()->captureDevice();
    clearOptions(m_microphone, "Audio source");
    m_noAudio = addOption(m_microphone, text("No audio"), QVariant());
    m_noAudio->setEnabled(!m_engine->active() || m_engine->running());
    addOption(m_microphone, text("Default microphone"), QString());
    for (const auto& device : SoundManager::enumerateCaptureDevices()) addOption(m_microphone, device.name, device.id);
    if (!option(m_microphone, input)) addOption(m_microphone, text("Selected microphone unavailable"), input);
    if (audio) select(m_microphone, input); else m_noAudio->setChecked(true);
}

void StreamPublishWindow::refreshCodecs() {
    QScopedValueRollback<bool> updating(m_updating, true);
    int previous = selected(m_codec) ? value(m_codec).toInt() : m_settings.value("codec").toInt();
    clearOptions(m_codec, "Video codec");
    for (int codec : m_engine->codecs()) addOption(m_codec, LiveVideoEncoder::codecName(codec), codec);
    if (!select(m_codec, previous) && previous && selected(m_codec))
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop saved codec=%d unavailable; using codec=%d", previous, value(m_codec).toInt());
    synchronize();
}
StreamPublishConfig StreamPublishWindow::selection() const {
    StreamPublishConfig config;
    int sourceIndex = selected(m_source) ? value(m_source).toInt() : -1;
    if (sourceIndex >= 0 && sourceIndex < m_sources.size()) config.source = m_sources[sourceIndex];
    QSize size = value(m_resolution).toSize();
    if (!size.isEmpty()) { config.video.width = size.width(); config.video.height = size.height(); }
    if (selected(m_fps)) config.video.fps = value(m_fps).toInt();
    config.nativeSize = config.source.kind == StreamVideoSource::Screen && !m_modes.isEmpty() && size == m_modes.front().size;
    config.video.codec = value(m_codec).toInt(); config.video.rotation = value(m_rotation).toInt();
    config.video.bitrate = value(m_bitrate).toInt();
    config.automaticBitrate = !config.video.bitrate;
    if (config.automaticBitrate) config.video.bitrate = std::clamp(config.video.width * config.video.height * config.video.fps / 11, 250000, 16000000);
    if (config.video.codec == 1 || config.video.codec == 2) {
        config.video.maxQp = value(m_maxQp).toInt() ? value(m_maxQp).toInt() : -1;
        config.adaptiveQp = config.video.maxQp >= 0 && m_adaptiveQp->isChecked();
    }
    config.showCursor = m_cursor->isChecked();
    config.audio = selected(m_microphone) != m_noAudio;
    config.startMuted = m_audioMuted; return config;
}
void StreamPublishWindow::apply() {
    if (m_updating || !selected(m_resolution) || !selected(m_fps)) return;
    if (!m_engine->active()) { m_status->setProperty("streamError", QString()); m_status->clear(); }
    if (m_engine->active()) m_engine->reconfigure(selection());
    if (!m_engine->active()) synchronize(); else updateMenuButtons();
    saveSettings();
}
void StreamPublishWindow::synchronize() {
    QScopedValueRollback<bool> updating(m_updating, true); bool active = m_engine->active();
    if (!active) m_buffer->setPositions({});
    m_start->setText(active ? text("Stop stream") : text("Start stream"));
    m_start->setEnabled((active && m_engine->running()) || (!active && StreamPublishEngine::supported()
        && selected(m_source) && selected(m_resolution) && selected(m_fps) && selected(m_codec)));
    for (auto* menu : {m_source, m_resolution, m_fps, m_codec, m_bitrate, m_rotation})
        menu->menuAction()->setEnabled(!active || m_engine->running());
    m_microphone->menuAction()->setEnabled(!active || m_engine->running());
    m_noAudio->setEnabled(!active || m_engine->running());
    if (active) m_audioMuted = m_engine->muted();
    bool audio = active ? m_engine->config().audio : selection().audio;
    m_sound->setEnabled(audio && (!active || m_engine->running()));
    { QSignalBlocker blocked(m_sound); m_sound->setChecked(audio && !m_audioMuted); }
    if (active) {
        auto config = m_engine->config();
        for (int i = 0; i < m_sources.size(); i++) if (m_sources[i].sameSource(config.source)) {
                select(m_source, i); if (!config.source.size.isEmpty()) m_sources[i].size = config.source.size;
            }
        refreshModes(config);
        select(m_codec, config.video.codec);
        int bitrate = config.automaticBitrate ? 0 : config.video.bitrate;
        if (!option(m_bitrate, bitrate)) addOption(m_bitrate, QStringLiteral("%1 Mbit/s").arg(bitrate / 1000000.0, 0, 'f', 2), bitrate);
        select(m_bitrate, bitrate);
        if (config.video.codec == 1 || config.video.codec == 2) {
            int qp = config.video.maxQp < 0 ? 0 : config.video.maxQp;
            if (!select(m_maxQp, qp)) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop active QP ceiling unavailable value=%d", qp);
            m_adaptiveQp->setChecked(config.adaptiveQp);
        }
        select(m_rotation, config.video.rotation);
        if (config.audio) select(m_microphone, SoundManager::instance()->captureDevice()); else m_noAudio->setChecked(true);
        m_cursor->setChecked(config.showCursor);
    }
    m_cursor->setVisible(StreamPublishEngine::cursorSupported(selection().source));
    bool qpSupported = value(m_codec).toInt() == 1 || value(m_codec).toInt() == 2;
    m_maxQp->menuAction()->setEnabled(qpSupported && (!active || m_engine->running()));
    m_adaptiveQp->setEnabled(m_maxQp->menuAction()->isEnabled() && value(m_maxQp).toInt() > 0);
    bool portal = selection().source.id == "portal";
    m_cursor->setEnabled(!active || (m_engine->running() && !portal));
    m_cursor->setToolTip(portal ? text("Change cursor visibility before starting screen sharing") : QString());
    updateMenuButtons();
    updatePreview();
}

void StreamPublishWindow::updateMenuButtons() {
    auto update = [this](QMenu* menu, const char* heading, const QString& shape, const QString& label = {}, const QString& detail = {}, int rotation = 0) {
        auto colors = palette();
        if (shape == "microphone-muted") colors.setColor(QPalette::WindowText, QColor(220, 60, 60));
        menu->menuAction()->setIcon(controlIcon(colors, shape, label, detail, rotation));
        QString description = text(heading);
        if (auto* action = selected(menu)) description += QStringLiteral(": ") + action->text();
        menu->menuAction()->setToolTip(description); menu->setAccessibleName(description);
    };
    auto config = selection();
    update(m_source, "Video source", config.source.kind == StreamVideoSource::Camera ? "camera" : "screen", config.source.name);
    update(m_codec, "Video codec", "chip", selected(m_codec) ? LiveVideoEncoder::codecName(config.video.codec).remove('.') : "—");
    QString bitrate = config.video.bitrate >= 1000000 ? QString::number(config.video.bitrate / 1000000.0, 'g', 2) + 'M'
        : QString::number(config.video.bitrate / 1000) + 'k';
    update(m_bitrate, "Video bitrate", "gauge", bitrate, config.automaticBitrate ? "A" : QString());
    QSize size = value(m_resolution).toSize();
    update(m_resolution, "Video resolution", "screen", size.isEmpty() ? "Auto" : QString::number(std::min(size.width(), size.height())) + 'p');
    update(m_fps, "Frame rate", "fps", selected(m_fps) ? QString::number(config.video.fps) : "—", "FPS");
    update(m_rotation, "Rotation", "rotation", QStringLiteral("%1°").arg(config.video.rotation), {}, config.video.rotation);
    update(m_maxQp, "Maximum QP", "qp", value(m_maxQp).toInt() ? value(m_maxQp).toString() : "Auto", "QP");
    update(m_microphone, "Audio source", config.audio ? "microphone" : "microphone-muted", selected(m_microphone)->text());
    m_sound->setIcon(controlIcon(palette(), m_sound->isChecked() ? "microphone" : "microphone-muted", QStringLiteral("Mute")));
    m_sound->setToolTip(QStringLiteral("Mute"));
    m_sound->setAccessibleName(m_sound->toolTip());
    m_menuBar->setFixedWidth(m_menuBar->sizeHint().width());
    setMinimumWidth(layout()->minimumSize().width());
}
