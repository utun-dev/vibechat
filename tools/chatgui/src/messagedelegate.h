/* Роли QStandardItemModel и единая геометрия text/voice/file/image/video, цитат, реакций и статусов.
 * GUI-поток: sizeHint/paint/hit-test используют один layout. Координаты option.rect — в viewport view.
 * Медиа открывается стационарным левым кликом по телу; drag/release без press не запускает действие.
 * Общий voice playback принадлежит VoicePlayer/SoundManager; timer/view отслеживаются через QPointer.
 * Paint не запускает I/O/декодеры и не двигает playback clock; позиция обновляется таймером из PCM cursor. */
#pragma once
#include <cstdint>

#include <QStyledItemDelegate>
#include <QPersistentModelIndex>
#include <QVector>
#include <QSharedPointer>
#include <functional>
#include "chatview.h"

struct MessageText;

enum MessageDataRole {
    MsgAuthorRole       = Qt::UserRole + 1,
    MsgTextRole         = Qt::UserRole + 2,
    MsgTimeRole         = Qt::UserRole + 3,
    MsgEditedRole       = Qt::UserRole + 4,
    MsgAvatarRole       = Qt::UserRole + 5,
    MsgHasQuoteRole     = Qt::UserRole + 6,
    MsgQuoteAuthorRole  = Qt::UserRole + 7,
    MsgQuoteTextRole    = Qt::UserRole + 8,
    MsgReactionsRole    = Qt::UserRole + 9,
    MsgIdRole           = Qt::UserRole + 10,
    MsgIsOutgoingRole   = Qt::UserRole + 11,
    MsgContentTypeRole  = Qt::UserRole + 12,
    MsgVoiceDurationRole = Qt::UserRole + 13,
    MsgVoiceFileRole    = Qt::UserRole + 14,
    MsgVoicePlayStateRole = Qt::UserRole + 15,
    MsgVoicePlayPosRole   = Qt::UserRole + 16,
    MsgVoiceWaveformRole  = Qt::UserRole + 17,
    MsgVoiceFileSizeRole  = Qt::UserRole + 18,
    MsgVoiceBlockSizeRole = Qt::UserRole + 19,
    MsgVoiceNumBlocksRole = Qt::UserRole + 21,
    MsgVoiceSigsRole      = Qt::UserRole + 22,
    MsgFileDisplayNameRole = Qt::UserRole + 23,
    MsgFileSizeRole       = Qt::UserRole + 24,
    MsgFileDownloadStateRole = Qt::UserRole + 25,
    MsgChannelIdRole      = Qt::UserRole + 26,
    MsgFileDownloadProgressRole = Qt::UserRole + 27,
    MsgMediaBlocksDoneRole  = Qt::UserRole + 28,
    MsgMediaNumBlocksRole   = Qt::UserRole + 29,
    MsgVoiceDownloadPendingRole = Qt::UserRole + 30,
    MsgImageWidthRole       = Qt::UserRole + 31,
    MsgImageHeightRole      = Qt::UserRole + 32,
    MsgVideoWidthRole       = Qt::UserRole + 33,
    MsgVideoHeightRole      = Qt::UserRole + 34,
    MsgVideoDurationMsRole  = Qt::UserRole + 35,
    MsgDeliveryStateRole   = Qt::UserRole + 36,
    MsgInviteLinkRole      = Qt::UserRole + 37, /* полная ссылка; MsgTextRole содержит локализованный заголовок и ссылку */
    MsgMessageKeyRole      = Qt::UserRole + 38,
    MsgRawTimestampRole    = Qt::UserRole + 39,
    MsgReplyToRole         = Qt::UserRole + 40,
    MsgHighlightRole       = Qt::UserRole + 41,
    MsgEncryptedRole       = Qt::UserRole + 42,
    MsgVersionRole         = Qt::UserRole + 43,
    MsgImagePreviewRole    = Qt::UserRole + 44, /* QImage картинки либо первого видеокадра из общего кеша, подготовленный worker. */
    MsgImageErrorRole      = Qt::UserRole + 45, /* Ошибка подготовки локального медиа; не повторять до изменения исходника/загрузки. */
    MsgAuthorAddressRole  = Qt::UserRole + 47, /* Ed25519 автора. */
    MsgReplyQuoteRole      = Qt::UserRole + 46, /* Неизменяемый полный фрагмент, пусто для обычного ответа. */
};

class MessageDelegate : public QStyledItemDelegate, public ChatSelectionDelegate {
    Q_OBJECT
public:
    static constexpr int kAvatar = 54; // Общий размер аватара; узкий режим убирает его из строки.

    explicit MessageDelegate(QObject *parent = nullptr);

    // Отрисовать строку и вернуть её высоту по ролям модели; model/index должны жить до возврата.
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;
    // Полная ссылка под point в viewport (включая сокращённый invite), либо пусто вне текста/ссылки.
    QString linkAt(const QStyleOptionViewItem& option, const QModelIndex& index, const QPoint& point) const;
    // Номер блока под кнопкой Copy в viewport, -1 вне кнопок; тот же layout, что при рисовании.
    int codeCopyAt(const QStyleOptionViewItem& option, const QModelIndex& index, const QPoint& point) const;
    // UTF-16 границы в видимом тексте; точки локальны строке. Общий расчёт для подсветки, Copy и цитирования.
    QPair<int, int> selectionRange(const QStyleOptionViewItem& option, const QModelIndex& index,
                                   const QPoint& first, const QPoint& last) const override;

    // Привязать положительную длительность и живой PCM id к filePath после успешного VoicePlayer callback.
    static void startAutoPlay(const QString& filePath, float actualDurationSec, uint64_t playbackId);
    // Остановить player, отменить подготовку и очистить общий voice state/timer; повтор безопасен.
    static void stopAllAutoPlay();

    // Настроить до установки делегата в view; false убирает геометрию и hit-test реакций целиком.
    void setReactionsEnabled(bool enabled) { m_reactionsEnabled = enabled; }

    // GUI callbacks заимствуют path/index на время вызова; download возвращает true только после принятия команды.
    void setVideoOpenCallback(std::function<void(const QString&)> cb) { m_videoOpenCb = std::move(cb); }
    void setImageOpenCallback(std::function<void(const QString&)> cb) { m_imageOpenCb = std::move(cb); }
    void setDownloadCallback(std::function<bool(const QModelIndex&)> cb) { m_downloadCb = std::move(cb); }
signals:
    void attachmentOpening(const QModelIndex& index); // Сохранить адрес сообщения до асинхронного открытия.
    void attachmentOpened(const QString& path); // Успешное открытие/воспроизведение, не подготовка превью.
    void avatarRequested(const QString& channel, const QByteArray& owner);
    void reactionRequested(const QModelIndex& index, int emoji); // Переключить существующую реакцию.
    void messageMenuRequested(const QModelIndex& index, const QPoint& globalPos); // Обычный левый click по сообщению.
    void pollRequested(const QString& messageId);
    void replyRequested(const QString& messageId);

private:
    struct ReactionChip {
        QRect rect; // Hit-test и отрисовка в локальных координатах строки.
        int id; QString emoji; int count; bool mine; // ID реакции, текст, счётчик и локальное участие.
    };
    // Все QRect локальны к строке; недействительный прямоугольник означает отсутствующий элемент.
    struct Layout {
        QSharedPointer<const MessageText> formattedText; // Кеш видимого текста/цветов; nullptr сохраняет прежний plain-text путь.
        int viewWidth;
        int bubbleWidth;
        QRect authorRect;
        QRect avatarRect;
        QRect bubbleRect;
        QRect quoteRect;
        QRect quoteBarRect;
        QRect quoteAuthorRect;
        QRect quoteTextRect;
        QRect textRect;
        QRect statusRect;
        QRect deliveryRect;
        int totalHeight;
        bool narrow = false;
        bool emojiOnly = false;
        int emojiCount = 0;
        QRect emojiArea;
        bool isOutgoing = false;
        bool isVoice = false;
        bool isFile = false;
        bool isVideo = false;
        bool isImage = false;
        QRect voicePlayBtnRect;
        QRect voiceWaveformRect;
        QRect voiceDurationRect;
        QRect voiceTimeRect;
        QString voiceDuration;
        int mediaBarColor = 0; /* 0=gray, 1=yellow, 2=green */
        QRect mediaBarRect;
        QRect progressBarRect;
        float progressFrac = 0.0f;
        QRect videoThumbRect;
        QRect videoPlayBtnRect;
        QRect videoDurBadgeRect;
        QRect imageThumbRect;
        QRect imageBadgeRect;
        QRect reactionTime;
        QString reactionTimeText;
        QVector<ReactionChip> reactionChips;
    };

    bool m_reactionsEnabled = true; // Чат поддерживает реакции; обсуждение Kanban — без реакций.
    std::function<void(const QString&)> m_videoOpenCb;
    std::function<void(const QString&)> m_imageOpenCb;
    QPersistentModelIndex m_messagePress; // Кандидат меню; открываем только без drag и в той же строке.
    QPoint m_messagePressPosition;
    QPersistentModelIndex m_avatarPress;
    QPoint m_avatarPressPosition;
    QPersistentModelIndex m_reactionPress;
    QPoint m_reactionPressPosition;
    int m_pressedReaction = -2;
    QPersistentModelIndex m_pollPress;
    QPoint m_pollPressPosition;
    QPersistentModelIndex m_quotePress;
    QPoint m_quotePressPosition;
    QPersistentModelIndex m_mediaPress; // Строка press; инвалидируется при удалении/сбросе модели.
    QPersistentModelIndex m_codePress; // Строка нажатой Copy; копируем лишь тот же неизменённый блок при click без drag.
    QPoint m_codePressPosition;
    int m_codePressBlock = -1;
    QString m_codePressSource;
    QPersistentModelIndex m_copiedCode; // Последняя успешно нажатая Copy; состояние только GUI, не данные сообщения.
    int m_copiedBlock = -1;
    QString m_copiedSource; // Изменённый текст сразу скрывает подтверждение старого блока.
    quint64 m_copyFeedbackSequence = 0; // Повторное нажатие продлевает подтверждение, старый timeout его не снимает.
    QPoint m_mediaPressPosition; // Порог клика <5px, как для цитат/реакций.
    std::function<bool(const QModelIndex&)> m_downloadCb;

    Layout calcLayout(const QStyleOptionViewItem &option,
                      const QModelIndex &index) const;
    Layout calcBodyLayout(const QStyleOptionViewItem& option, const QModelIndex& index) const;
    void paintBody(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index, const Layout& layout) const;
    void drawReactions(QPainter* painter, const Layout& layout, const QStyleOptionViewItem& option) const;
    void drawQuote(QPainter *p, const Layout &L,
                   const QModelIndex &index, const QStyleOptionViewItem &opt) const;
    void drawBubble(QPainter *p, const Layout &L, const QStyleOptionViewItem &opt) const;
    void drawStatusBar(QPainter *p, const Layout &L,
                       const QModelIndex &index, const QStyleOptionViewItem &opt) const;
    void drawAvatar(QPainter *p, const Layout &L,
                    const QModelIndex &index) const;

    static constexpr int kGap      = 8;
    static constexpr int kRadius   = 10;
    static constexpr int kPadH     = 12;
    static constexpr int kPadTop   = 8;
    static constexpr int kPadBot   = 6;
    static constexpr int kTail     = 6;
    static constexpr int kMarginH  = 8;
    static constexpr int kItemGap  = 3;
    static constexpr int kQuoteGap = 4;
    static constexpr int kQuotePadH = 6;
    static constexpr int kQuotePadV = 6;
    static constexpr int kQuoteBarW = 2;
    static constexpr int kQuoteAv  = 16;
    static constexpr int kStatusH  = 18;
};
