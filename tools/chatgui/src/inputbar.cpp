#include <QCoreApplication>
#include "inputbar.h"
#include <QScrollArea>
#include "emojipanel.h"
#include "formattingpanel.h"
#include "audiorecorder.h"
#include "voicemessageencoder.h"
#include "debug_ui.h"
#include "outline_icon.h"
#include <QDebug>
#include <QTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QScrollBar>
#include <QApplication>
#include <QLabel>
#include <QResizeEvent>
#include <QDir>
#include <QDateTime>
#include <QProgressBar>
#include <QIcon>
#include <QTimer>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMimeDatabase>
#include <QImageReader>
#include <QImageIOHandler>
#include <QUrl>
#include <QToolTip>
#include <QBuffer>
#include <QFile>
#include <QImageWriter>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QRadioButton>
#include <QRunnable>
#include <QTemporaryFile>
#include <cmath>
#include <cstring>
#include <functional>

extern "C" {
#include "video/video.h"
#include "media_async/attachment_send.h"
#include "mem.h"
}


namespace {
constexpr qint64 clipboardMaxBytes = 128 * 1024 * 1024;
constexpr qint64 clipboardMaxPixels = 32 * 1024 * 1024;
const QList<QByteArray> clipboardImageFormats = {"image/png", "image/jpeg", "image/bmp", "image/webp"};

bool hasClipboardImage(const QMimeData* source) {
    if (!source) return false;
    if (source->hasImage()) return true;
    for (const auto& format : clipboardImageFormats) if (source->hasFormat(QString::fromLatin1(format))) return true;
    return false;
}

bool validClipboardSize(const QSize& size) {
    return size.width() > 0 && size.height() > 0 && size.width() <= 16384 && size.height() <= 16384 &&
        qint64(size.width()) * size.height() <= clipboardMaxPixels;
}

/* Общая точка вставки для клавиатуры и контекстного меню; обычный текст обрабатывает QTextEdit. */
class ComposerTextEdit final : public QTextEdit {
public:
    ComposerTextEdit(QWidget* parent, std::function<void(const QMimeData*)> pasteImage)
        : QTextEdit(parent), m_pasteImage(std::move(pasteImage)) {}
protected:
    bool canInsertFromMimeData(const QMimeData* source) const override {
        return hasClipboardImage(source) || QTextEdit::canInsertFromMimeData(source);
    }
    void insertFromMimeData(const QMimeData* source) override {
        if (hasClipboardImage(source)) {
            if (isReadOnly()) { GUI_WARN("clipboard image: insertion rejected in read-only composer"); return; }
            m_pasteImage(source);
        } else QTextEdit::insertFromMimeData(source);
    }
private:
    std::function<void(const QMimeData*)> m_pasteImage;
};
}

/* Не QObject: результат worker и GUI совместно держат путь, последний владелец удаляет только своё изображение. */
struct ClipboardImage {
    QString path, name;
    QImage thumbnail;
    QSize dimensions;
    qint64 bytes = 0;
    ~ClipboardImage() {
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.remove()) GUI_WARN("clipboard image: cleanup failed path=%s error=%s", qPrintable(path), qPrintable(file.errorString()));
        else GUI_DEBUG("clipboard image: temporary image removed path=%s", qPrintable(path));
    }
};

/* Оба файла принадлежат черновику до очистки; worker заранее выбирает вариант по правилу размеров. */
struct ClipboardImages {
    std::shared_ptr<ClipboardImage> png, jpeg;
    bool preferJpeg = false;
};

/* Возврат происходит после destructor QTemporaryFile: его native handle уже закрыт, в том числе в Windows. */
static std::shared_ptr<ClipboardImage> writeClipboardImage(const QImage& image, const char* format, int quality,
                                                         QString& error) {
    auto result = std::make_shared<ClipboardImage>();
    const QString suffix = !std::strcmp(format, "jpeg") ? "jpg" : "png";
    QTemporaryFile file(QDir::tempPath() + "/vibechat-clipboard-XXXXXX." + suffix);
    if (!file.open()) { error = file.errorString(); return {}; }
    result->path = file.fileName(); file.setAutoRemove(false);
    QImageWriter writer(&file, format);
    if (quality >= 0) writer.setQuality(quality);
    if (!writer.write(image)) { error = writer.errorString(); return {}; }
    if (!file.flush()) { error = file.errorString(); return {}; }
    result->bytes = file.size();
    return result;
}

/* Оба кодека работают в worker; JPEG (85) выбирается только при выигрыше минимум вдвое. */
static std::shared_ptr<ClipboardImages> encodeClipboardImage(QImage image, const QByteArray& encoded,
                                                           const QString& name, QString& error) {
    if (image.isNull()) {
        QBuffer buffer; buffer.setData(encoded);
        if (!buffer.open(QIODevice::ReadOnly)) { error = buffer.errorString(); return {}; }
        QImageReader reader(&buffer); reader.setAutoTransform(true);
        if (!reader.canRead()) error = reader.errorString();
        else if (!validClipboardSize(reader.size())) error = QStringLiteral("invalid dimensions or more than 32 megapixels");
        else { image = reader.read(); if (image.isNull()) error = reader.errorString(); }
    }
    if (error.isEmpty() && (!validClipboardSize(image.size()) || image.sizeInBytes() > clipboardMaxBytes))
        error = QStringLiteral("image exceeds 16384 pixels per side, 32 megapixels or 128 MiB");
    if (!error.isEmpty()) return {};
    image.setDevicePixelRatio(1);
    QString pngError, jpegError;
    const auto png = writeClipboardImage(image, "png", -1, pngError);
    QImage jpegImage = image;
    if (image.hasAlphaChannel()) {
        jpegImage = QImage(image.size(), QImage::Format_RGB32);
        if (jpegImage.isNull()) jpegError = QStringLiteral("cannot allocate JPEG background");
        else { jpegImage.fill(Qt::white); QPainter painter(&jpegImage); painter.drawImage(0, 0, image); }
    }
    const auto jpeg = jpegError.isEmpty() ? writeClipboardImage(jpegImage, "jpeg", 85, jpegError) : nullptr;
    if (!png) GUI_WARN("clipboard image: PNG encoding failed name=%s reason=%s", qPrintable(name), qPrintable(pngError));
    if (!jpeg) GUI_WARN("clipboard image: JPEG encoding failed name=%s reason=%s", qPrintable(name), qPrintable(jpegError));
    if (!png && !jpeg) { error = QString("PNG: %1; JPEG: %2").arg(pngError, jpegError); return {}; }
    auto result = std::make_shared<ClipboardImages>(); result->png = png; result->jpeg = jpeg;
    result->preferJpeg = jpeg && (!png || png->bytes >= 2 * jpeg->bytes);
    if (png) {
        png->name = name + ".png"; png->dimensions = image.size();
        png->thumbnail = image.scaled(80, 60, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (jpeg) {
        jpeg->name = name + ".jpg"; jpeg->dimensions = image.size();
        jpeg->thumbnail = jpegImage.scaled(80, 60, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    GUI_INFO("clipboard image: compression name=%s png_bytes=%lld jpeg_bytes=%lld jpeg_quality=85 selected=%s alpha_background=white",
             qPrintable(name), (long long)(png ? png->bytes : -1), (long long)(jpeg ? jpeg->bytes : -1), result->preferJpeg ? "jpeg" : "png");
    return result;
}

/* Независимый source для worker: удаление/замена черновика не влияет на запрос, а отказ не мешает повтору. */
static bool copyClipboardImage(const QString& source, struct attachment_send_req* request, QString& error) {
    ClipboardImage temporary;
    QTemporaryFile output(QDir::tempPath() + "/vibechat-clipboard-send-XXXXXX." + QFileInfo(source).suffix());
    if (!output.open()) { error = output.errorString(); return false; }
    temporary.path = output.fileName(); output.setAutoRemove(false);
    const QByteArray path = temporary.path.toUtf8();
    if (path.size() >= int(sizeof(request->source))) { error = QStringLiteral("temporary source path is too long"); return false; }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) { error = input.errorString(); return false; }
    while (!input.atEnd()) {
        const QByteArray bytes = input.read(65536);
        if (input.error() != QFileDevice::NoError) { error = input.errorString(); return false; }
        if (output.write(bytes) != bytes.size()) { error = output.errorString(); return false; }
    }
    if (!output.flush()) { error = output.errorString(); return false; }
    snprintf(request->source, sizeof(request->source), "%s", path.constData());
    request->temporary_source = 1;
    temporary.path.clear();
    return true;
}

static bool isImageFile(const QString& suffix) {
    QString s = suffix.toLower();
    return s == "jpg" || s == "jpeg" || s == "png" || s == "gif" ||
           s == "bmp" || s == "webp";
}

static bool isVideoFile(const QString& suffix) {
    return QStringList{"mp4", "mov", "mkv", "avi", "webm", "m4v"}.contains(suffix.toLower());
}

static QPushButton *makeIconBtn(const QIcon& icon, const QString &tooltip, QWidget *parent) {
    auto *btn = new QPushButton(parent);
    btn->setIcon(icon);
    btn->setIconSize(QSize(20, 20));
    btn->setFixedSize(34, 34);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tooltip);
    btn->setStyleSheet(
        "QPushButton { border: none; border-radius: 4px; background: transparent; }"
        "QPushButton:hover { background: palette(light); }"
    );
    return btn;
}

static QWidget *wrapBottomBtn(QPushButton *btn, int pad) {
    auto *w = new QWidget(btn->parentWidget());
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(btn);
    l->addSpacing(pad);
    return w;
}

QString InputBar::formatFileSize(qint64 bytes) {
    if (bytes < 1024) return QCoreApplication::translate("ChatGui", "%1B").arg(bytes);
    double kb = bytes / 1024.0;
    if (kb < 1024.0) return QCoreApplication::translate("ChatGui", "%1KB").arg(kb, 0, 'f', kb < 10.0 ? 1 : 0);
    double mb = kb / 1024.0;
    if (mb < 1024.0) return QCoreApplication::translate("ChatGui", "%1MB").arg(mb, 0, 'f', mb < 10.0 ? 1 : 0);
    double gb = mb / 1024.0;
    return QCoreApplication::translate("ChatGui", "%1GB").arg(gb, 0, 'f', 1);
}

InputBar::InputBar(EmojiPanel *emojiPanel, FormattingPanel *formattingPanel, AudioRecorder *recorder, QWidget *parent)
    : QWidget(parent)
    , m_input(new ComposerTextEdit(this, [this](const QMimeData* source) { pasteImage(source); }))
    , m_emojiBtn(nullptr)
    , m_pttBtn(nullptr)
    , m_emojiPanel(emojiPanel)
    , m_formattingPanel(formattingPanel)
    , m_recorder(recorder)
{
    m_clipboardPool.setMaxThreadCount(1);
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 0);
    mainLayout->setSpacing(2);
    m_replyPreview = new QWidget(this);
    auto* replyLayout = new QHBoxLayout(m_replyPreview); replyLayout->setContentsMargins(6, 4, 6, 4);
    m_replyLabel = new QLabel(m_replyPreview); m_replyLabel->setTextFormat(Qt::PlainText);
    m_replyLabel->setMinimumWidth(0); m_replyLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_replyLabel->setStyleSheet("border-left: 1px solid palette(highlight); padding-left: 6px;");
    m_replyLabel->setWordWrap(true);
    auto* replyScroll = new QScrollArea(m_replyPreview); replyScroll->setWidgetResizable(true);
    replyScroll->setFrameShape(QFrame::NoFrame); replyScroll->setMaximumHeight(120); replyScroll->setWidget(m_replyLabel);
    replyLayout->addWidget(replyScroll, 1);
    auto* cancelReply = new QPushButton(QString::fromUtf8("✕"), m_replyPreview);
    cancelReply->setToolTip(QCoreApplication::translate("ChatGui", "Cancel reply"));
    cancelReply->setFixedSize(24, 24); replyLayout->addWidget(cancelReply);
    connect(cancelReply, &QPushButton::clicked, this, [this] { if (m_editId.isEmpty()) clearReply(); else cancelEdit(); });
    mainLayout->addWidget(m_replyPreview); m_replyPreview->hide();

    /* Attachment preview bar (hidden until file selected) */
    m_attachPreview = new QWidget(this);
    auto *prevLay = new QHBoxLayout(m_attachPreview);
    prevLay->setContentsMargins(0, 2, 0, 2);
    prevLay->setSpacing(4);
    m_attachThumbnail = new QLabel(m_attachPreview);
    m_attachThumbnail->setObjectName("clipboardImageThumbnail");
    m_attachThumbnail->hide();
    prevLay->addWidget(m_attachThumbnail);
    m_attachPreviewLabel = new QLabel(m_attachPreview);
    m_attachPreviewLabel->setTextFormat(Qt::PlainText);
    m_attachPreviewLabel->setMinimumWidth(0); m_attachPreviewLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_attachPreviewLabel->setStyleSheet("background: palette(light); border-radius: 6px; padding: 2px 8px;");
    prevLay->addWidget(m_attachPreviewLabel, 1);
    m_clipboardFormats = new QWidget(m_attachPreview); m_clipboardFormats->hide();
    auto* formatLayout = new QHBoxLayout(m_clipboardFormats); formatLayout->setContentsMargins(0, 0, 0, 0);
    m_clipboardJpeg = new QRadioButton(m_clipboardFormats); m_clipboardJpeg->setObjectName("clipboardJpegChoice");
    m_clipboardPng = new QRadioButton(m_clipboardFormats); m_clipboardPng->setObjectName("clipboardPngChoice");
    m_clipboardJpeg->setToolTip(QCoreApplication::translate("ChatGui", "JPEG quality: 85. Transparent areas use a white background."));
    m_clipboardPng->setToolTip(QCoreApplication::translate("ChatGui", "PNG preserves the original pixels and transparency."));
    for (auto* button : {m_clipboardJpeg, m_clipboardPng}) { formatLayout->addWidget(button); button->installEventFilter(this); }
    connect(m_clipboardJpeg, &QRadioButton::clicked, this, [this] { selectClipboardFormat(true); });
    connect(m_clipboardPng, &QRadioButton::clicked, this, [this] { selectClipboardFormat(false); });
    prevLay->addWidget(m_clipboardFormats);
    m_encryptCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Encrypt attachment"), m_attachPreview);
    m_encryptCheck->setObjectName("encryptAttachmentCheckBox");
    m_encryptCheck->setToolTip(QCoreApplication::translate("ChatGui", "Encrypt attachment; approve each recipient's key request"));
    prevLay->addWidget(m_encryptCheck);
    connect(m_encryptCheck, &QCheckBox::toggled, this, [this](bool checked) {
        GUI_DEBUG("attachment encryption choice: target=%s source=%s encrypted=%d",
                  qPrintable(m_channelIdForRecord), qPrintable(m_pendingAttachSrc), checked);
    });
    m_attachClearBtn = new QPushButton(QString::fromUtf8("\xE2\x9C\x95"), m_attachPreview);
    m_attachClearBtn->setObjectName("clearAttachmentButton");
    m_attachClearBtn->setFixedSize(20, 20);
    m_attachClearBtn->setCursor(Qt::PointingHandCursor);
    m_attachClearBtn->setStyleSheet("QPushButton { border: none; background: transparent; color: palette(text); }"
                                     "QPushButton:hover { color: #e74c3c; }");
    connect(m_attachClearBtn, &QPushButton::clicked, this, &InputBar::clearPendingAttachment);
    prevLay->addWidget(m_attachClearBtn);
    m_attachPreview->hide();
    mainLayout->addWidget(m_attachPreview);

    m_preparationLabel = new QLabel(this);
    m_preparationLabel->setObjectName("mediaPreparationStatus");
    m_preparationLabel->hide();
    mainLayout->addWidget(m_preparationLabel);

    auto *layout = new QHBoxLayout();
    layout->setSpacing(4);

    m_attachBtn = makeIconBtn(attachmentIcon(palette()),
                              QCoreApplication::translate("ChatGui", "Attach"), this);
    connect(m_attachBtn, &QPushButton::clicked, this, &InputBar::onAttachClicked);
    layout->addWidget(wrapBottomBtn(m_attachBtn, 3), 0, Qt::AlignBottom);

    QPainterPath camera; camera.addRoundedRect(QRectF(2, 5, 14, 14), 2, 2);
    camera.moveTo(16, 9); camera.lineTo(22, 6); camera.lineTo(22, 18); camera.lineTo(16, 15);
    m_videoRecordBtn = makeIconBtn(outlineIcon(camera, palette().color(QPalette::ButtonText)),
        QCoreApplication::translate("ChatGui", "Record video"), this);
    m_videoRecordBtn->setObjectName("recordVideoButton"); m_videoRecordBtn->setEnabled(false);
    connect(m_videoRecordBtn, &QPushButton::clicked, this, [this] {
        if (!canAttachFile()) return;
        emit videoRecordingRequested(m_channelIdForRecord, m_dmGeneration, m_replyTo, m_replyQuote);
    });
    layout->addWidget(wrapBottomBtn(m_videoRecordBtn, 3), 0, Qt::AlignBottom);

    m_input->setAcceptRichText(false);
    m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setTabChangesFocus(true);
    setFocusProxy(m_input);
    m_input->setStyleSheet(
        "QTextEdit { border: 1px solid palette(mid); border-radius: 6px;"
        " padding: 4px 8px; background: palette(base); }"
    );
    m_input->installEventFilter(this);
    QFontMetrics fm(m_input->font());
    m_input->setFixedHeight(fm.lineSpacing() + 12);
    layout->addWidget(m_input, 1);

    m_formattingBtn = makeIconBtn(QIcon(), QCoreApplication::translate("ChatGui", "Message formatting"), this);
    m_formattingBtn->setObjectName(QStringLiteral("messageFormattingHelpButton"));
    m_formattingBtn->setText(QStringLiteral("?"));
    m_formattingBtn->setAccessibleName(m_formattingBtn->toolTip());
    m_formattingBtn->setStyleSheet(m_formattingBtn->styleSheet() + "QPushButton { font-size: 20px; font-weight: bold; }");
    m_formattingBtn->setVisible(m_formattingPanel != nullptr);
    layout->addWidget(wrapBottomBtn(m_formattingBtn, 3), 0, Qt::AlignBottom);

    m_emojiBtn = makeIconBtn(smileIcon(palette()),
                             QCoreApplication::translate("ChatGui", "Emoji"), this);
    m_emojiBtn->setObjectName(QStringLiteral("emojiButton"));
    layout->addWidget(wrapBottomBtn(m_emojiBtn, 3), 0, Qt::AlignBottom);

    m_pttBtn = makeIconBtn(outlineIcon(microphoneOutline(), palette().color(QPalette::ButtonText)),
                           QCoreApplication::translate("ChatGui", "Voice message (hold)"), this);
    GUI_DEBUG("composer: attachment, emoji, formatting and voice buttons initialized icons=vector icon_px=20 stroke=1.8 HiDPI=1x..4x");
    layout->addWidget(wrapBottomBtn(m_pttBtn, 3), 0, Qt::AlignBottom);

    auto *sendBtn = makeIconBtn(QIcon(":/icons/send.png"),
                                QCoreApplication::translate("ChatGui", "Send"), this);
    m_sendBtn = sendBtn;
    sendBtn->setStyleSheet(sendBtn->styleSheet() +
        "QPushButton { border: 1px solid palette(highlight); }");
    layout->addWidget(sendBtn, 0, Qt::AlignBottom);

    connect(m_input, &QTextEdit::textChanged, this, &InputBar::adjustInputHeight);
    connect(sendBtn, &QPushButton::clicked,   this, &InputBar::onSendClicked);

    /* PTT button: press to record, release to stop */
    connect(m_pttBtn, &QPushButton::pressed,  this, &InputBar::onPttPressed);
    connect(m_pttBtn, &QPushButton::released, this, &InputBar::onPttReleased);

    if (m_emojiPanel) {
        connect(m_emojiBtn, &QPushButton::clicked, this, [this]() {
            const bool show = !m_emojiPanel->isVisible();
            if (show && m_formattingPanel) m_formattingPanel->hide();
            m_emojiPanel->setVisible(show);
        });
        connect(m_emojiPanel, &EmojiPanel::emojiSelected, this, [this](const QString &emoji) {
            m_input->insertPlainText(emoji);
            m_input->setFocus();
        });
    }
    if (m_formattingPanel) {
        connect(m_formattingBtn, &QPushButton::clicked, this, [this]() {
            const bool show = !m_formattingPanel->isVisible();
            if (show && m_emojiPanel) m_emojiPanel->hide();
            m_formattingPanel->setVisible(show);
            GUI_DEBUG("composer: formatting panel toggled visible=%d", show);
        });
    }

    /* Recording overlay — appears over the text input during recording */
    m_recordOverlay = new QWidget(this);
    m_recordOverlay->setObjectName(QStringLiteral("recordOverlay"));
    m_recordOverlay->setStyleSheet("#recordOverlay { background: palette(base); border: 1px solid palette(bright-text); border-radius: 6px; }");
    m_recordOverlay->hide();
    auto* roLayout = new QHBoxLayout(m_recordOverlay);
    roLayout->setContentsMargins(12, 0, 8, 0);
    roLayout->setSpacing(6);
    m_recordLabel = new QLabel(m_recordOverlay);
    m_recordLabel->setStyleSheet("color: palette(text); font-size: 14px; font-weight: bold; background: transparent;");
    roLayout->addWidget(m_recordLabel);

    m_levelBar = new QProgressBar(m_recordOverlay);
    m_levelBar->setRange(0, 100);
    m_levelBar->setValue(0);
    m_levelBar->setTextVisible(false);
    m_levelBar->setFixedSize(80, 14);
    m_levelBar->setStyleSheet(
        "QProgressBar { border: 1px solid rgba(255,255,255,0.2); border-radius: 3px; background: rgba(0,0,0,0.3); }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        " stop:0 #27ae60, stop:0.6 #f1c40f, stop:1 #e74c3c); border-radius: 2px; }");
    roLayout->addWidget(m_levelBar);

    roLayout->addStretch();
    m_cancelRecBtn = new QPushButton(QString::fromUtf8("\xE2\x9C\x95"), m_recordOverlay);
    m_cancelRecBtn->setFixedSize(24, 24);
    m_cancelRecBtn->setStyleSheet(
        "QPushButton { border: none; color: palette(text); font-size: 16px; background: transparent; }"
        "QPushButton:hover { color: palette(bright-text); }");
    connect(m_cancelRecBtn, &QPushButton::clicked, this, [this]() {
        m_pttActive = false;
        if (m_recorder && m_recorder->isRecording()) m_recorder->stopRecording();
    });
    roLayout->addWidget(m_cancelRecBtn);

    if (recorder) {
        connect(recorder, &AudioRecorder::recordingStarted, this, &InputBar::showRecordOverlay);
        connect(recorder, &AudioRecorder::recordingStopped, this, [this](int) { hideRecordOverlay(); });
        connect(recorder, &AudioRecorder::durationChanged, this, &InputBar::updateRecordingUI);
    }

    mainLayout->addLayout(layout);
    installFileDropTarget(this);
    installFileDropTarget(m_input->viewport());
    connect(this, &InputBar::attachmentRejected, this, [this](const QString& reason) {
        QToolTip::showText(mapToGlobal(QPoint(8, 0)), reason, this);
    });
}

/* QWidget уничтожает дочерние viewport после полей InputBar: снимаем callbacks до их уничтожения. */
InputBar::~InputBar() {
    cancelClipboardPreparation();
    m_clipboardPool.waitForDone();
    m_clipboardDraft.reset();
    m_clipboardChoices.reset();
    GUI_DEBUG("InputBar: removing file drop targets count=%d", int(m_dropTargets.size()));
    for (auto* target : m_dropTargets) {
        target->removeEventFilter(this);
        disconnect(target, nullptr, this, nullptr);
    }
    m_dropTargets.clear();
    delete m_dropHint.data();
}

/* Смена цели создаёт новую ревизию: поздний результат не очищает следующий ответ. */
void InputBar::setReply(const QByteArray& messageId, const QString& author, const QString& preview, const QString& quote) {
    const QByteArray encoded = quote.toUtf8();
    if (encoded.size() > DB_SYNC_MESSAGE_MAX || encoded.contains('\0')) {
        GUI_ERROR("quote: invalid UTF-8 snapshot bytes=%d", int(encoded.size())); return;
    }
    if (!m_editId.isEmpty()) cancelEdit();
    if (m_dmMode || messageId.size() != 32 || m_channelIdForRecord.isEmpty()) {
        GUI_ERROR("reply: invalid composer target/key bytes=%d", int(messageId.size())); return;
    }
    ++m_replyGeneration; m_replyTo = messageId; m_submissionFailed = false;
    m_replyQuote = encoded;
    m_replyLabel->setText(author.simplified() + "\n" + (quote.isEmpty() ? preview.simplified() : quote));
    m_replyPreview->show(); m_input->setFocus();
    GUI_DEBUG("reply: selected ch=%s id=%s generation=%llu quote_bytes=%d", qPrintable(m_channelIdForRecord),
              messageId.toHex().constData(), (unsigned long long)m_replyGeneration, int(m_replyQuote.size()));
}

void InputBar::clearReply() {
    if (!m_replyTo.isEmpty()) GUI_DEBUG("reply: cleared ch=%s generation=%llu", qPrintable(m_channelIdForRecord),
                                       (unsigned long long)m_replyGeneration);
    ++m_replyGeneration; m_replyTo.clear(); m_replyQuote.clear(); m_submissionFailed = false; m_replyPreview->hide();
}

bool InputBar::submissionPending() const {
    for (const auto& pending : m_pendingSubmissions)
        if (pending.generation == m_replyGeneration && pending.target == m_channelIdForRecord) return true;
    return false;
}

/* Канальный текст и каждая часть ответа имеют собственное подтверждение и снимок черновика. */
quint64 InputBar::beginSubmission(const QByteArray& reply, quint64 generation, const QString& target,
                                const QString& text, const QString& source) {
    if (reply.isEmpty() && text.isEmpty() && (source.isEmpty() || (m_dmMode && !m_clipboardDraft))) return 0;
    const quint64 request = ++s_submissionSequence;
    m_pendingSubmissions.insert(request, {generation, target, text, source, m_input->document()->revision(), m_attachmentRevision});
    GUI_DEBUG("send: pending ch=%s reply=%s request=%llu", qPrintable(target), reply.toHex().constData(), (unsigned long long)request);
    return request;
}

void InputBar::submissionResult(quint64 request, bool success, bool tooLarge) {
    if (!request && !success) {
        m_submissionFailed = true;
        GUI_ERROR("send: untracked attachment rejected target=%s", qPrintable(m_channelIdForRecord));
        emit attachmentRejected(QCoreApplication::translate("ChatGui", "Could not send message. Please try again.")); return;
    }
    if (request && request == m_editRequest) {
        m_editRequest = 0; m_input->setReadOnly(false);
        if (success) cancelEdit();
        else {
            GUI_ERROR("edit: save failed target=%s", qPrintable(m_channelIdForRecord));
            emit attachmentRejected(QCoreApplication::translate("ChatGui", "Could not save edit. The message may have changed. Reopen it to retry."));
        }
        return;
    }
    if (!request || !m_pendingSubmissions.contains(request)) return;
    const auto pending = m_pendingSubmissions.take(request);
    GUI_DEBUG("send: result ch=%s request=%llu success=%d", qPrintable(pending.target), (unsigned long long)request, success);
    if (pending.generation != m_replyGeneration || pending.target != m_channelIdForRecord) return;
    if (!success) {
        m_submissionFailed = true;
        GUI_ERROR("send: submission failed ch=%s request=%llu", qPrintable(pending.target), (unsigned long long)request);
        const QString error = tooLarge
            ? QCoreApplication::translate("ChatGui", "The message and quote exceed the 4000-byte limit. Shorten the reply or select a smaller fragment.")
            : pending.source.isEmpty()
            ? QCoreApplication::translate("ChatGui", "Could not send message. Please try again.")
            : QCoreApplication::translate("ChatGui", "Could not send attachment \"%1\". Please try again.")
                .arg(QFileInfo(pending.source).fileName());
        emit attachmentRejected(error);
        return;
    }
    if (!pending.text.isEmpty() && pending.textRevision == m_input->document()->revision()) m_input->clear();
    if (!pending.source.isEmpty() && pending.attachmentRevision == m_attachmentRevision) clearPendingAttachment();
    if (!submissionPending() && !m_submissionFailed && !m_clipboardPreparing &&
        m_input->toPlainText().isEmpty() && m_pendingAttachSrc.isEmpty()) clearReply();
    emit messageAccepted();
}

/* Черновик и выбранный ответ восстанавливаются после сохранения/отмены. Вложения остаются в composer. */
void InputBar::editMessage(const QByteArray& id, quint64 version, const QString& text) {
    if (m_dmMode || id.size() != 32 || !version || m_editRequest || submissionPending() || m_pttActive) {
        GUI_WARN("edit: composer busy/invalid target=%s", qPrintable(m_channelIdForRecord)); return;
    }
    cancelClipboardPreparation();
    if (!m_editId.isEmpty()) cancelEdit();
    m_savedText = m_input->toPlainText(); m_savedReply = m_replyTo; m_savedReplyLabel = m_replyLabel->text();
    m_savedQuote = m_replyQuote;
    clearReply(); m_editId = id; m_editVersion = version;
    m_replyLabel->setText(QCoreApplication::translate("ChatGui", "Editing message")); m_replyPreview->show();
    m_attachPreview->hide(); m_videoRecordBtn->setEnabled(false); m_attachBtn->setEnabled(false); m_pttBtn->setEnabled(false);
    m_sendBtn->setToolTip(QCoreApplication::translate("ChatGui", "Save edit"));
    m_input->setPlainText(text); m_input->setFocus();
    GUI_INFO("edit: opened target=%s id=%s ver=%llu", qPrintable(m_channelIdForRecord), id.toHex().constData(),
        (unsigned long long)version);
}

void InputBar::cancelEdit() {
    if (m_editId.isEmpty()) return;
    m_editId.clear(); m_editVersion = 0; m_editRequest = 0; m_input->setReadOnly(false);
    m_input->setPlainText(m_savedText); m_savedText.clear();
    m_replyTo = m_savedReply; m_savedReply.clear(); m_replyLabel->setText(m_savedReplyLabel); m_savedReplyLabel.clear();
    m_replyQuote = m_savedQuote; m_savedQuote.clear();
    m_replyPreview->setVisible(!m_replyTo.isEmpty()); m_attachPreview->setVisible(!m_pendingAttachSrc.isEmpty());
    m_attachBtn->setEnabled(m_mediaEnabled); m_pttBtn->setEnabled(m_mediaEnabled);
    m_videoRecordBtn->setEnabled(m_videoRecordingEnabled && m_mediaEnabled);
    m_sendBtn->setToolTip(QCoreApplication::translate("ChatGui", "Send"));
    showPreparation();
}

/* Qt доставляет drop в viewport, а не в QTextEdit/QListView. Родитель composer принимает drop на кнопках. */
void InputBar::installFileDropTarget(QWidget* target) {
    if (!target || m_dropTargets.contains(target)) return;
    target->setAcceptDrops(true);
    target->installEventFilter(this);
    m_dropTargets.insert(target);
    if (target != this) connect(target, &QObject::destroyed, this, [this, target] { m_dropTargets.remove(target); });
    GUI_DEBUG("InputBar: file drop target installed class=%s", target->metaObject()->className());
}

bool InputBar::canAttachFile() const {
    return isEnabled() && m_editId.isEmpty() && !m_channelIdForRecord.isEmpty() && m_attachBtn->isEnabled() &&
        !m_pttActive && (!m_recorder || !m_recorder->isRecording());
}

void InputBar::rejectAttachment(const QString& reason, const QString& source) {
    GUI_WARN("attachment rejected: target=%s source=%s reason=%s", qPrintable(m_channelIdForRecord),
             qPrintable(source), qPrintable(reason));
    emit attachmentRejected(reason);
}

void InputBar::cancelClipboardPreparation() {
    ++m_clipboardGeneration;
    m_clipboardPool.clear();
    m_clipboardPreparing = false;
    showPreparation();
}

void InputBar::pasteImage(const QMimeData* source) {
    if (!canAttachFile()) {
        rejectAttachment(QCoreApplication::translate("ChatGui", "Images cannot be pasted in the current composer state.")); return;
    }
    const quint64 context = m_attachmentContext, generation = ++m_clipboardGeneration;
    const QString target = m_channelIdForRecord;
    m_clipboardPool.clear(); m_clipboardPreparing = true; showPreparation();
    GUI_DEBUG("clipboard image: snapshot target=%s generation=%llu formats=%s", qPrintable(target),
              (unsigned long long)generation, qPrintable(source->formats().join(",")));
    QImage image;
    QByteArray representation = "Qt image";
    if (source->hasImage()) {
        const QVariant data = source->imageData();
        image = qvariant_cast<QImage>(data);
        if (image.isNull() && data.canConvert<QPixmap>()) image = qvariant_cast<QPixmap>(data).toImage();
    }
    QByteArray encoded;
    if (image.isNull()) for (const auto& format : clipboardImageFormats) {
        if (!source->hasFormat(QString::fromLatin1(format))) continue;
        encoded = source->data(QString::fromLatin1(format));
        if (!encoded.isEmpty()) { representation = format; break; }
    }
    if (context != m_attachmentContext || generation != m_clipboardGeneration) {
        GUI_DEBUG("clipboard image: snapshot superseded target=%s generation=%llu", qPrintable(target),
                  (unsigned long long)generation); return;
    }
    if ((!image.isNull() && (!validClipboardSize(image.size()) || image.sizeInBytes() > clipboardMaxBytes)) ||
        encoded.size() > clipboardMaxBytes || (image.isNull() && encoded.isEmpty())) {
        m_clipboardPreparing = false; showPreparation();
        GUI_WARN("clipboard image: invalid snapshot target=%s dimensions=%dx%d image_bytes=%lld encoded_bytes=%lld",
                 qPrintable(target), image.width(), image.height(), (long long)image.sizeInBytes(), (long long)encoded.size());
        rejectAttachment(QCoreApplication::translate("ChatGui", "Cannot paste image: invalid data or more than 32 megapixels / 128 MiB."));
        return;
    }
    const QString name = "clipboard-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
    ++m_attachmentRevision;
    GUI_INFO("clipboard image: preparing target=%s generation=%llu representation=%s dimensions=%dx%d encoded_bytes=%lld",
             qPrintable(target), (unsigned long long)generation, representation.constData(), image.width(), image.height(),
             (long long)encoded.size());
    m_clipboardPool.start(QRunnable::create([this, image, encoded, name, target, context, generation] {
        QString error;
        const auto result = encodeClipboardImage(image, encoded, name, error);
        if (!result) GUI_ERROR("clipboard image: preparation failed target=%s reason=%s", qPrintable(target), qPrintable(error));
        if (!QMetaObject::invokeMethod(this, [this, result, error, target, context, generation] {
            if (context != m_attachmentContext || generation != m_clipboardGeneration) {
                GUI_DEBUG("clipboard image: stale image discarded target=%s generation=%llu", qPrintable(target),
                          (unsigned long long)generation); return;
            }
            m_clipboardPreparing = false; showPreparation();
            if (!result) {
                rejectAttachment(QCoreApplication::translate("ChatGui", "Could not prepare clipboard image: %1").arg(error)); return;
            }
            const auto selected = result->preferJpeg ? result->jpeg : result->png;
            if (!prepareAttachment(selected->path, context)) return;
            m_clipboardChoices = result;
            m_clipboardJpeg->setText(QString("JPEG (%1)").arg(result->jpeg ? formatFileSize(result->jpeg->bytes) : QString::fromUtf8("—")));
            m_clipboardPng->setText(QString("PNG (%1)").arg(result->png ? formatFileSize(result->png->bytes) : QString::fromUtf8("—")));
            m_clipboardJpeg->setEnabled(bool(result->jpeg)); m_clipboardPng->setEnabled(bool(result->png));
            m_clipboardFormats->show(); selectClipboardFormat(result->preferJpeg);
            (result->preferJpeg ? m_clipboardJpeg : m_clipboardPng)->setFocus(Qt::OtherFocusReason);
        }, Qt::QueuedConnection)) GUI_ERROR("clipboard image: cannot queue result target=%s", qPrintable(target));
    }));
}

void InputBar::selectClipboardFormat(bool jpeg) {
    const auto selected = m_clipboardChoices ? (jpeg ? m_clipboardChoices->jpeg : m_clipboardChoices->png) : nullptr;
    if (!selected || !canAttachFile() || m_clipboardPreparing) {
        GUI_WARN("clipboard image: format selection unavailable target=%s jpeg=%d", qPrintable(m_channelIdForRecord), jpeg);
        m_clipboardJpeg->setChecked(m_clipboardChoices && m_clipboardDraft == m_clipboardChoices->jpeg);
        m_clipboardPng->setChecked(m_clipboardChoices && m_clipboardDraft == m_clipboardChoices->png);
        return;
    }
    if (selected != m_clipboardDraft) ++m_attachmentRevision;
    m_clipboardDraft = selected;
    m_pendingAttachSrc = selected->path; m_pendingAttachDest = selected->path;
    m_pendingAttachName = selected->name; m_pendingAttachSize = selected->bytes;
    m_clipboardJpeg->setChecked(jpeg); m_clipboardPng->setChecked(!jpeg);
    m_attachPreviewLabel->setText(QString("%1 (%2, %3×%4)").arg(selected->name, formatFileSize(selected->bytes))
                                .arg(selected->dimensions.width()).arg(selected->dimensions.height()));
    m_attachThumbnail->setPixmap(QPixmap::fromImage(selected->thumbnail)); m_attachThumbnail->show();
    GUI_INFO("clipboard image: format selected target=%s format=%s source=%s dimensions=%dx%d bytes=%lld",
             qPrintable(m_channelIdForRecord), jpeg ? "jpeg" : "png", qPrintable(selected->path),
             selected->dimensions.width(), selected->dimensions.height(), (long long)selected->bytes);
}

bool InputBar::eventFilter(QObject *obj, QEvent *event) {
    if (m_dropTargets.contains(obj)) {
        if (event->type() == QEvent::DragLeave) {
            if (m_dropHint) m_dropHint->hide();
        } else if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            if (!drop->mimeData()->hasUrls()) return QWidget::eventFilter(obj, event);
            if (!canAttachFile() || !(drop->possibleActions() & Qt::CopyAction)) {
                if (m_dropHint) m_dropHint->hide();
                drop->ignore();
                if (event->type() == QEvent::DragEnter)
                    GUI_DEBUG("attachment drag unavailable: target=%s enabled=%d recording=%d actions=%d",
                              qPrintable(m_channelIdForRecord), m_attachBtn->isEnabled(), m_pttActive, int(drop->possibleActions()));
                return true;
            }
            const auto urls = drop->mimeData()->urls();
            QString error;
            if (urls.size() != 1) error = QCoreApplication::translate("ChatGui", "Drop one file at a time.");
            else if (!urls.front().isLocalFile()) error = QCoreApplication::translate("ChatGui", "Only local files can be attached.");
            if (event->type() != QEvent::Drop) {
                auto* target = qobject_cast<QWidget*>(obj);
                if (!m_dropHint) {
                    m_dropHint = new QLabel(target);
                    m_dropHint->setAttribute(Qt::WA_TransparentForMouseEvents);
                    m_dropHint->setAlignment(Qt::AlignCenter);
                    m_dropHint->setWordWrap(true);
                    m_dropHint->setStyleSheet("QLabel { border: 1px dashed palette(highlight); color: palette(text);"
                                             " background: rgba(40, 110, 160, 80); }");
                }
                if (m_dropHint->parentWidget() != target) m_dropHint->setParent(target);
                m_dropHint->setGeometry(target->rect().adjusted(2, 2, -2, -2));
                m_dropHint->setText(error.isEmpty() ? QCoreApplication::translate("ChatGui", "Drop a file to attach") : error);
                m_dropHint->show(); m_dropHint->raise();
                drop->setDropAction(Qt::CopyAction); drop->accept();
                return true;
            }
            if (m_dropHint) m_dropHint->hide();
            if (!error.isEmpty()) { rejectAttachment(error); drop->ignore(); return true; }
            const QString source = urls.front().toLocalFile();
            const quint64 context = m_attachmentContext;
            GUI_INFO("attachment dropped: target=%s source=%s", qPrintable(m_channelIdForRecord), qPrintable(source));
            // Завершаем native drag до возможного модального выбора режима видео; контекст проверяется повторно.
            QTimer::singleShot(0, this, [this, source, context] { prepareAttachment(source, context); });
            drop->setDropAction(Qt::CopyAction); drop->accept();
            return true;
        }
    }
    if ((obj == m_input || obj == m_clipboardJpeg || obj == m_clipboardPng) && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape && !m_editId.isEmpty()) { cancelEdit(); return true; }
        if (ke->key() == Qt::Key_Escape && !m_replyTo.isEmpty()) { clearReply(); return true; }
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (ke->modifiers() & Qt::ControlModifier) {
                m_input->setFocus();
                m_input->insertPlainText("\n");
                return true;
            } else {
                onSendClicked();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void InputBar::adjustInputHeight() {
    m_input->document()->adjustSize();

    int lineSpacing = m_input->fontMetrics().lineSpacing();
    int docH = (int)m_input->document()->size().height() + 12;
    int maxH = 12 * lineSpacing + 12;
    int h = qBound(lineSpacing + 12, docH, maxH);
    m_input->setFixedHeight(h);

    bool needScroll = docH > maxH;
    m_input->setVerticalScrollBarPolicy(needScroll ? Qt::ScrollBarAlwaysOn
                                                   : Qt::ScrollBarAlwaysOff);
    if (needScroll) {
        QTextCursor c = m_input->textCursor();
        if (c.atEnd())
            m_input->verticalScrollBar()->setValue(m_input->verticalScrollBar()->maximum());
        else
            m_input->ensureCursorVisible();
    }
}

void InputBar::onSendClicked() {
    if (!isEnabled() || m_channelIdForRecord.isEmpty()) {
        GUI_WARN("send: composer unavailable target=%s", qPrintable(m_channelIdForRecord)); return;
    }
    if (!m_editId.isEmpty()) {
        const QString text = m_input->toPlainText();
        if (m_editRequest || text.isEmpty()) return;
        m_editRequest = ++s_submissionSequence; m_input->setReadOnly(true);
        emit saveEdit(m_channelIdForRecord, m_editId, m_editVersion, text, m_editRequest); return;
    }
    if (m_clipboardPreparing) {
        GUI_WARN("send: clipboard image still preparing target=%s", qPrintable(m_channelIdForRecord)); return;
    }
    if (submissionPending()) {
        GUI_WARN("send: submission still pending ch=%s", qPrintable(m_channelIdForRecord)); return;
    }
    m_submissionFailed = false;
    QPointer<InputBar> alive(this);
    const quint64 context = m_attachmentContext;
    const QString target = m_channelIdForRecord;
    const QByteArray reply = m_replyTo;
    const QByteArray quote = m_replyQuote;
    QString text = m_input->toPlainText();
    while (!text.isEmpty() && (text.back() == ' ' || text.back() == '\t'))
        text.chop(1);

    bool hasAttach = !m_pendingAttachSrc.isEmpty();
    if (!hasAttach && text.isEmpty()) return;

    if (hasAttach) {
        const QString attachmentSource = m_pendingAttachSrc, attachmentDestination = m_pendingAttachDest, attachmentName = m_pendingAttachName;
        const quint64 requestId = beginSubmission(reply, m_replyGeneration, m_channelIdForRecord, {}, m_pendingAttachSrc);
        const bool encrypted = !m_dmMode && m_encryptCheck->isChecked();
        const bool typedImage = m_pendingAttachAsImage &&
            QStringList{"jpg", "jpeg", "png"}.contains(QFileInfo(attachmentSource).suffix().toLower());
        if (typedImage) {
            QByteArray format; QSize size; QString readError; bool readable;
            { // Reader должен закрыть исходник до синхронного submissionResult, который может удалить черновик изображения.
                QImageReader reader(attachmentSource); reader.setAutoTransform(true);
                format = reader.format().toLower(); size = reader.size(); readable = reader.canRead(); readError = reader.errorString();
                if (reader.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
            }
            const QByteArray source = attachmentSource.toUtf8(), name = attachmentName.toUtf8();
            if (!readable || (format != "jpeg" && format != "png") || size.width() <= 0 || size.height() <= 0 ||
                size.width() > 16384 || size.height() > 16384 || source.size() >= 1024 || name.size() >= 256) {
                GUI_ERROR("image rejected: source=%s format=%s dimensions=%dx%d error=%s", qPrintable(attachmentSource),
                          format.constData(), size.width(), size.height(), qPrintable(readError));
                submissionResult(requestId, false);
                rejectAttachment(QCoreApplication::translate("ChatGui", "Cannot read the image or its dimensions are too large."), attachmentSource);
                return;
            }
            auto* request = (struct attachment_send_req*)u_calloc(1, sizeof(struct attachment_send_req));
            if (!request) { GUI_ERROR("image request allocation failed"); submissionResult(requestId, false); return; }
            request->encrypted = encrypted; request->dm_generation = m_dmGeneration;
            request->info.kind = format == "png" ? ATTACHMENT_PNG : ATTACHMENT_JPEG;
            request->info.width = (uint16_t)size.width(); request->info.height = (uint16_t)size.height();
            snprintf(request->source, sizeof(request->source), "%s", source.constData());
            snprintf(request->info.name, sizeof(request->info.name), "%s", name.constData());
            if (reply.size() == 32) memcpy(request->reply_to, reply.constData(), 32);
            memcpy(request->reply_quote, quote.constData(), size_t(quote.size()));
            request->request_id = requestId;
            if (m_clipboardDraft) {
                QString error;
                if (!copyClipboardImage(attachmentSource, request, error)) {
                    GUI_ERROR("clipboard image: send snapshot failed source=%s reason=%s", qPrintable(attachmentSource), qPrintable(error));
                    attachment_send_discard(request); submissionResult(requestId, false); return;
                }
                GUI_DEBUG("clipboard image: send owns snapshot target=%s draft=%s source=%s request=%llu", qPrintable(target),
                          qPrintable(attachmentSource), request->source, (unsigned long long)requestId);
            }
            GUI_INFO("image queued: target=%s format=%s dimensions=%dx%d encrypted=%d", qPrintable(target),
                     format.constData(), size.width(), size.height(), encrypted);
            emit submitAttachment(target, request);
        } else if (encrypted) {
            auto* request = (struct attachment_send_req*)u_calloc(1, sizeof(struct attachment_send_req));
            QByteArray source = m_pendingAttachSrc.toUtf8(), name = m_pendingAttachName.toUtf8();
            if (!request || source.size() >= (int)sizeof(request->source) || name.size() >= (int)sizeof(request->info.name)) {
                GUI_ERROR("encrypted attachment: invalid path/name or allocation failed");
                u_free(request); submissionResult(requestId, false); return;
            }
            request->encrypted = 1;
            request->info.kind = m_pendingAttachAsVideo ? ATTACHMENT_VIDEO : ATTACHMENT_FILE;
            request->transcode = m_pendingAttachAsVideo;
            if (request->transcode) name = (QFileInfo(m_pendingAttachName).completeBaseName() + ".mp4").toUtf8();
            if (m_pendingAttachAsImage) {
                const QByteArray type = QMimeDatabase().mimeTypeForFile(m_pendingAttachSrc, QMimeDatabase::MatchExtension).name().toUtf8();
                if (type.size() < (int)sizeof(request->content_type)) snprintf(request->content_type, sizeof(request->content_type), "%s", type.constData());
            }
            snprintf(request->source, sizeof(request->source), "%s", source.constData());
            snprintf(request->info.name, sizeof(request->info.name), "%s", name.constData());
            if (reply.size() == 32) memcpy(request->reply_to, reply.constData(), 32);
            memcpy(request->reply_quote, quote.constData(), size_t(quote.size()));
            request->request_id = requestId;
            emit submitAttachment(target, request);
        } else if (m_pendingAttachAsImage) {
            GUI_INFO("send: image=%s name=%s", qPrintable(m_pendingAttachSrc), qPrintable(m_pendingAttachName));
            emit sendImageAttachment(target, attachmentSource, attachmentDestination, attachmentName, reply, requestId, quote);
        } else if (m_pendingAttachAsVideo) {
            GUI_INFO("send: video=%s name=%s", qPrintable(m_pendingAttachSrc), qPrintable(m_pendingAttachName));
            auto* request = (struct attachment_send_req*)u_calloc(1, sizeof(struct attachment_send_req));
            QByteArray source = m_pendingAttachSrc.toUtf8(), name = (QFileInfo(m_pendingAttachName).completeBaseName() + ".mp4").toUtf8();
            if (!request || source.size() >= (int)sizeof(request->source) || name.size() >= (int)sizeof(request->info.name)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "InputBar: video request allocation/path failed");
                u_free(request); submissionResult(requestId, false); return;
            }
            request->info.kind = ATTACHMENT_VIDEO; request->transcode = 1;
            request->dm_generation = m_dmGeneration;
            if (reply.size() == 32) memcpy(request->reply_to, reply.constData(), 32);
            memcpy(request->reply_quote, quote.constData(), size_t(quote.size()));
            request->request_id = requestId;
            snprintf(request->source, sizeof(request->source), "%s", source.constData());
            snprintf(request->info.name, sizeof(request->info.name), "%s", name.constData());
            emit submitAttachment(target, request);
        } else {
            GUI_INFO("send: file=%s name=%s", qPrintable(m_pendingAttachSrc), qPrintable(m_pendingAttachName));
            emit sendFileAttachment(target, attachmentSource, attachmentDestination, attachmentName, reply, requestId, quote);
        }
        if (!alive || context != m_attachmentContext) return;
        if (m_submissionFailed) return;
        if (!requestId) clearPendingAttachment();
    }
    if (!text.isEmpty()) {
        const quint64 request = beginSubmission(reply, m_replyGeneration, m_channelIdForRecord, text);
        emit sendMessage(text, reply, request, quote);
        if (!alive || context != m_attachmentContext) return;
        if (!request && !m_submissionFailed) m_input->clear();
        if (m_emojiPanel) m_emojiPanel->hide();
        if (m_formattingPanel) m_formattingPanel->hide();
        m_input->setFocus();
    }
}

void InputBar::setRecordingContext(const QString& mediaDirBase, const QString& channelId, quint64 dmGeneration, const QString& card) {
    if (m_channelIdForRecord == channelId && m_dmGeneration && m_dmGeneration != dmGeneration)
        forgetDmHistory(channelId, m_dmGeneration);
    if (m_channelIdForRecord != channelId || m_mediaDirBase != mediaDirBase || m_dmGeneration != dmGeneration || m_card != card) {
        cancelEdit();
        clearReply();
        ++m_attachmentContext;
        clearPendingAttachment();
        if (m_dropHint) m_dropHint->hide();
    }
    m_card = card;
    m_encryptCheck->setVisible(card.isEmpty() && !m_dmMode);
    if (!card.isEmpty()) m_encryptCheck->setChecked(false);
    m_pttBtn->setVisible(m_recorder != nullptr && card.isEmpty());
    m_mediaDirBase = mediaDirBase;
    m_channelIdForRecord = channelId;
    m_dmGeneration = dmGeneration;
    showPreparation();
}

/* Старое завершение worker не должно вернуть ошибку/индикатор в повторно открытый PM. */
void InputBar::forgetDmHistory(const QString& target, quint64 generation) {
    m_discardedPreparations.unite(m_preparations.take(target));
    m_preparationErrors.remove(target);
    if (m_recordTarget == target && m_recordDmGeneration == generation) {
        m_pttActive = false;
        if (m_recorder && m_recorder->isRecording()) m_recorder->stopRecording();
        m_recordTarget.clear(); m_recordReply.clear(); m_recordQuote.clear(); m_recordDmGeneration = 0;
        m_pttBtn->setStyleSheet("QPushButton { border: none; border-radius: 4px; background: transparent; }"
                               "QPushButton:hover { background: palette(light); }");
    }
    if (m_channelIdForRecord == target && m_dmGeneration == generation) {
        ++m_attachmentContext; clearPendingAttachment(); clearReply(); m_input->clear();
        if (m_dropHint) m_dropHint->hide();
    }
    GUI_INFO("PM composer cleared: target=%s generation=%llu", qPrintable(target), (unsigned long long)generation);
    showPreparation();
}

void InputBar::setDmMode(bool enabled) {
    m_dmMode = enabled;
    m_encryptCheck->setVisible(!enabled);
    if (enabled) m_encryptCheck->setChecked(false);
    m_pttBtn->setEnabled(true);
    m_attachBtn->setEnabled(true);
}

void InputBar::setVideoRecordingEnabled(bool enabled) {
    m_videoRecordingEnabled = enabled;
    m_videoRecordBtn->setToolTip(QCoreApplication::translate("ChatGui", enabled ? "Record video" : "Video capture requires Qt Multimedia and FFmpeg"));
    m_videoRecordBtn->setEnabled(enabled && m_mediaEnabled && m_editId.isEmpty());
}

void InputBar::setMediaEnabled(bool enabled) {
    m_videoRecordBtn->setEnabled(enabled && m_videoRecordingEnabled && m_editId.isEmpty());
    m_mediaEnabled = enabled;
    if (m_encryptCheck) m_encryptCheck->setEnabled(enabled);
    if (m_pttBtn)    m_pttBtn->setEnabled(enabled && m_editId.isEmpty());
    if (m_attachBtn) m_attachBtn->setEnabled(enabled && m_editId.isEmpty());
    if (!enabled) cancelClipboardPreparation();
    else showPreparation();
    if (!enabled && m_dropHint) m_dropHint->hide();
}

void InputBar::onPttPressed() {
    if (!m_editId.isEmpty()) { GUI_WARN("voice: blocked while editing"); return; }
    if (!isEnabled() || m_channelIdForRecord.isEmpty()) { GUI_WARN("voice: composer unavailable"); return; }
    if (submissionPending() || m_clipboardPreparing) { GUI_WARN("send: recording blocked while submission/image pending"); return; }
    if (!m_recorder || !m_recorder->isInitialized()) return;
    m_recorder->startRecording();
    m_pttActive = m_recorder->isRecording();
    if (!m_pttActive) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice recording did not start; composer remains idle"); return; }
    m_recordTarget = m_channelIdForRecord;
    m_recordDmGeneration = m_dmGeneration;
    m_recordReply = m_replyTo; m_recordReplyGeneration = m_replyGeneration; m_submissionFailed = false;
    m_recordQuote = m_replyQuote;
    m_pttBtn->setStyleSheet(m_pttBtn->styleSheet() +
        "QPushButton { background: #e74c3c; border-radius: 4px; }");
}

void InputBar::onPttReleased() {
    if (!m_pttActive) return;
    m_pttActive = false;
    m_recorder->stopRecording();
    m_pttBtn->setStyleSheet(
        "QPushButton { border: none; border-radius: 4px; background: transparent; }"
        "QPushButton:hover { background: palette(light); }");

    if (!isEnabled()) {
        GUI_INFO("voice: discarded recording while composer unavailable target=%s", qPrintable(m_recordTarget)); return;
    }

    int durationMs = m_recorder->durationMs();
    if (durationMs < 500) {
        GUI_INFO("InputBar: voice message too short (%dms), discarded", durationMs);
        return;
    }

    auto* request = (struct attachment_send_req*)u_calloc(1, sizeof(struct attachment_send_req));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "InputBar: voice request allocation failed"); return; }
    request->info.kind = ATTACHMENT_VOICE; request->preset = VoiceEncoder::preset();
    request->dm_generation = m_recordDmGeneration;
    if (!m_recorder->takeRecording(request)) { u_free(request); return; }
    if (m_recordReply.size() == 32) memcpy(request->reply_to, m_recordReply.constData(), 32);
    memcpy(request->reply_quote, m_recordQuote.constData(), size_t(m_recordQuote.size()));
    request->request_id = beginSubmission(m_recordReply, m_recordReplyGeneration, m_recordTarget);
    emit submitAttachment(m_recordTarget, request);
}

void InputBar::onAttachClicked() {
    if (!canAttachFile()) return;
    const quint64 context = m_attachmentContext;
    QString srcPath = QFileDialog::getOpenFileName(this, QCoreApplication::translate("ChatGui", "Attach file"));
    if (srcPath.isEmpty()) return;
    prepareAttachment(srcPath, context);
}

/* Выбор через скрепку и drop одинаково проверяют файл и заменяют прежний preview только после успеха. */
bool InputBar::prepareAttachment(const QString& srcPath, quint64 context) {
    if (context != m_attachmentContext || !canAttachFile()) {
        rejectAttachment(QCoreApplication::translate("ChatGui", "Attachment cancelled: the chat changed or attachments are unavailable."), srcPath);
        return false;
    }
    QFileInfo fi(srcPath);
    if (!fi.isFile() || !fi.isReadable()) {
        rejectAttachment(QCoreApplication::translate("ChatGui", "Select an existing, readable file."), srcPath); return false;
    }
    if (fi.size() == 0 && (!m_dmMode || isVideoFile(fi.suffix()))) {
        rejectAttachment(QCoreApplication::translate("ChatGui", "Cannot attach an empty file."), srcPath); return false;
    }

    bool asImage = isImageFile(fi.suffix()) && (!m_dmMode || QStringList{"jpg", "jpeg", "png"}.contains(fi.suffix().toLower()));
    bool asVideo = false;
    if (!asImage && isVideoFile(fi.suffix())) {
        QMessageBox box(this);
        box.setWindowTitle(QCoreApplication::translate("ChatGui", "Send video"));
        box.setText(QCoreApplication::translate("ChatGui", "The file contains video. How would you like to send it?"));
        QPushButton *fileBtn = box.addButton(QCoreApplication::translate("ChatGui", "As a file"), QMessageBox::AcceptRole);
        QPushButton *videoBtn = box.addButton(QCoreApplication::translate("ChatGui", "As a video"), QMessageBox::DestructiveRole);
        box.exec();
        asVideo = (box.clickedButton() == videoBtn);
        if (!box.clickedButton() || (!asVideo && box.clickedButton() != fileBtn)) {
            GUI_DEBUG("attachment selection cancelled: video mode source=%s", qPrintable(srcPath));
            return false;
        }
        GUI_INFO("InputBar: video detected, mode=%s",
                 asVideo ? "video" : "file");
    }
    if (context != m_attachmentContext || !canAttachFile()) {
        rejectAttachment(QCoreApplication::translate("ChatGui", "Attachment cancelled: the chat changed or attachments are unavailable."), srcPath);
        return false;
    }
    const QString mediaPath = m_mediaDirBase + "/" + m_channelIdForRecord;
    if ((!m_dmMode || asVideo) && !QDir().mkpath(mediaPath)) {
        GUI_ERROR("attachment media directory unavailable: target=%s path=%s", qPrintable(m_channelIdForRecord), qPrintable(mediaPath));
        rejectAttachment(QCoreApplication::translate("ChatGui", "Cannot create the attachment directory."), srcPath); return false;
    }
    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    const int rnd = rand() & 0xFFFF;

    QString destPath;
    if (m_dmMode && !asVideo) destPath = srcPath;
    else if (asVideo) {
        destPath = QString("%1/%2_%3_%4.mp4")
            .arg(mediaPath, ts).arg(fi.completeBaseName()).arg(rnd, 4, 16, QChar('0'));
    } else {
        destPath = QString("%1/%2_%3_%4.%5")
            .arg(mediaPath, ts).arg(fi.completeBaseName()).arg(rnd, 4, 16, QChar('0'))
            .arg(fi.suffix().isEmpty() ? "bin" : fi.suffix());
    }

    m_encryptCheck->setChecked(false);
    cancelClipboardPreparation();
    m_clipboardDraft.reset();
    m_clipboardChoices.reset(); m_clipboardFormats->hide();
    m_attachThumbnail->clear(); m_attachThumbnail->hide();
    m_pendingAttachSrc  = srcPath;
    ++m_attachmentRevision;
    m_pendingAttachDest = destPath;
    m_pendingAttachName = fi.fileName();
    m_pendingAttachSize = fi.size();
    m_pendingAttachAsVideo = asVideo;
    m_pendingAttachAsImage = asImage;

    QString labelText = QString::fromUtf8("\xF0\x9F\x93\x8E %1 (%2)")
        .arg(fi.fileName(), formatFileSize(fi.size()));
    m_attachPreviewLabel->setText(labelText);
    m_attachPreview->show();

    GUI_INFO("attachment preview ready: target=%s source=%s bytes=%lld video=%d image=%d",
             qPrintable(m_channelIdForRecord), qPrintable(srcPath), (long long)fi.size(), asVideo ? 1 : 0, asImage ? 1 : 0);
    m_input->setFocus();
    return true;
}

void InputBar::clearPendingAttachment() {
    cancelClipboardPreparation();
    m_clipboardDraft.reset();
    m_clipboardChoices.reset();
    if (m_clipboardFormats->isAncestorOf(QApplication::focusWidget())) m_input->setFocus();
    m_clipboardFormats->hide();
    m_attachThumbnail->clear(); m_attachThumbnail->hide();
    m_encryptCheck->setChecked(false);
    ++m_attachmentRevision;
    m_pendingAttachSrc.clear();
    m_pendingAttachDest.clear();
    m_pendingAttachName.clear();
    m_pendingAttachSize = 0;
    m_pendingAttachAsVideo = false;
    m_pendingAttachAsImage = false;
    m_attachPreview->hide();
}

void InputBar::updatePreparation(const QString& target, const QByteArray& job, int state) {
    if (m_discardedPreparations.contains(job)) {
        if (state) m_discardedPreparations.remove(job);
        GUI_DEBUG("PM preparation discarded after deletion: target=%s job=%s state=%d", qPrintable(target), job.toHex().constData(), state);
        return;
    }
    if (state == 0) { m_preparations[target].insert(job); m_preparationErrors.remove(target); }
    else {
        m_preparations[target].remove(job);
        if (m_preparations[target].isEmpty()) m_preparations.remove(target);
        if (state == 2) m_preparationErrors.insert(target);
    }
    showPreparation();
}

void InputBar::showPreparation() {
    QString status;
    m_clipboardFormats->setEnabled(!m_clipboardPreparing && canAttachFile());
    if (m_clipboardPreparing || !m_preparations.value(m_channelIdForRecord).isEmpty())
        status = QCoreApplication::translate("ChatGui", "Preparing media…");
    else if (m_preparationErrors.contains(m_channelIdForRecord)) status = QCoreApplication::translate("ChatGui", "Failed to prepare attachment");
    m_preparationLabel->setText(status);
    m_preparationLabel->setVisible(!status.isEmpty());
}

void InputBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_recordOverlay) {
        QRect r = m_input->geometry();
        int margin = 4;
        m_recordOverlay->setGeometry(r.adjusted(margin, margin, -margin, -margin));
        m_recordOverlay->raise();
    }
}

void InputBar::showRecordOverlay() {
    if (m_recordOverlay) {
        m_recordOverlay->setGeometry(m_input->geometry().adjusted(4, 4, -4, -4));
        m_recordOverlay->show();
        m_recordOverlay->raise();
        m_recordLabel->setText(QCoreApplication::translate("ChatGui", "🔴 Recording 0:00"));
        if (m_levelBar) m_levelBar->setValue(0);
    }
}

void InputBar::hideRecordOverlay() {
    if (m_recordOverlay) m_recordOverlay->hide();
}

void InputBar::updateRecordingUI(int ms) {
    if (m_recordLabel) {
        int secs = ms / 1000;
        m_recordLabel->setText(QCoreApplication::translate("ChatGui", "🔴 Recording %1:%2")
            .arg(secs / 60).arg(secs % 60, 2, 10, QChar('0')));
    }
    if (m_levelBar && m_recorder) {
        float peak = m_recorder->peakLevel();
        float db = peak > 0.0001f ? 20.0f * log10f(peak) : -70.0f;
        int pct = (int)((db + 70.0f) / 70.0f * 100.0f);
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        m_levelBar->setValue(pct);
        const char* color = pct < 60 ? "#27ae60" : pct < 85 ? "#f1c40f" : "#e74c3c";
        m_levelBar->setStyleSheet(QString(
            "QProgressBar { border: 1px solid rgba(255,255,255,0.2); border-radius: 3px; background: rgba(0,0,0,0.3); }"
            "QProgressBar::chunk { background: %1; border-radius: 2px; }").arg(color));
    }
}

void InputBar::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() != QEvent::PaletteChange) return;
    if (m_attachBtn) m_attachBtn->setIcon(attachmentIcon(palette()));
    if (m_videoRecordBtn) {
        QPainterPath camera; camera.addRoundedRect(QRectF(2, 5, 14, 14), 2, 2);
        camera.moveTo(16, 9); camera.lineTo(22, 6); camera.lineTo(22, 18); camera.lineTo(16, 15);
        m_videoRecordBtn->setIcon(outlineIcon(camera, palette().color(QPalette::ButtonText)));
    }
    if (m_emojiBtn) m_emojiBtn->setIcon(smileIcon(palette()));
    if (m_pttBtn) m_pttBtn->setIcon(outlineIcon(microphoneOutline(), palette().color(QPalette::ButtonText)));
    GUI_DEBUG("Composer: palette changed, icon color=%s", qPrintable(palette().buttonText().color().name()));
}
