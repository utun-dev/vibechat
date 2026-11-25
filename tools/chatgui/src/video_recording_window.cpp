#include "video_recording_window.h"
#include "capture_controls.h"
#include "videoplayer_window.h"
#include "videoplayer_engine.h"
#include "sound_manager.h"
#include "inputbar.h"
#include "debug_config.h"
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QMessageBox>
#include <QCloseEvent>
#include <QShortcut>
#include <QJsonDocument>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QPointer>
#include <QTransform>
#include <QDir>
#include <QCoreApplication>
#include <cstring>
#include <QRunnable>
#include <algorithm>
extern "C" {
#include "media_async/attachment_send.h"
#include "mem.h"
}
namespace {
QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }
using namespace CaptureControls;
}
VideoRecordingWindow::VideoRecordingWindow(const QString& target, const QString& recipient, quint64 generation, const QByteArray& reply,
                                         const QString& settings, const QString& card, QWidget* parent, const QByteArray& quote)
    : QWidget(parent, Qt::Window), m_target(target), m_card(card), m_generation(generation), m_reply(reply), m_quote(quote),
      m_engine(new VideoRecordingEngine(this)), m_player(new VideoPlayerEngine(this)) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(text("Record video")); resize(940, 650);
    m_saved = QJsonDocument::fromJson(settings.toUtf8()).object(); m_jobs.setMaxThreadCount(1);
    auto* layout = new QVBoxLayout(this);
    auto* recipientLabel = new QLabel(text("Recipient: %1").arg(recipient), this); recipientLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(recipientLabel);
    auto* bar = new QMenuBar(this); bar->setNativeMenuBar(false);
    auto* style = new MenuStyle; style->setParent(this); bar->setStyle(style);
    auto menu = [bar](const char* heading, const char* name) {
        auto* result = bar->addMenu(QString()); result->setObjectName(name); new QActionGroup(result);
        result->addSection(text(heading)); result->menuAction()->setToolTip(text(heading)); return result;
    };
    m_source = menu("Video source", "recordingSource"); m_codec = menu("Video codec", "recordingCodec");
    m_bitrate = menu("Video bitrate", "recordingBitrate"); m_resolution = menu("Video resolution", "recordingResolution");
    m_fps = menu("Frame rate", "recordingFps"); m_rotation = menu("Rotation", "recordingRotation");
    m_qp = menu("Maximum QP", "recordingQp"); m_microphone = menu("Audio source", "recordingMicrophone");
    layout->addWidget(bar);
    for (int codec : LiveVideoEncoder::available()) if (codec <= 4) addOption(m_codec, LiveVideoEncoder::codecName(codec), codec);
    addOption(m_bitrate, text("Automatic"), 0);
    for (int bitrate : {250000, 500000, 1000000, 2000000, 4000000, 8000000, 12000000})
        addOption(m_bitrate, QStringLiteral("%1 Mbit/s").arg(bitrate / 1000000.0), bitrate);
    for (int qp : {0, 18, 23, 28, 35, 40}) addOption(m_qp, qp ? QString::number(qp) : text("Automatic"), qp);
    for (int rotation : {0, 90, 270}) addOption(m_rotation, QStringLiteral("%1°").arg(rotation), rotation);
    addOption(m_microphone, text("No audio"), QVariant()); addOption(m_microphone, text("Default microphone"), QString());
    for (const auto& device : SoundManager::enumerateCaptureDevices()) addOption(m_microphone, device.name, device.id);
    select(m_microphone, SoundManager::instance()->captureDevice());
    auto* row = new QHBoxLayout;
    m_cursor = new QCheckBox(text("Show mouse cursor"), this); row->addWidget(m_cursor);
    m_adaptive = new QCheckBox(text("Adapt QP to the target bitrate"), this); row->addWidget(m_adaptive);
    m_mute = new QCheckBox(text("Mute"), this); row->addWidget(m_mute);
    m_encrypt = new QCheckBox(text("Encrypt attachment"), this); m_encrypt->setVisible(!target.startsWith("dm:") && card.isEmpty()); row->addWidget(m_encrypt);
    layout->addLayout(row);
    m_preview = new VideoRenderWidget(this); layout->addWidget(m_preview, 1);
    auto* playback = new QHBoxLayout; m_play = new QPushButton(text("Play / Pause"), this); m_play->hide(); playback->addWidget(m_play);
    m_seek = new QSlider(Qt::Horizontal, this); m_seek->hide(); playback->addWidget(m_seek); layout->addLayout(playback);
    auto* actions = new QHBoxLayout;
    m_start = new QPushButton(text("Record"), this); m_start->setObjectName("recordingStart"); actions->addWidget(m_start);
    m_pause = new QPushButton(text("Pause"), this); m_pause->setObjectName("recordingPause"); actions->addWidget(m_pause);
    actions->addStretch(); m_send = new QPushButton(text("Send"), this); m_send->setObjectName("recordingSend"); actions->addWidget(m_send);
    m_delete = new QPushButton(text("Delete"), this); actions->addWidget(m_delete); layout->addLayout(actions);
    m_status = new QLabel(this); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_stats = new QLabel(this); layout->addWidget(m_stats);
    m_cursor->setChecked(m_saved.value("cursor").toBool(true)); m_adaptive->setChecked(m_saved.value("adaptive").toBool());
    m_mute->setChecked(m_saved.value("muted").toBool());
    for (auto pair : {std::pair{m_codec, "codec"}, {m_bitrate, "bitrate"}, {m_qp, "qp"}, {m_rotation, "rotation"}})
        select(pair.first, m_saved.value(pair.second).toInt(pair.first == m_qp ? 28 : 0));
    if (!m_saved.value("audio").toBool(true)) select(m_microphone, QVariant());
    connect(m_source, &QMenu::triggered, this, [this] { refreshModes(); saveSettings(); synchronize(); });
    connect(m_resolution, &QMenu::triggered, this, [this] { refreshModes(); saveSettings(); synchronize(); });
    for (auto* menu : {m_codec, m_bitrate, m_qp, m_rotation, m_fps, m_microphone})
        connect(menu, &QMenu::triggered, this, [this] { saveSettings(); synchronize(); });
    for (auto* check : {m_cursor, m_adaptive, m_mute}) connect(check, &QCheckBox::toggled, this, [this] {
        m_engine->setMuted(m_mute->isChecked()); saveSettings(); synchronize();
    });
    connect(m_start, &QPushButton::clicked, this, [this] {
        if (m_engine->state() == VideoRecordingEngine::Idle) {
            saveSettings(); m_engine->start(m_directory.filePath("recording.mkv"), selection());
        } else m_engine->stop();
    });
    connect(m_pause, &QPushButton::clicked, this, [this] {
        if (m_engine->state() == VideoRecordingEngine::Paused) { saveSettings(); m_engine->resume(selection()); }
        else m_engine->pause();
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        m_discard = true; m_engine->stop(); m_player->close(); close();
    });
    connect(m_send, &QPushButton::clicked, this, &VideoRecordingWindow::send);
    connect(m_play, &QPushButton::clicked, m_player, &VideoPlayerEngine::togglePlayPause);
    connect(m_seek, &QSlider::sliderMoved, m_player, &VideoPlayerEngine::seekMs);
    connect(m_player, &VideoPlayerEngine::framePresented, this, [this] { m_preview->setFrame(m_player->currentFrame()); });
    connect(m_player, &VideoPlayerEngine::opened, this, [this](int, int, qint64 duration, bool) { m_seek->setRange(0, int(duration)); });
    connect(m_player, &VideoPlayerEngine::positionChanged, this, [this](qint64 position) {
        if (!m_seek->isSliderDown()) m_seek->setValue(position);
    });
    connect(m_player, &VideoPlayerEngine::error, this, [this](const QString& error) {
        m_status->setText(error); m_invalid = true; synchronize();
    });
    connect(m_engine, &VideoRecordingEngine::previewChanged, this, [this](const QImage& image) {
        int rotation = selection().video.rotation;
        m_preview->setFrame(rotation ? image.transformed(QTransform().rotate(rotation)) : image);
    });
    connect(m_engine, &VideoRecordingEngine::sourcesChanged, this, &VideoRecordingWindow::refreshSources);
    connect(m_engine, &VideoRecordingEngine::stateChanged, this, &VideoRecordingWindow::synchronize);
    connect(m_engine, &VideoRecordingEngine::errorOccurred, this, [this](const QString& error) { m_status->setText(error); });
    connect(m_engine, &VideoRecordingEngine::softwareEncodingRequested, this,
        [this](quint64 generation, const QString& codec, const QString& details) {
        auto* question = new QMessageBox(QMessageBox::Warning, text("Hardware encoding is unavailable"),
            text("Use CPU encoding for %1? This may cause high CPU usage.").arg(codec), QMessageBox::Yes | QMessageBox::No, this);
        question->setAttribute(Qt::WA_DeleteOnClose); question->setDefaultButton(QMessageBox::No); question->setDetailedText(details);
        connect(question, &QMessageBox::finished, m_engine, [this, generation](int result) {
            m_engine->answerSoftwareEncoding(generation, result == QMessageBox::Yes);
        });
        connect(m_engine, &VideoRecordingEngine::stateChanged, question, [this, question] {
            if (m_engine->state() != VideoRecordingEngine::Starting) question->reject();
        });
        question->open();
    });
    connect(m_engine, &VideoRecordingEngine::finished, this, [this](const QString& path) {
        if (m_discard) { close(); return; }
        m_path = path;
        if (!path.isEmpty()) { m_player->open(path); m_play->show(); m_seek->show(); show(); raise(); }
        synchronize();
    });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this); connect(escape, &QShortcut::activated, this, &QWidget::close);
    refreshSources(); synchronize();
}
VideoRecordingWindow::~VideoRecordingWindow() {
    m_invalid = true; blockSignals(true); // Queued snapshot completion при drain не должен отправлять из уничтожаемого окна.
    m_jobs.waitForDone(); delete m_player; delete m_engine;
    if (m_directory.isValid() && !m_directory.remove())
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "recording draft cleanup failed path=%s", qPrintable(m_directory.path()));
}
StreamPublishConfig VideoRecordingWindow::selection() const {
    StreamPublishConfig c;
    int index = value(m_source).toInt(); if (index >= 0 && index < m_sources.size()) c.source = m_sources[index];
    auto size = value(m_resolution).toSize(); if (size.isValid()) { c.video.width = size.width(); c.video.height = size.height(); }
    c.video.fps = value(m_fps).toInt(); c.video.codec = value(m_codec).toInt(); c.video.rotation = value(m_rotation).toInt();
    c.video.bitrate = value(m_bitrate).toInt(); c.automaticBitrate = !c.video.bitrate;
    if (c.automaticBitrate) c.video.bitrate = std::clamp(c.video.width * c.video.height * c.video.fps / 11, 250000, 16000000);
    c.video.maxQp = c.video.codec <= 2 && value(m_qp).toInt() ? value(m_qp).toInt() : -1;
    c.adaptiveQp = c.video.maxQp >= 0 && m_adaptive->isChecked(); c.audio = value(m_microphone).isValid();
    c.startMuted = m_mute->isChecked(); c.showCursor = m_cursor->isChecked(); return c;
}
void VideoRecordingWindow::refreshSources() {
    auto before = selection().source;
    m_sources = m_engine->sources(); clearOptions(m_source, "Video source");
    int chosen = -1;
    for (int i = 0; i < m_sources.size(); ++i) {
        auto& s = m_sources[i]; addOption(m_source, s.name, i);
        if (s.sameSource(before)) chosen = i;
        bool saved = s.kind == StreamVideoSource::Window ? s.name == m_saved.value("source_name").toString()
            : s.id == QByteArray::fromBase64(m_saved.value("source").toString().toLatin1());
        if (!m_restored && saved && int(s.kind) == m_saved.value("kind").toInt()) chosen = i;
    }
    if (chosen < 0 && !before.id.isEmpty() && m_engine->state() != VideoRecordingEngine::Idle) {
        chosen = m_sources.size(); m_sources.append(before);
        auto* missing = addOption(m_source, text("Selected source unavailable"), chosen); missing->setEnabled(false);
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording selected source unavailable source=%s", qPrintable(before.name));
    }
    if (chosen >= 0) select(m_source, chosen);
    m_source->addSeparator();
    connect(m_source->addAction(text("Refresh")), &QAction::triggered, m_engine, &VideoRecordingEngine::refreshSources);
    refreshModes(); if (!m_sources.isEmpty()) m_restored = true; synchronize();
}
void VideoRecordingWindow::refreshModes() {
    auto c = selection(); auto state = m_engine->state();
    QSize previous = value(m_resolution).toSize(); int fps = value(m_fps).toInt();
    if (!m_restored) {
        previous = QSize(m_saved.value("width").toInt(1280), m_saved.value("height").toInt(720)); fps = m_saved.value("fps").toInt(30);
    }
    if (state == VideoRecordingEngine::Idle) {
        clearOptions(m_resolution, "Video resolution");
        QList<QSize> sizes;
        if (c.source.kind == StreamVideoSource::Camera) {
            for (const auto& mode : c.source.modes) if (!sizes.contains(mode.size)) sizes.append(mode.size);
        } else {
            for (const auto& mode : stream_video_screen_modes(c.source.size))
                if (!mode.size.isEmpty() && !sizes.contains(mode.size)) sizes.append(mode.size);
        }
        if (sizes.isEmpty()) sizes = {QSize(1920,1080), QSize(1280,720), QSize(640,360)};
        for (auto size : sizes) addOption(m_resolution, QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()), size);
        if (!select(m_resolution, previous)) select(m_resolution, QSize(1280,720));
    }
    clearOptions(m_fps, "Frame rate");
    for (int rate : {15,24,25,30,50,60}) addOption(m_fps, QStringLiteral("%1 fps").arg(rate), rate);
    if (!select(m_fps, fps)) select(m_fps, 30);
}
void VideoRecordingWindow::synchronize() {
    auto state = m_engine->state(); bool idle = state == VideoRecordingEngine::Idle, paused = state == VideoRecordingEngine::Paused;
    bool ready = state == VideoRecordingEngine::Ready, editable = idle || paused;
    for (auto* menu : {m_source,m_fps,m_bitrate,m_rotation,m_qp,m_microphone}) menu->menuAction()->setEnabled(editable);
    m_resolution->menuAction()->setEnabled(idle); m_codec->menuAction()->setEnabled(idle);
    auto c = selection(); m_qp->menuAction()->setEnabled(editable && c.video.codec <= 2);
    m_adaptive->setEnabled(editable && c.video.maxQp >= 0); m_cursor->setEnabled(editable && StreamPublishEngine::cursorSupported(c.source));
    m_mute->setEnabled(editable || state == VideoRecordingEngine::Recording);
    m_start->setText(idle ? text("Record") : text("Stop"));
    m_start->setEnabled(!m_invalid && ((idle && selected(m_source) && selected(m_codec) && m_directory.isValid()) ||
        state == VideoRecordingEngine::Recording || paused || state == VideoRecordingEngine::Starting));
    m_pause->setText(paused ? text("Continue") : text("Pause")); m_pause->setEnabled(state == VideoRecordingEngine::Recording || paused);
    m_send->setEnabled(ready && !m_path.isEmpty() && !m_request && !m_preparing && !m_invalid);
    m_delete->setEnabled(!m_request && !m_preparing);
    m_stats->setText(text("Duration: %1 s · Size: %2 MiB · %3").arg(m_engine->durationMs()/1000.0, 0, 'f', 1)
        .arg(m_engine->bytes()/1048576.0, 0, 'f', 2).arg(paused ? text("Paused") : ready ? text("Ready") : idle ? text("Ready to record") : text("Recording")));
    auto icon = [this](QMenu* menu, const QString& shape) {
        auto* action = selected(menu); QString label = action ? action->text() : QStringLiteral("—");
        menu->menuAction()->setIcon(controlIcon(palette(), shape, label)); menu->menuAction()->setToolTip(label);
    };
    icon(m_source,c.source.kind == StreamVideoSource::Camera ? "camera" : "screen");
    icon(m_codec,"chip"); icon(m_bitrate,"gauge"); icon(m_resolution,"screen"); icon(m_fps,"fps");
    m_codec->menuAction()->setIcon(controlIcon(palette(), "chip", LiveVideoEncoder::codecName(c.video.codec).remove('.')));
    m_resolution->menuAction()->setIcon(controlIcon(palette(), "screen", QString::number(std::min(c.video.width,c.video.height)) + 'p'));
    m_fps->menuAction()->setIcon(controlIcon(palette(), "fps", QString::number(c.video.fps), "FPS"));
    m_bitrate->menuAction()->setIcon(controlIcon(palette(), "gauge", QString::number(c.video.bitrate/1000000.0,'g',2) + 'M',
        c.automaticBitrate ? "A" : QString())); icon(m_rotation,"rotation"); icon(m_qp,"qp"); icon(m_microphone,"microphone");
    if (idle && selected(m_source)) m_engine->preview(c, isVisible());
}
void VideoRecordingWindow::saveSettings() {
    auto c = selection(); if (c.source.id.isEmpty()) return;
    m_saved = {{"source",c.source.kind == StreamVideoSource::Window ? QString() : QString::fromLatin1(c.source.id.toBase64())},
        {"source_name", c.source.kind == StreamVideoSource::Window ? c.source.name : QString()}, {"kind",int(c.source.kind)},
        {"width",c.video.width},{"height",c.video.height},{"fps",c.video.fps},{"codec",c.video.codec},
        {"bitrate",value(m_bitrate).toInt()},{"qp",value(m_qp).toInt()},{"rotation",c.video.rotation},
        {"audio",c.audio},{"cursor",c.showCursor},{"adaptive",c.adaptiveQp},{"muted",c.startMuted}};
    if (c.audio) {
        auto* sound = SoundManager::instance(); auto id = value(m_microphone).toString();
        if (id != sound->captureDevice()) { sound->setDevices(sound->playbackDevice(), id); emit microphoneChanged(id); }
    }
    emit settingsChanged(QString::fromUtf8(QJsonDocument(m_saved).toJson(QJsonDocument::Compact)));
}
void VideoRecordingWindow::closeEvent(QCloseEvent* event) {
    auto s = m_engine->state();
    if (!m_discard && (s == VideoRecordingEngine::Starting || s == VideoRecordingEngine::Recording || s == VideoRecordingEngine::Pausing ||
        s == VideoRecordingEngine::Paused || s == VideoRecordingEngine::Stopping)) {
        event->ignore(); m_engine->stop(); return;
    }
    if (!m_discard && (m_request || m_preparing || s == VideoRecordingEngine::Ready)) { event->ignore(); hide(); return; }
    QWidget::closeEvent(event);
}
void VideoRecordingWindow::invalidate(const QString& target, quint64 generation) {
    if (target != m_target || generation != m_generation) return;
    m_invalid = true; m_engine->stop(); m_status->setText(text("The recipient history was deleted. This recording cannot be sent.")); synchronize();
}
void VideoRecordingWindow::submissionResult(quint64 request, bool success, bool tooLarge) {
    if (!m_request || request != m_request) return;
    m_request = 0;
    if (success) { m_discard = true; close(); }
    else {
        m_status->setText(text(tooLarge
            ? "The message and quote exceed the 4000-byte limit. Shorten the reply or select a smaller fragment."
            : "Could not send video. Please try again.")); show(); synchronize();
    }
}
void VideoRecordingWindow::send() {
    if (m_invalid || m_preparing || m_request || m_path.isEmpty()) return;
    m_preparing = true; synchronize();
    const QString path = m_path;
    m_jobs.start(QRunnable::create([this, path] {
        QTemporaryFile snapshot(QDir::tempPath() + "/utun-recording-send-XXXXXX.mkv");
        QString copy, error;
        auto owned = std::shared_ptr<QString>(new QString, [](QString* path) {
            if (!path->isEmpty() && QFile::exists(*path) && !QFile::remove(*path))
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "recording snapshot cleanup failed path=%s", qPrintable(*path));
            delete path;
        });
        if (!snapshot.open()) error = snapshot.errorString();
        else {
            copy = snapshot.fileName(); snapshot.close();
            if (!snapshot.remove() || !QFile::copy(path, copy)) { error = text("Could not copy the recorded video"); copy.clear(); }
            else { snapshot.setAutoRemove(false); *owned = copy; }
        }
        QMetaObject::invokeMethod(this, [this, copy, error, owned] {
            m_preparing = false;
            if (m_invalid || !error.isEmpty()) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recording snapshot failed invalid=%d reason=%s", m_invalid, qPrintable(error));
                if (!error.isEmpty()) m_status->setText(error);
                synchronize(); return;
            }
            auto* req = static_cast<attachment_send_req*>(u_calloc(1, sizeof(attachment_send_req)));
            QByteArray source = copy.toUtf8();
            if (!req || source.size() >= int(sizeof(req->source))) {
                u_free(req); m_status->setText(text("Could not prepare video attachment"));
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recording attachment allocation/path failed"); synchronize(); return;
            }
            req->info.kind = ATTACHMENT_VIDEO; req->temporary_source = 1; req->dm_generation = m_generation;
            req->encrypted = !m_target.startsWith("dm:") && m_card.isEmpty() && m_encrypt->isChecked();
            snprintf(req->card, sizeof(req->card), "%s", qPrintable(m_card));
            snprintf(req->source, sizeof(req->source), "%s", source.constData());
            snprintf(req->info.name, sizeof(req->info.name), "recording.mkv");
            snprintf(req->content_type, sizeof(req->content_type), "video/x-matroska");
            auto config = selection(); req->info.width = config.video.width; req->info.height = config.video.height;
            req->info.duration_ms = m_engine->durationMs();
            if (m_reply.size() == 32) memcpy(req->reply_to, m_reply.constData(), 32);
            if (m_quote.size() > DB_SYNC_MESSAGE_MAX || m_quote.contains('\0')) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "video quote invalid bytes=%d", int(m_quote.size()));
                attachment_send_discard(req); m_status->setText(text("Invalid quote")); synchronize(); return;
            }
            memcpy(req->reply_quote, m_quote.constData(), size_t(m_quote.size()));
            req->request_id = m_request = InputBar::nextSubmissionId();
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "recording attachment submit target=%s card=%s request=%llu generation=%llu bytes=%lld transcode=0",
                qPrintable(m_target), qPrintable(m_card), (unsigned long long)m_request, (unsigned long long)m_generation, (long long)m_engine->bytes());
            owned->clear(); // Единственный обработчик submitAttachment теперь владеет temporary_source.
            synchronize(); emit submitAttachment(m_target, req);
        });
    }));
}
