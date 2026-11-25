#include <QCoreApplication>
#include "callwindow.h"
#include "capture_controls.h"
#include "outline_icon.h"
#include "../transport/gui_bridge.h"
#include "call_audio_engine.h"
#include "call_audio_graphs.h"
#include "call_video_engine.h"
#include "../../../src/call/call_video_port.h"
#include "../../../src/call/call_video_playout.h"
#include "sound_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QFormLayout>
#include <QWidgetAction>
#include <QTimer>
#include <QDateTime>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <cstring>

extern "C" {
#include "../../lib/debug_config.h"
}

/* ── рисование иконок звонка (QPainter, без внешних ассетов) ── */

enum class Glyph { Phone, PhoneDown, Mic, MicOff, Camera, CameraOff, Settings, Diagnostics };

static QString callText(const char* text) { return QCoreApplication::translate("ChatGui", text); }
using namespace CaptureControls;

/* Рисует исходный shared-кадр в свободной области; layout задаёт масштаб, кадр не ограничивает минимум окна. */
class CallLocalPreview : public QWidget {
public:
    explicit CallLocalPreview(QWidget* parent) : QWidget(parent) {
        setObjectName("callLocalPreview"); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setImage(const QImage& image) { m_image = image; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.fillRect(rect(), palette().window());
        if (m_image.isNull()) return;
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QSize fitted = m_image.size().scaled(size(), Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted), m_image);
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call local preview resized area=%dx%d image=%dx%d", width(), height(), m_image.width(), m_image.height());
    }
private:
    QImage m_image; // Shared-память исходного CPU-кадра; paint не создаёт уменьшенные промежуточные pixmap.
};

// Отдельное окно показа. Закрытие скрывает только изображение, не завершает звонок.
class CallVideoWindow : public QWidget {
public:
    explicit CallVideoWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
        setObjectName("callVideoWindow"); setMinimumSize(320, 240); setAttribute(Qt::WA_QuitOnClose, false);
        QSize available = parent->screen() ? parent->screen()->availableGeometry().size() : QSize(1280, 900);
        resize(qMin(1100, available.width() * 9 / 10), qMin(800, available.height() * 9 / 10));
    }
    QImage remote, local;
    CallVideoEngine* engine = nullptr;
    quint64 serial = 0;
    quint64 callId = 0;
    int64_t requestedAt = 0; // GUI-получение кадра -> начало paintEvent, отдельно от самой работы painter.
protected:
    void moveEvent(QMoveEvent* event) override { ++m_moves; QWidget::moveEvent(event); }
    void resizeEvent(QResizeEvent* event) override { ++m_resizes; QWidget::resizeEvent(event); }
    void hideEvent(QHideEvent* event) override { presented(); QWidget::hideEvent(event); }
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (isMinimized()) presented();
    }
    void closeEvent(QCloseEvent* event) override {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call video window closed; call continues"); event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_F11 || (event->key() == Qt::Key_Escape && isFullScreen())) {
            toggleFullScreen(); event->accept(); return;
        }
        QWidget::keyPressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) { toggleFullScreen(); event->accept(); return; }
        QWidget::mouseDoubleClickEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        const int64_t started = call_video_playout_now_us(), requestWait = serial && requestedAt ? started - requestedAt : 0;
        const bool selected = serial != 0;
        QPainter painter(this); painter.fillRect(rect(), Qt::black); painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (remote.isNull()) return;
        QSize fitted = remote.size().scaled(size(), Qt::KeepAspectRatio);
        QRect videoRect(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
        const int64_t prepared = call_video_playout_now_us();
        painter.drawImage(videoRect, remote);
        const int64_t drawn = call_video_playout_now_us();
        if (!local.isNull()) {
            QSize preview = local.size().scaled(qMin(200, fitted.width() / 4), qMin(150, fitted.height() / 4), Qt::KeepAspectRatio);
            QRect target(QPoint(videoRect.right() - preview.width() - 12, videoRect.top() + 12), preview);
            painter.setPen(QPen(Qt::white, 2)); painter.drawRect(target.adjusted(-1, -1, 1, 1)); painter.drawImage(target, local);
        }
        const int64_t overlay = call_video_playout_now_us();
        painter.end();
        const int64_t finished = call_video_playout_now_us();
        presented();
        ++m_paints; if (selected) ++m_selectedPaints;
        m_requestMax = std::max(m_requestMax, requestWait); m_paintMax = std::max(m_paintMax, finished - started);
        m_prepareMax = std::max(m_prepareMax, prepared - started); m_drawMax = std::max(m_drawMax, drawn - prepared);
        m_overlayMax = std::max(m_overlayMax, overlay - drawn); m_finishMax = std::max(m_finishMax, finished - overlay);
        if (!m_slowReported && (requestWait > 50000 || finished - started > 20000)) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video GUI paint delayed call=%llu request_wait_us=%lld paint_us=%lld "
                "prepare_us=%lld draw_us=%lld overlay_us=%lld finish_us=%lld window=%dx%d image=%dx%d dpr_x1000=%d mono_us=%lld",
                (unsigned long long)callId, (long long)requestWait, (long long)(finished - started),
                (long long)(prepared - started), (long long)(drawn - prepared), (long long)(overlay - drawn),
                (long long)(finished - overlay), width(), height(), remote.width(), remote.height(),
                int(devicePixelRatioF() * 1000), (long long)finished);
            m_slowReported = true;
        }
        if (!m_reportAt) m_reportAt = started;
        if (finished - m_reportAt >= 1000000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video GUI paint call=%llu interval_us=%lld paints=%u selected=%u moves=%u resizes=%u "
                "request_wait_max_us=%lld paint_max_us=%lld prepare_max_us=%lld draw_max_us=%lld overlay_max_us=%lld finish_max_us=%lld",
                (unsigned long long)callId, (long long)(finished - m_reportAt), m_paints, m_selectedPaints, m_moves, m_resizes,
                (long long)m_requestMax, (long long)m_paintMax, (long long)m_prepareMax, (long long)m_drawMax,
                (long long)m_overlayMax, (long long)m_finishMax);
            m_reportAt = finished; m_paints = m_selectedPaints = m_moves = m_resizes = 0;
            m_requestMax = m_paintMax = m_prepareMax = m_drawMax = m_overlayMax = m_finishMax = 0; m_slowReported = false;
        }
    }
private:
    void presented() {
        if (engine && serial) { engine->imagePresented(serial); serial = 0; }
    }
    void toggleFullScreen() {
        if (isFullScreen()) { showNormal(); if (!m_geometry.isEmpty()) restoreGeometry(m_geometry); }
        else { m_geometry = saveGeometry(); showFullScreen(); }
    }
    QByteArray m_geometry;
    int64_t m_reportAt = 0, m_requestMax = 0, m_paintMax = 0, m_prepareMax = 0, m_drawMax = 0, m_overlayMax = 0, m_finishMax = 0;
    unsigned m_paints = 0, m_selectedPaints = 0, m_moves = 0, m_resizes = 0;
    bool m_slowReported = false;
};

static QColor colorForName(const QString& name) {
    static const QColor palette[] = {
        QColor("#5B9BD5"), QColor("#70AD47"), QColor("#ED7D31"),
        QColor("#9B59B6"), QColor("#F4B400"), QColor("#E67E22"),
        QColor("#16A085"), QColor("#D35400"),
    };
    quint32 h = 0;
    for (const QChar c : name) h = h * 31u + c.unicode();
    return palette[h % 8];
}

static QPixmap makeAvatar(const QString& name, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(colorForName(name));
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, size, size);
    QString initial = name.trimmed().isEmpty() ? QStringLiteral("?") : name.trimmed().left(1).toUpper();
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(size / 2);
    p.setFont(f);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, initial);
    p.end();
    return pm;
}

static void drawPhoneGlyph(QPainter* p, const QRectF& r, bool down) {
    const qreal s = r.width();
    p->save();
    p->translate(r.center());
    p->rotate(down ? 135.0 : -45.0);
    const qreal w = s * 0.36, h = s * 0.64;
    QPainterPath path;
    path.addRoundedRect(QRectF(-w / 2, -h / 2, w, h), w * 0.5, w * 0.5);
    p->setPen(Qt::NoPen);
    p->setBrush(Qt::white);
    p->drawPath(path);
    p->restore();
}

/* Тот же контур и толщина штриха, что у микрофона в панели ввода. */
static void drawMicGlyph(QPainter* p, const QRectF& r, bool off) {
    p->save();
    p->translate(r.topLeft()); p->scale(r.width() / 24.0, r.height() / 24.0);
    p->setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    p->drawPath(microphoneOutline(off));
    p->restore();
}

static void drawCameraGlyph(QPainter* p, const QRectF& r, bool off) {
    const qreal s = r.width();
    p->save();
    p->translate(r.topLeft());
    p->setPen(QPen(Qt::white, s * 0.07, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    p->drawRoundedRect(QRectF(s * 0.06, s * 0.24, s * 0.56, s * 0.52), s * 0.07, s * 0.07);
    QPainterPath lens;
    lens.moveTo(s * 0.62, s * 0.40); lens.lineTo(s * 0.90, s * 0.24);
    lens.lineTo(s * 0.90, s * 0.76); lens.lineTo(s * 0.62, s * 0.60); lens.closeSubpath();
    p->drawPath(lens);
    if (off) p->drawLine(QPointF(s * 0.06, s * 0.06), QPointF(s * 0.94, s * 0.94));
    p->restore();
}

static void drawSettingsGlyph(QPainter* p, const QRectF& r) {
    const qreal s = r.width();
    p->save(); p->translate(r.center());
    p->setPen(Qt::NoPen); p->setBrush(Qt::white);
    for (int tooth = 0; tooth < 8; ++tooth) {
        p->save(); p->rotate(tooth * 45.0);
        p->drawRoundedRect(QRectF(-s * 0.10, -s * 0.47, s * 0.20, s * 0.22), s * 0.03, s * 0.03);
        p->restore();
    }
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addEllipse(QRectF(-s * 0.34, -s * 0.34, s * 0.68, s * 0.68));
    ring.addEllipse(QRectF(-s * 0.14, -s * 0.14, s * 0.28, s * 0.28));
    p->drawPath(ring);
    p->restore();
}

static QIcon makeRoundIcon(const QColor& bg, Glyph glyph, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawEllipse(0, 0, size, size);
    const QRectF glyphRect(size * 0.24, size * 0.24, size * 0.52, size * 0.52);
    switch (glyph) {
    case Glyph::Phone:     drawPhoneGlyph(&p, glyphRect, false); break;
    case Glyph::PhoneDown: drawPhoneGlyph(&p, glyphRect, true);  break;
    case Glyph::Mic:       drawMicGlyph(&p, glyphRect, false);   break;
    case Glyph::MicOff:    drawMicGlyph(&p, glyphRect, true);    break;
    case Glyph::Camera:    drawCameraGlyph(&p, glyphRect, false); break;
    case Glyph::CameraOff: drawCameraGlyph(&p, glyphRect, true);  break;
    case Glyph::Settings:  drawSettingsGlyph(&p, glyphRect);      break;
    case Glyph::Diagnostics: {
        p.setPen(QPen(Qt::white, glyphRect.width() * 0.07, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(glyphRect.bottomLeft(), glyphRect.topLeft()); p.drawLine(glyphRect.bottomLeft(), glyphRect.bottomRight());
        QPainterPath graph; graph.moveTo(glyphRect.left() + glyphRect.width() * 0.1, glyphRect.center().y());
        graph.lineTo(glyphRect.left() + glyphRect.width() * 0.35, glyphRect.center().y());
        graph.lineTo(glyphRect.center().x(), glyphRect.top() + glyphRect.height() * 0.2);
        graph.lineTo(glyphRect.left() + glyphRect.width() * 0.65, glyphRect.top() + glyphRect.height() * 0.8);
        graph.lineTo(glyphRect.right() - glyphRect.width() * 0.1, glyphRect.top() + glyphRect.height() * 0.3);
        p.setBrush(Qt::NoBrush); p.drawPath(graph); break;
    }
    }
    p.end();
    return QIcon(pm);
}

static QPushButton* makeRoundBtn(const QIcon& icon, const QString& tooltip, QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setIcon(icon);
    btn->setIconSize(QSize(52, 52));
    btn->setFixedSize(64, 64);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tooltip);
    btn->setStyleSheet(
        "QPushButton { border: none; border-radius: 32px; background: transparent; }"
        "QPushButton:pressed { background: rgba(255,255,255,0.15); }");
    return btn;
}

static QWidget* wrapWithCaption(QWidget* btn, const QString& caption, QLabel** captionOut = nullptr) {
    auto* w = new QWidget(btn->parentWidget());
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(6);
    l->addWidget(btn, 0, Qt::AlignHCenter);
    auto* cap = new QLabel(caption, w);
    cap->setAlignment(Qt::AlignCenter);
    cap->setStyleSheet("color: palette(text); font-size: 11px; background: transparent;");
    l->addWidget(cap, 0, Qt::AlignHCenter);
    if (captionOut) *captionOut = cap;
    return w;
}

CallWindow::CallWindow(QWidget* parent)
    : QWidget(parent, Qt::Window) {
    setWindowTitle(QCoreApplication::translate("ChatGui", "Call"));
    resize(520, 520);
    setAttribute(Qt::WA_DeleteOnClose, false);

    m_avatarLabel = new QLabel(this);
    m_avatarLabel->setObjectName("callAvatar");
    m_avatarLabel->setFixedSize(80, 80);
    m_avatarLabel->setAlignment(Qt::AlignCenter);

    m_peerLabel = new QLabel(this);
    m_peerLabel->setAlignment(Qt::AlignCenter);
    m_peerLabel->setStyleSheet("font-size: 17px; font-weight: bold; color: palette(text);");

    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 13px;");
    m_statusLabel->setWordWrap(true);
    connect(CallAudioEngine::instance(), &CallAudioEngine::captureErrorChanged, this, [this] {
        auto* audio = CallAudioEngine::instance();
        if (m_state == Active && audio->callId() == m_callId)
            m_statusLabel->setText(audio->captureError().isEmpty() ? callText("In call") : audio->captureError());
    });

    m_durationLabel = new QLabel(QStringLiteral("00:00"), this);
    m_durationLabel->setAlignment(Qt::AlignCenter);
    m_durationLabel->setStyleSheet("font-size: 15px; color: palette(text); font-family: monospace;");

    m_bgpPathLabel = new QLabel(this);
    m_bgpPathLabel->setAlignment(Qt::AlignCenter);
    m_bgpPathLabel->setWordWrap(true);
    m_bgpPathLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    m_bgpPathLabel->hide();
    m_connectionLabel = new QLabel(this);
    m_connectionLabel->setAlignment(Qt::AlignCenter);
    m_connectionLabel->setStyleSheet("color: palette(placeholder-text); font-size: 13px;");
    setConnectionType(0, {});
    m_statsLabel = new QLabel(this);
    m_statsLabel->setObjectName("callAudioStats");
    m_statsLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_statsLabel->setWordWrap(true);
    m_statsLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    m_statsLabel->setTextFormat(Qt::RichText);
    m_statsLabel->hide();

    m_acceptBtn = makeRoundBtn(makeRoundIcon(QColor("#27ae60"), Glyph::Phone, 96), QCoreApplication::translate("ChatGui", "Accept"), this);
    m_declineBtn = makeRoundBtn(makeRoundIcon(QColor("#e74c3c"), Glyph::PhoneDown, 96), QCoreApplication::translate("ChatGui", "Decline"), this);
    m_hangupBtn = makeRoundBtn(makeRoundIcon(QColor("#e74c3c"), Glyph::PhoneDown, 96), QCoreApplication::translate("ChatGui", "End call"), this);
    m_muteBtn = makeRoundBtn(makeRoundIcon(QColor("#7f8c8d"), Glyph::Mic, 96), QCoreApplication::translate("ChatGui", "Mute"), this);
    m_muteBtn->setObjectName(QStringLiteral("callMuteButton"));
    m_muteBtn->setCheckable(true);
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call microphone button initialized icon=shared-outline muted_icon=diagonal-strike");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    m_videoWindow = new CallVideoWindow(this);
    m_panel = new QWidget(this); m_panel->setObjectName("callPanel"); m_panel->setMaximumWidth(640);
    m_panel->setStyleSheet("QWidget#callPanel { background: palette(window); border-radius: 12px; }");
    // Содержимое сверху, отдельная строка действий всегда у нижнего края окна.
    root->addWidget(m_panel, 0, Qt::AlignHCenter);
    QVBoxLayout* lay = new QVBoxLayout(m_panel);
    lay->setContentsMargins(16, 20, 16, 16);
    lay->setSpacing(8);
    lay->addWidget(m_avatarLabel, 0, Qt::AlignHCenter);
    lay->addWidget(m_peerLabel);
    lay->addWidget(m_statusLabel);
    m_mediaWarning = new QLabel(this); m_mediaWarning->setWordWrap(true);
    m_mediaWarning->setAlignment(Qt::AlignCenter); m_mediaWarning->hide(); lay->addWidget(m_mediaWarning);
    m_continueBtn = new QPushButton(callText("Call anyway"), this);
    lay->addWidget(m_continueBtn);
    connect(m_continueBtn, &QPushButton::clicked, this, [this] {
        if (m_state != WaitDecision) return;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "desktop caller overrides phone busy id=%016llx", (unsigned long long)m_callId);
        if (gui_bridge_call_action(GUI_CALL_CONTINUE, m_callId) < 0) {
            m_statusLabel->setText(callText("Could not send the call command")); return;
        }
        setState(Outgoing);
        SoundManager::instance()->playLoop("call_connecting");
    });
    lay->addWidget(m_durationLabel);
    m_diagnostics = new QWidget(m_panel); m_diagnostics->setObjectName("callDiagnostics");
    auto* diagnosticLayout = new QVBoxLayout(m_diagnostics); diagnosticLayout->setContentsMargins(12, 12, 12, 12);
    auto* diagnosticTitle = new QLabel(callText("Diagnostics"), m_diagnostics);
    diagnosticLayout->addWidget(diagnosticTitle);
    diagnosticLayout->addWidget(m_connectionLabel); diagnosticLayout->addWidget(m_bgpPathLabel); diagnosticLayout->addWidget(m_statsLabel);
    diagnosticLayout->addStretch(); m_diagnostics->hide();
    for (auto* label : {m_connectionLabel, m_bgpPathLabel, m_statsLabel}) {
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop); label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    m_video = new CallVideoEngine(this);
    m_video->setPresentationDriven(true); m_videoWindow->engine = m_video;
    auto* previewArea = new QWidget(this); previewArea->setObjectName("callPreviewArea");
    auto* previewLayout = new QVBoxLayout(previewArea); previewLayout->setContentsMargins(0, 0, 0, 0);
    m_localPreview = new CallLocalPreview(previewArea); m_localPreview->hide(); previewLayout->addWidget(m_localPreview);
    root->addWidget(previewArea, 1);
    m_videoStatus = new QLabel(this); m_videoStatus->setObjectName("callVideoStatus"); m_videoStatus->setWordWrap(true); lay->addWidget(m_videoStatus);
    m_videoControls = new QWidget(this); auto* videoLayout = new QVBoxLayout(m_videoControls);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    m_videoMenuBar = new QMenuBar(m_videoControls); m_videoMenuBar->setNativeMenuBar(false); m_videoMenuBar->setMouseTracking(true);
    m_videoMenuBar->setObjectName("callVideoControls");
    auto* menuStyle = new MenuStyle; menuStyle->setParent(this); m_videoMenuBar->setStyle(menuStyle);
    videoLayout->addWidget(m_videoMenuBar, 0, Qt::AlignHCenter); lay->insertWidget(0, m_videoControls);
    auto makeMenu = [this](const char* heading, const char* name) {
        auto* menu = m_videoMenuBar->addMenu(QString()); menu->setObjectName(QString::fromLatin1(name));
        new QActionGroup(menu); menu->addSection(callText(heading)); return menu;
    };
    m_camera = makeMenu("Camera", "callVideoCamera");
    m_codec = makeMenu("Video codec", "callVideoCodec");
    m_bitrate = makeMenu("Video bitrate", "callVideoBitrate");
    m_resolution = makeMenu("Video resolution", "callVideoResolution");
    m_fps = makeMenu("Frame rate", "callVideoFps");
    m_output = makeMenu("Audio output", "callAudioOutput");
    addOption(m_output, callText("Default (auto)"), QString());
    auto* actions = new QHBoxLayout();
    m_videoEnabled = makeRoundBtn(makeRoundIcon(QColor("#7f8c8d"), Glyph::CameraOff, 96), callText("Turn video on"), this);
    m_videoEnabled->setObjectName("callVideoEnabled");
    m_videoEnabled->setCheckable(true); m_videoEnabled->setAccessibleName(callText("Turn video on"));
    actions->addStretch(1);
    m_settingsBtn = makeRoundBtn(makeRoundIcon(QColor("#7f8c8d"), Glyph::Settings, 96), callText("Video settings"), this);
    m_settingsBtn->setObjectName("callVideoSettings");
    m_settingsBtn->setCheckable(true); m_settingsBtn->setAccessibleName(callText("Video settings"));
    m_diagnosticsBtn = makeRoundBtn(makeRoundIcon(QColor("#7f8c8d"), Glyph::Diagnostics, 96), callText("Diagnostics"), this);
    m_diagnosticsBtn->setObjectName("callDiagnosticsButton");
    m_diagnosticsBtn->setAccessibleName(callText("Diagnostics"));
    m_showVideoBtn = new QPushButton(callText("Show video"), this); actions->addWidget(m_showVideoBtn);
    connect(m_diagnosticsBtn, &QPushButton::clicked, this, [this] {
        if (!m_audioGraphs) m_audioGraphs = new CallAudioGraphs(m_callId, m_diagnostics, this);
        m_audioGraphs->showNormal(); m_audioGraphs->raise(); m_audioGraphs->activateWindow();
    });
    lay->addLayout(actions);
    connect(m_settingsBtn, &QPushButton::toggled, this, [this](bool checked) {
        m_settingsBtn->setIcon(makeRoundIcon(checked ? QColor("#2980b9") : QColor("#7f8c8d"), Glyph::Settings, 96));
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "desktop video settings visible=%d", checked);
        updateVideoPresentation();
    });
    connect(m_showVideoBtn, &QPushButton::clicked, this, [this] {
        if (m_videoWindow->isMinimized()) m_videoWindow->showNormal(); else m_videoWindow->show();
        m_videoWindow->raise(); m_videoWindow->activateWindow();
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call video window reopened");
    });
    connect(m_video, &CallVideoEngine::camerasChanged, this, [this](const VideoCaptureDevices& cameras) {
        const auto previous = value(m_camera); m_cameras = cameras;
        clearOptions(m_camera, "Camera");
        for (const auto& camera : cameras) {
            auto* action = addOption(m_camera, camera.name, camera.id); action->setEnabled(!camera.modes.isEmpty());
        }
        auto* previousAction = option(m_camera, previous);
        if (previousAction && previousAction->isEnabled()) previousAction->setChecked(true);
        else for (auto* action : m_camera->actions()) {
            if (action->isCheckable() && action->isEnabled()) { action->setChecked(true); break; }
        }
        refreshVideoModes();
        if (m_videoEnabled->isChecked()) applyVideo();
    });
    addOption(m_codec, callText("Auto (H.265 → H.264)"), 0); addOption(m_codec, "H.265", 2); addOption(m_codec, "H.264", 1);
    auto* ranges = new QWidget(m_bitrate); auto* rates = new QFormLayout(ranges);
    auto* rangeAction = new QWidgetAction(m_bitrate); rangeAction->setDefaultWidget(ranges); m_bitrate->addAction(rangeAction);
    m_videoMin = new QSpinBox(ranges); m_videoMax = new QSpinBox(ranges);
    m_videoMin->setObjectName("callVideoMinimumBitrate"); m_videoMax->setObjectName("callVideoMaximumBitrate");
    for (auto* spin : {m_videoMin, m_videoMax}) {
        spin->setRange(300, 4000); spin->setSingleStep(100); spin->setSuffix(callText(" kbit/s"));
    }
    rates->addRow(callText("Call video: minimum bitrate"), m_videoMin);
    rates->addRow(callText("Call video: maximum bitrate"), m_videoMax);
    m_videoMin->setValue(300); m_videoMax->setValue(4000);
    ranges->setToolTip(callText("Video bitrate adapts to the connection within this range. Default: 300–4000 kbit/s."));
    connect(m_videoEnabled, &QPushButton::toggled, this, [this](bool checked) {
        m_videoEnabled->setIcon(makeRoundIcon(checked ? QColor("#2980b9") : QColor("#7f8c8d"), checked ? Glyph::Camera : Glyph::CameraOff, 96));
        const QString action = callText(checked ? "Turn video off" : "Turn video on");
        m_videoEnabled->setToolTip(action); m_videoEnabled->setAccessibleName(action);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "desktop video button id=%016llx enabled=%d", (unsigned long long)m_callId, checked);
        applyVideo();
    });
    connect(m_camera, &QMenu::triggered, this, [this] { refreshVideoModes(); applyVideo(); });
    connect(m_resolution, &QMenu::triggered, this, [this] {
        populateFrameRates(m_fps, m_modes, value(m_resolution).toSize(), value(m_fps).toInt()); applyVideo();
    });
    for (auto* menu : {m_fps, m_codec}) connect(menu, &QMenu::triggered, this, &CallWindow::applyVideo);
    auto updateRange = [this] {
        if (m_videoMin->value() > m_videoMax->value()) m_videoMax->setValue(m_videoMin->value());
        if (m_videoCall && call_video_port_range(m_videoCall, m_videoMin->value() * 1000, m_videoMax->value() * 1000) < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call bitrate range refused id=%llu min_kbps=%d max_kbps=%d",
                (unsigned long long)m_videoCall, m_videoMin->value(), m_videoMax->value());
            m_videoStatus->setText(callText("Could not save the call video bitrate range.")); m_videoStatus->show();
        }
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call bitrate range selected id=%llu min_kbps=%d max_kbps=%d",
            (unsigned long long)m_videoCall, m_videoMin->value(), m_videoMax->value());
        updateVideoMenuButtons();
    };
    connect(m_videoMin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int minimum) {
        if (minimum > m_videoMax->value()) m_videoMax->setValue(minimum); updateVideoMenuButtons();
    });
    connect(m_videoMax, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int maximum) {
        if (maximum < m_videoMin->value()) m_videoMin->setValue(maximum); updateVideoMenuButtons();
    });
    connect(m_videoMin, &QSpinBox::editingFinished, this, updateRange);
    connect(m_videoMax, &QSpinBox::editingFinished, this, updateRange);
    connect(m_video, &CallVideoEngine::statusChanged, this, [this](const QString& text) {
        m_videoStatus->setText(text); m_videoStatus->setVisible(m_state == Active && !text.isEmpty());
    });
    connect(m_video, &CallVideoEngine::imageChanged, this, [this](const QImage& image, quint64 serial) {
        bool changed = m_remoteImage.isNull() != image.isNull(); m_remoteImage = image;
        m_videoWindow->remote = image; m_videoWindow->serial = serial; m_videoWindow->callId = m_videoCall;
        m_videoWindow->requestedAt = call_video_playout_now_us(); m_videoWindow->update();
        if (changed) updateVideoPresentation();
        // Скрытое окно продолжает принимать поток, не ждёт невозможного paintEvent.
        if (!m_videoWindow->isVisible() || m_videoWindow->isMinimized()) m_video->imagePresented(serial);
        update();
    });
    connect(m_video, &CallVideoEngine::localImageChanged, this, [this](const QImage& image) {
        bool changed = m_localImage.isNull() != image.isNull(); m_localImage = image;
        m_videoWindow->local = image; m_videoWindow->update();
        m_localPreview->setImage(image);
        if (changed) updateVideoPresentation();
        update();
    });
    auto* videoTimer = new QTimer(this); videoTimer->setInterval(100);
    connect(videoTimer, &QTimer::timeout, this, [this] {
        if (m_state != Active || !CallAudioEngine::instance()->isActive() || CallAudioEngine::instance()->callId() != m_callId) return;
        if (m_videoCall != m_callId) { m_videoCall = m_callId; m_videoRangeReady = false; m_video->start(m_callId); applyVideo(); }
        struct call_video_port_status status{};
        if (!m_videoRangeReady && call_video_port_status(m_callId, &status) == 0 && status.minimum_bps) {
            m_videoMin->setValue(status.minimum_bps / 1000); m_videoMax->setValue(status.maximum_bps / 1000); m_videoRangeReady = true;
        }
    }); videoTimer->start();

    m_acceptCell = wrapWithCaption(m_acceptBtn, QCoreApplication::translate("ChatGui", "Accept"));
    m_declineCell = wrapWithCaption(m_declineBtn, QCoreApplication::translate("ChatGui", "Decline"));
    m_hangupCell = wrapWithCaption(m_hangupBtn, QCoreApplication::translate("ChatGui", "End call"));
    m_muteCell = wrapWithCaption(m_muteBtn, QCoreApplication::translate("ChatGui", "Mute"), &m_muteCaption);
    m_videoCell = wrapWithCaption(m_videoEnabled, callText("Camera"));
    m_settingsCell = wrapWithCaption(m_settingsBtn, callText("Video settings"));
    m_diagnosticsCell = wrapWithCaption(m_diagnosticsBtn, callText("Diagnostics"));

    auto* footer = new QWidget(this); footer->setObjectName("callActions");
    QHBoxLayout* btn = new QHBoxLayout(footer); btn->setContentsMargins(0, 0, 0, 0);
    btn->setSpacing(10);
    btn->addStretch(1);
    btn->addWidget(m_acceptCell);
    btn->addWidget(m_declineCell);
    btn->addWidget(m_hangupCell);
    btn->addWidget(m_muteCell);
    btn->addWidget(m_videoCell);
    btn->addWidget(m_settingsCell);
    btn->addWidget(m_diagnosticsCell);
    btn->addStretch(1);
    root->addWidget(footer, 0, Qt::AlignHCenter);

    connect(m_acceptBtn, &QPushButton::clicked, this, &CallWindow::onAccept);
    connect(m_declineBtn, &QPushButton::clicked, this, &CallWindow::onDecline);
    connect(m_hangupBtn, &QPushButton::clicked, this, &CallWindow::onHangup);
    connect(m_muteBtn, &QPushButton::toggled, this, &CallWindow::onMuteToggled);
    connect(m_output, &QMenu::triggered, this, &CallWindow::onDeviceChanged);

    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(1000);
    connect(m_durationTimer, &QTimer::timeout, this, &CallWindow::updateDuration);

    updateVideoMenuButtons(); setState(Outgoing);
}

CallWindow::~CallWindow() {
    // Остановить callbacks до удаления дочерних виджетов окна.
    m_video->disconnect(this); m_video->stop();
}

void CallWindow::setPeerName(const QString& name) {
    m_peerLabel->setText(name);
    m_avatarLabel->setPixmap(makeAvatar(name, 80));
    m_videoWindow->setWindowTitle(callText("Video — %1").arg(name));
}

void CallWindow::setBgpPath(const QString& path) {
    m_bgpPathLabel->setText(path);
    m_bgpPathLabel->setVisible(m_state == Active && !path.isEmpty());
}

void CallWindow::setConnectionType(uint8_t type, const QByteArray& nextHop) {
    const QString names[] = { QCoreApplication::translate("ChatGui", "not connected"), QCoreApplication::translate("ChatGui", "direct"),
                              QCoreApplication::translate("ChatGui", "reverse"), QCoreApplication::translate("ChatGui", "indirect") };
    m_connectionLabel->setText(QCoreApplication::translate("ChatGui", "Connection: %1").arg(names[type <= 3 ? type : 0]));
    m_connectionLabel->setToolTip(type == 3 ? QCoreApplication::translate("ChatGui", "Relay: 0x%1").arg(QString::fromLatin1(nextHop.toHex())) : QString());
}

void CallWindow::setState(State s) {
    m_state = s;
    if (s == Preparing || s == Active || s == Ended) setMediaWarning(QString());
    switch (s) {
    case Answering:
        m_statusLabel->setText(callText("Waiting for call confirmation…")); m_durationLabel->hide(); break;
    case Preparing:
        m_statusLabel->setText(callText("Preparing conversation…")); m_durationLabel->hide(); break;
    case WaitDecision:
        m_statusLabel->setText(callText("Subscriber is on a phone call"));
        m_durationLabel->hide();
        break;
    case Outgoing:
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Connecting…"));
        m_durationLabel->hide();
        break;
    case Ringing:
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Calling…"));
        m_durationLabel->hide();
        break;
    case Incoming:
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Incoming call…"));
        m_durationLabel->hide();
        break;
    case Active:
        m_statusLabel->setText(CallAudioEngine::instance()->captureError().isEmpty()
            ? callText("In call") : CallAudioEngine::instance()->captureError());
        m_durationLabel->setText(QStringLiteral("00:00"));
        m_durationLabel->show();
        m_startMs = QDateTime::currentMSecsSinceEpoch();
        m_durationTimer->start();
        populateOutputDevices();
        m_muteBtn->setChecked(false);
        break;
    case Ended:
        m_video->stop(); m_videoCall = 0;
        setConnectionType(0, {});
        m_statusLabel->setText(QCoreApplication::translate("ChatGui", "Ended"));
        m_durationTimer->stop();
        break;
    }
    updateButtons();
}

void CallWindow::setMediaWarning(const QString& text) {
    m_mediaWarning->setText(text); m_mediaWarning->setVisible(!text.isEmpty());
}

void CallWindow::populateOutputDevices() {
    clearOptions(m_output, "Audio output");
    addOption(m_output, callText("Default (auto)"), QString());
    const auto devs = SoundManager::enumeratePlaybackDevices();
    for (int i = 0; i < devs.size(); i++) {
        QString label = devs[i].name;
        if (devs[i].isDefault) label += QCoreApplication::translate("ChatGui", " (default)");
        addOption(m_output, label, devs[i].id);
    }
    QString cur = CallAudioEngine::instance()->playbackDevice();
    if (!select(m_output, cur))
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call audio output unavailable device=%s; using default selection", qPrintable(cur));
    updateVideoMenuButtons();
}

void CallWindow::updateButtons() {
    m_continueBtn->setVisible(m_state == WaitDecision);
    m_acceptCell->setVisible(m_state == Incoming);
    m_declineCell->setVisible(m_state == Incoming);
    m_hangupCell->setVisible(m_state != Incoming && m_state != Ended);
    m_muteCell->setVisible(m_state == Active);
    updateVideoPresentation();
}

void CallWindow::updateVideoPresentation() {
    bool receiving = m_state == Active && !m_remoteImage.isNull();
    if (receiving != m_receivingVideo) {
        m_receivingVideo = receiving;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call video separate window remote=%d local=%d", receiving, !m_localImage.isNull());
        if (receiving) m_videoWindow->show(); else m_videoWindow->hide();
    }
    bool active = m_state == Active, settings = active && m_settingsBtn->isChecked();
    m_avatarLabel->setVisible(!active || (!settings && !receiving && m_localImage.isNull()));
    m_localPreview->setVisible(active && !receiving && !m_localImage.isNull());
    bool diagnosticData = active || m_audioGraphs;
    m_connectionLabel->setVisible(diagnosticData); m_bgpPathLabel->setVisible(diagnosticData && !m_bgpPathLabel->text().isEmpty());
    m_statsLabel->setVisible(diagnosticData && !m_statsLabel->text().isEmpty());
    m_diagnosticsCell->setVisible(active);
    m_videoCell->setVisible(active); m_settingsCell->setVisible(active);
    m_videoControls->setVisible(settings); m_showVideoBtn->setVisible(receiving);
    m_videoStatus->setVisible(active && !m_videoStatus->text().isEmpty());
    QSize panelMinimum = m_panel->minimumSizeHint();
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call layout active=%d preview=%d settings=%d avatar=%d diagnostics=%d window=%dx%d panel_min=%dx%d",
        active, m_localPreview->isVisible(), settings, !m_avatarLabel->isHidden(), !m_diagnostics->isHidden(),
        width(), height(), panelMinimum.width(), panelMinimum.height());
    update();
}

void CallWindow::applyVideo() {
    updateVideoMenuButtons();
    if (!m_videoCall) return;
    QSize size = value(m_resolution).toSize();
    const bool enabled = m_videoEnabled->isChecked() && !size.isEmpty() && selected(m_fps);
    if (m_videoEnabled->isChecked() && !enabled) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call camera has no selected mode device=%s", value(m_camera).toByteArray().constData());
        m_videoStatus->setText(callText("No supported video modes")); m_videoStatus->show();
    }
    if (size.isEmpty()) size = QSize(640, 480);
    const int fps = value(m_fps).toInt();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call video selection id=%llu enabled=%d device=%s mode=%dx%d/%d codec=%d",
        (unsigned long long)m_videoCall, enabled, value(m_camera).toByteArray().constData(), size.width(), size.height(), fps, value(m_codec).toInt());
    m_video->setVideo(enabled, value(m_camera).toByteArray(), size.width(), size.height(), value(m_codec).toInt(), fps ? fps : 30);
}

void CallWindow::refreshVideoModes() {
    QSize wanted = value(m_resolution).toSize(); if (wanted.isEmpty()) wanted = QSize(640, 480);
    const int previousFps = selected(m_fps) ? value(m_fps).toInt() : 30;
    clearOptions(m_resolution, "Video resolution"); m_modes.clear();
    for (const auto& camera : m_cameras) if (camera.id == value(m_camera).toByteArray()) { m_modes = camera.modes; break; }
    for (const auto& mode : m_modes) if (!option(m_resolution, mode.size))
        addOption(m_resolution, QStringLiteral("%1 × %2").arg(mode.size.width()).arg(mode.size.height()), mode.size);
    if (!select(m_resolution, wanted) && !select(m_resolution, QSize(640, 480)) && !m_modes.isEmpty())
        select(m_resolution, m_modes.back().size);
    populateFrameRates(m_fps, m_modes, value(m_resolution).toSize(), previousFps);
    if (m_modes.isEmpty()) DEBUG_WARN(DEBUG_CATEGORY_CALL, "call camera has no supported modes device=%s", value(m_camera).toByteArray().constData());
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call camera modes device=%s count=%lld requested=%dx%d selected=%dx%d/%d",
        value(m_camera).toByteArray().constData(), (long long)m_modes.size(), wanted.width(), wanted.height(),
        value(m_resolution).toSize().width(), value(m_resolution).toSize().height(), value(m_fps).toInt());
    updateVideoMenuButtons();
}

void CallWindow::updateVideoMenuButtons() {
    auto update = [this](QMenu* menu, const char* heading, const QString& shape, const QString& label, const QString& detail = {}) {
        menu->menuAction()->setIcon(controlIcon(palette(), shape, label, detail));
        QString description = callText(heading);
        if (auto* action = selected(menu)) description += QStringLiteral(": ") + action->text();
        menu->menuAction()->setToolTip(description); menu->setAccessibleName(description);
    };
    auto* camera = selected(m_camera);
    update(m_camera, "Camera", "camera", camera ? camera->text() : "—");
    int codec = value(m_codec).toInt();
    update(m_codec, "Video codec", "chip", codec ? QStringLiteral("H%1").arg(codec == 2 ? 265 : 264) : "Auto");
    auto shortRate = [](int kbps) { return kbps >= 1000 ? QString::number(kbps / 1000.0, 'g', 2) + 'M' : QString::number(kbps) + 'k'; };
    update(m_bitrate, "Video bitrate", "gauge", shortRate(m_videoMax->value()), shortRate(m_videoMin->value()));
    QString range = callText("Bitrate range, kbit/s:") + QStringLiteral(" %1–%2").arg(m_videoMin->value()).arg(m_videoMax->value());
    m_bitrate->menuAction()->setToolTip(range); m_bitrate->setAccessibleName(range);
    QSize size = value(m_resolution).toSize();
    update(m_resolution, "Video resolution", "screen", size.isEmpty() ? "—" : QString::number(std::min(size.width(), size.height())) + 'p');
    update(m_fps, "Frame rate", "fps", selected(m_fps) ? value(m_fps).toString() : "—", "FPS");
    auto* output = selected(m_output); update(m_output, "Audio output", "output", output ? output->text() : "—");
    m_resolution->menuAction()->setEnabled(!m_modes.isEmpty()); m_fps->menuAction()->setEnabled(!m_modes.isEmpty());
    m_videoMenuBar->setFixedWidth(m_videoMenuBar->sizeHint().width());
}

void CallWindow::onAccept() {
    if (gui_bridge_call_action(GUI_CALL_ACCEPT, m_callId) < 0) {
        m_statusLabel->setText(callText("Could not send the call command")); return;
    }
    SoundManager::instance()->stopLoop();
    setState(Answering); // Медиа разрешит только CALL_ACCEPTED после подтверждения выбора ядром.
}

void CallWindow::onDecline() {
    if (gui_bridge_call_action(GUI_CALL_DECLINE, m_callId) < 0)
        m_statusLabel->setText(callText("Could not send the call command"));
}

void CallWindow::onHangup() {
    if (gui_bridge_call_action(GUI_CALL_HANGUP, m_callId) < 0)
        m_statusLabel->setText(callText("Could not send the call command"));
}

void CallWindow::onMuteToggled(bool checked) {
    CallAudioEngine::instance()->setMuted(checked);
    m_muteBtn->setIcon(makeRoundIcon(checked ? QColor("#e74c3c") : QColor("#7f8c8d"),
                                     checked ? Glyph::MicOff : Glyph::Mic, 96));
    if (m_muteCaption)
        m_muteCaption->setText(checked ? QCoreApplication::translate("ChatGui", "Unmute") : QCoreApplication::translate("ChatGui", "Mute"));
}

void CallWindow::onDeviceChanged() {
    QString id = value(m_output).toString();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call audio output selected device=%s", qPrintable(id));
    CallAudioEngine::instance()->setPlaybackDevice(id);
    updateVideoMenuButtons();
}

/* Закрытие непринятого вызова равнозначно Decline; локальный Ended исключает повторную команду. */
void CallWindow::closeEvent(QCloseEvent* event) {
    const State previous = m_state;
    if (previous != Ended) {
        setState(Ended);
        const auto action = previous == Incoming ? GUI_CALL_DECLINE : GUI_CALL_HANGUP;
        const char* name = action == GUI_CALL_DECLINE ? "decline" : "hangup";
        if (gui_bridge_call_action(action, m_callId) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call window closed without queued %s id=%llu previous_state=%d",
                name, (unsigned long long)m_callId, int(previous));
        else DEBUG_INFO(DEBUG_CATEGORY_CALL, "call window closed: queued %s id=%llu previous_state=%d",
                name, (unsigned long long)m_callId, int(previous));
    }
    emit closed(m_callId);
    event->accept();
}

void CallWindow::updateDuration() {
    if (m_state != Active) return;
    qint64 ms = QDateTime::currentMSecsSinceEpoch() - m_startMs;
    qint64 s = ms / 1000;
    m_durationLabel->setText(QStringLiteral("%1:%2")
        .arg(s / 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0')));
}

void CallWindow::setAudioStats(int rtt, int buffer, int tempo, int dropped, int underruns,
                              int minimum, int maximum, int reserve) {
    QString text = QCoreApplication::translate("ChatGui",
        "Buffer: %1 ms · Reserve: %2 ms\nOver 10 s: minimum %3 · maximum %4 ms\nSpeed: %5× · RTT: %6 "
        "ms\nBuffer drops: %7 · Underruns: %8")
        .arg(buffer).arg(reserve).arg(minimum).arg(maximum)
        .arg(tempo / 100.0, 0, 'f', 2).arg(rtt).arg(dropped).arg(underruns);
    auto* audio = CallAudioEngine::instance();
    if (audio->callId() == m_callId) {
        auto output = audio->outputLatency();
        auto value = [](int ms) { return ms >= 0 ? QString::number(ms) : QStringLiteral("—"); };
        QString total = value(output.outputMs);
        if (output.outputMs >= 0 && !output.measured) total.prepend(QStringLiteral("~"));
        text += QLatin1Char('\n') + QCoreApplication::translate("ChatGui", "Audio output: %1 ms (includes app queue: %2 ms)")
            .arg(total).arg(value(output.queueMs));
    }
    QString html = text.toHtmlEscaped();
    static const QRegularExpression values(QStringLiteral("(~?(?:\\d+(?:[.,]\\d+)?|—)(?:\\h*(?:ms|мс|s|с|×))?)"));
    html.replace(values, QStringLiteral("<b style=\"color: %1;\">\\1</b>").arg(palette().color(QPalette::WindowText).name()));
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>")); m_statsLabel->setText(html);
    m_statsLabel->setVisible(m_state == Active);
}
