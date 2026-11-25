/* GUI-composer канала/PM: текст, emoji, справка форматирования, ответ/редактирование, голосовая запись и preview вложения.
 * Сам не отправляет данные в сеть: владелец обрабатывает сигналы в GUI и передаёт работу ядру.
 * Для submitAttachment нужен ровно один обработчик, принимающий request вместе с PCM/compressor даже при отказе.
 * Канальный текст/файлы/ответы/правки и изображения из буфера в PM ждут submissionResult; остальные PM-вложения очищаются сразу.
 * Панели и recorder заимствованы, могут быть nullptr и должны жить во время использования; остальные виджеты — дочерние. */
#pragma once

#include <QWidget>
#include <QHash>
#include <QSet>
#include <QPointer>
#include <QThreadPool>
#include <memory>

class QTextEdit;
class QPushButton;
class QCheckBox;
class QLabel;
class QProgressBar;
class QTimer;
class EmojiPanel;
class FormattingPanel;
class AudioRecorder;
class QMimeData;
class QRadioButton;
struct attachment_send_req;
struct ClipboardImage;
struct ClipboardImages;

class InputBar : public QWidget {
    Q_OBJECT
public:
    /* Создать пустой composer; parent владеет виджетом. Перед отправкой задать цель через setRecordingContext.
     * Панели emoji/форматирования взаимно скрываются. Для PTT recorder должен быть init; его циклом владеет вызывающий. */
    explicit InputBar(EmojiPanel *emojiPanel, FormattingPanel *formattingPanel, AudioRecorder *recorder,
                      QWidget *parent = nullptr);
    ~InputBar() override; // Ждёт image-worker, удаляет свой временный файл, снимает drop-фильтры; заимствованные объекты не уничтожает.

    /* mediaDirBase — каталог media; channelId — канал либо dm:<conv_id>, dmGeneration — поколение PM (для канала 0).
     * card — ID карточки Kanban; смена карточки того же канала также отменяет подготовку/preview.
     * Изменение контекста отменяет edit/reply/preview, image-worker и отложенный drop; обычный текст сохраняется.
     * Запись уже фиксирует прежний target/generation; удаление текущего поколения PM отдельно очищает его черновик. */
    void setRecordingContext(const QString& mediaDirBase, const QString& channelId, quint64 dmGeneration = 0, const QString& card = {});
    /* Очистить текст/reply/preview и остановить запись только при совпадении target/generation.
     * Известные задачи target исключаются из индикатора; их поздние завершения игнорируются. Другой выбранный адресат не меняется. */
    void forgetDmHistory(const QString& target, quint64 generation);
    /* Заимствовать target, включить acceptDrops и установить общий фильтр URL-drop; повтор/null — no-op.
     * Уничтожение target снимает регистрацию, destructor composer — фильтр. acceptDrops обратно не переключается.
     * Drop только готовит preview; отправку инициируют кнопка/Enter. */
    void installFileDropTarget(QWidget* target);

    /* Режим PM скрывает/снимает групповое шифрование вложения; метод также разрешает PTT/скрепку.
     * Ограничение media/edit нужно применять после него через setMediaEnabled. */
    void setDmMode(bool enabled);
    void setMediaEnabled(bool enabled); // Разрешить media-кнопки с учётом edit; false не останавливает уже идущую запись.
    /* Состояние подготовки job в target: 0=start, 1=готово, 2=ошибка; job — непрозрачный UUID задачи.
     * Кеширует и для невыбранных целей; индикатор показывает только текущую, активные задачи имеют приоритет над ошибкой. */
    void updatePreparation(const QString& target, const QByteArray& job, int state);
    /* Выбрать ответ в канале: messageId ровно 32 байта, author/preview — plain text, пробелы в подписи упрощаются.
     * PM/пустая цель/неверный ключ отклоняются; edit сначала отменяется. Новый ответ имеет собственное поколение. */
    void setReply(const QByteArray& messageId, const QString& author, const QString& preview, const QString& quote = {});
    void clearReply(); // Снять ответ и сменить поколение; текст/preview вложения не очищаются, старые результаты не применяются.
    /* Подтвердить канальный текст/файл, изображение из буфера в PM, правку либо одну часть ответа по request.
     * Ошибка сохраняет черновик и показывает отказ. Успех очищает лишь неизменённую ревизию текста/вложения;
     * ответ снимается после успеха всех частей и очистки черновика. Старое поколение/другая цель не затрагиваются. */
    void submissionResult(quint64 request, bool success, bool tooLarge = false);
    /* Открыть edit канального сообщения: id=32 байта, version>0, text — исходный текст.
     * Сохраняет обычный черновик/reply, скрывает вложение; PM, pending edit/reply или PTT дают отказ с логом. */
    void editMessage(const QByteArray& id, quint64 version, const QString& text);
    void cancelEdit(); // Восстановить обычный черновик/reply/preview; late result edit игнорируется, запрос ядра не отменяется.

    static quint64 nextSubmissionId() { return ++s_submissionSequence; } // Общий GUI-only ID отправки, включая отдельную видеозапись.
    void setVideoRecordingEnabled(bool enabled); // Включает действие только при поддержке захвата/кодирования в приложении.
signals:
    void videoRecordingRequested(const QString& target, quint64 generation, const QByteArray& reply, const QByteArray& quote);
    void messageAccepted(); // Новая отправка текущего composer подтверждена ядром; правки/отказы/старый target не уведомляют.
    /* Текст текущего контекста; справа удалены только пробелы/табуляции. replyTo пуст либо ключ 32 байта.
     * request ненулевой для канала (включая обычный текст), 0 для PM; обработчик фиксирует текущую цель синхронно. */
    void sendMessage(const QString &text, const QByteArray& replyTo, quint64 request, const QByteArray& quote);
    /* Снимок правки: channel/id/version/text и ненулевой request; ввод read-only до результата/отмены. */
    void saveEdit(const QString& channel, const QByteArray& id, quint64 version, const QString& text, quint64 request);
    /* Передать единственному GUI-обработчику собственный u_calloc-request голоса/видео/JPEG/PNG/зашифрованного файла.
     * target зафиксирован (для PTT при старте); обработчик заполняет inst/target/is_dm и передаёт владение ядру
     * либо освобождает request/PCM/compressor. После сигнала InputBar не использует request. */
    void submitAttachment(const QString& target, struct attachment_send_req* request);
    /* Файл без подготовки видео: source не копируется здесь; dest — запланированный путь, в PM совпадает с source.
     * target/name/reply/request относятся к выбранному preview. Для канала нужен submissionResult; для PM request=0. */
    void sendFileAttachment(const QString& target, const QString& mediaSrc, const QString& mediaDest,
                            const QString& displayName, const QByteArray& replyTo, quint64 request, const QByteArray& quote);
    /* Дополнительные форматы изображения канала; JPEG/PNG идут через submitAttachment также в PM. */
    void sendImageAttachment(const QString& target, const QString& mediaSrc, const QString& mediaDest,
                             const QString& displayName, const QByteArray& replyTo, quint64 request, const QByteArray& quote);
    void attachmentRejected(const QString& reason); // Причина отказа для пользователя; подключён внутренний показ QToolTip.

protected:
    void changeEvent(QEvent* event) override; // Обновляет растровые значки при смене палитры.
    bool eventFilter(QObject *obj, QEvent *event) override; // URL-drop; Enter отправляет, Ctrl+Enter вставляет LF, Escape снимает edit/reply.
    void resizeEvent(QResizeEvent* event) override; // Выравнивает overlay записи по геометрии текстового поля.

private slots:
    void onSendClicked(); // Edit либо вложение и текст отдельными сигналами; повтор текущей pending отправки блокируется.
    void adjustInputHeight(); // От 1 до 12 строк, дальше включает вертикальную прокрутку с сохранением видимости курсора.
    void onPttPressed(); // Запускает recorder и фиксирует target/generation/reply только при успешном старте.
    void onPttReleased(); // Stop; запись от 500 мс передаётся через takeRecording/submitAttachment, меньшая отбрасывается.
    void onAttachClicked(); // QFileDialog → та же подготовка preview, что у drop; контекст проверяется после модального выбора.
    void clearPendingAttachment(); // Снять preview/encrypt и подготовку изображения; удалить только собственный временный исходник.
    void updateRecordingUI(int ms); // Длительность и RMS recorder (-70..0 dB → 0..100), чтение peakLevel потребляет метрику.
    void showRecordOverlay(); // Показать overlay и начальную длительность/уровень по recordingStarted.
    void hideRecordOverlay(); // Скрыть по recordingStopped; не выполняет отправку записи.

private:
    static QString formatFileSize(qint64 bytes); // Человекочитаемые B/KB/MB/GB, шаг 1024; bytes ожидается неотрицательным.
    struct PendingSubmission {
        quint64 generation; // Ревизия выбора ответа/контекста на момент отправки этой части.
        QString target, text, source; // Собственные снимки цели и отправленного текста/исходника; одна из частей может быть пустой.
        int textRevision; // QTextDocument::revision для защиты нового/изменённого черновика от позднего success.
        quint64 attachmentRevision; // Ревизия preview для такой же защиты выбранного файла.
    };
    /* Запомнить канальный текст/файл либо часть ответа; ID общий для всех composer, чтобы late result не попал новой форме.
     * Вложение без source/reply и обычный файл PM не требуют подтверждения: вернуть 0 и ничего не запоминать. */
    quint64 beginSubmission(const QByteArray& reply, quint64 generation, const QString& target,
                           const QString& text = {}, const QString& source = {});
    bool submissionPending() const; // Есть ожидающая часть текущего контекста/выбора ответа.
    QByteArray m_editId, m_savedReply; // Ключ редактируемого сообщения и ответ обычного черновика, сохранённый на время edit.
    QString m_savedText, m_savedReplyLabel; // Обычный текст/подпись ответа для cancelEdit или успешного сохранения.
    quint64 m_editVersion = 0, m_editRequest = 0; // Ожидаемая версия и ненулевой ID pending save; 0 — нет запроса.
    QPushButton* m_sendBtn = nullptr; // Send либо Save edit, действие определяется m_editId.
    QWidget* m_replyPreview = nullptr; // Общий preview ответа или режима edit.
    QLabel* m_replyLabel = nullptr; // Plain text автора/цитаты либо подписи edit.
    QByteArray m_replyTo, m_recordReply; // Выбранный ответ и его снимок для начатой голосовой записи.
    QByteArray m_replyQuote, m_recordQuote, m_savedQuote; // Собственные UTF-8 снимки цитаты текущего/записываемого/сохранённого ответа.
    quint64 m_replyGeneration = 0, m_recordReplyGeneration = 0; // Поколения текущего выбора и ответа голосовой записи.
    static inline quint64 s_submissionSequence = 0; // GUI-only: общий для всех InputBar счётчик request ID, включая edit.
    bool m_submissionFailed = false; // Хотя бы одна часть получила отказ; ответ не снимается при успехе другой части.
    quint64 m_attachmentRevision = 0; // Меняется при замене/очистке preview, не зависит от совпадения пути файла.
    QHash<quint64, PendingSubmission> m_pendingSubmissions; // ID → ожидающая часть; чужие поколения удаляются при получении результата.

    QTextEdit    *m_input; // Plain-text поле и focusProxy composer.
    QPushButton  *m_emojiBtn = nullptr; // Переключает emoji и скрывает справку при её открытии.
    QPushButton  *m_formattingBtn = nullptr; // Переключает справку и скрывает emoji при её открытии.
    QPushButton  *m_pttBtn = nullptr; // Press/start, release/stop, красный фон при активной записи.
    QPushButton* m_videoRecordBtn = nullptr; // Локальная запись; доступность задаёт владелец приложения.
    bool m_videoRecordingEnabled = false;
    QPushButton  *m_attachBtn = nullptr; // Открывает выбор одного локального файла.
    QCheckBox    *m_encryptCheck = nullptr; // Только канал; сбрасывается при смене/очистке preview и переходе в PM.
    EmojiPanel   *m_emojiPanel; // Заимствована; emojiSelected вставляется в текущую позицию курсора.
    FormattingPanel *m_formattingPanel; // Заимствованная панель справки; nullptr скрывает кнопку «?».
    AudioRecorder *m_recorder; // Заимствован, nullptr отключает запуск PTT.
    QWidget      *m_recordOverlay = nullptr; // Дочерний overlay над m_input.
    QLabel       *m_recordLabel = nullptr; // Время записи M:SS.
    QPushButton  *m_cancelRecBtn = nullptr; // Stop без отправки: снимает m_pttActive до остановки recorder.
    QProgressBar *m_levelBar = nullptr; // RMS последнего обработанного блока recorder, 0..100.
    QWidget      *m_attachPreview = nullptr; // Preview выбранного файла, скрыт до успешной подготовки выбора.
    QLabel       *m_attachPreviewLabel = nullptr; // Plain text: имя/размер файла, для изображения из буфера также размеры в пикселях.
    QLabel       *m_attachThumbnail = nullptr; // Миниатюра изображения из буфера; скрыта для обычного файла.
    QWidget      *m_clipboardFormats = nullptr; // Выбор JPEG/PNG с размерами; скрыт для обычного файла.
    QRadioButton *m_clipboardJpeg = nullptr, *m_clipboardPng = nullptr; // Взаимоисключающие варианты; выбранный получает фокус.
    QPushButton  *m_attachClearBtn = nullptr; // Снять preview без удаления исходника.
    bool m_pttActive = false; // Composer начал запись, которую release ещё может отправить.
    bool m_dmMode = false; // PM: общий формат JPEG/PNG/voice/video, групповое шифрование скрыто.
    bool m_mediaEnabled = true; // Запрошенное разрешение media, используется при восстановлении кнопок после edit.
    quint64 m_dmGeneration = 0, m_recordDmGeneration = 0; /* Поколения текущего composer и начатой записи PM. */
    QString m_mediaDirBase; // База планируемых путей вложений; файлы PM без transcode остаются по исходному пути.
    QString m_card; // Карточка Kanban внутри channel; пусто для обычного чата/PM.
    QString m_channelIdForRecord; // Текущий target composer: канал либо dm:<id>.
    QString m_recordTarget; // Target, зафиксированный при старте PTT, независимо от дальнейшего выбора канала.
    QString m_pendingAttachSrc; // Локальный source preview; только изображение из буфера принадлежит composer.
    QString m_pendingAttachDest; // Планируемый mediaDirBase/target/имя либо исходный путь PM.
    QString m_pendingAttachName; // Исходное имя для отображения/метаданных; видео при отправке получает суффикс mp4.
    qint64 m_pendingAttachSize = 0; // Размер на момент выбора, bytes.
    bool   m_pendingAttachAsVideo = false; // Пользователь выбрал подготовку/transcode видео.
    bool   m_pendingAttachAsImage = false; // Картинка в канале; в PM только JPEG/PNG.
    std::shared_ptr<ClipboardImage> m_clipboardDraft; // Собственное изображение черновика; каждой отправке принадлежит отдельная копия.
    std::shared_ptr<ClipboardImages> m_clipboardChoices; // Оба готовых файла; переключение не запускает повторное кодирование.
    QThreadPool m_clipboardPool; // Один image-worker; destructor ждёт завершения, queued-результат владеет своим файлом.
    quint64 m_clipboardGeneration = 0; // Номер вставки/отмены; устаревший результат не меняет preview.
    bool m_clipboardPreparing = false; // Пока последнее изображение кодируется, отправка заблокирована, старый preview сохраняется.
    QLabel* m_preparationLabel = nullptr; // Статус подготовки текущего target.
    QHash<QString, QSet<QByteArray>> m_preparations; // Target → UUID активных задач ядра.
    QSet<QString> m_preparationErrors; // Target с последней ошибкой, снимается при новом start или forget.
    QSet<QByteArray> m_discardedPreparations; /* Завершения задач удалённой истории, ещё ожидаемые из ядра. */
    void showPreparation(); // Обновить индикатор для текущей цели по кешу задач/ошибок.
    void pasteImage(const QMimeData* source); // Забрать изображение/байты буфера, проверить лимиты, запустить сравнение PNG/JPEG в worker.
    void cancelClipboardPreparation(); // Сменить поколение, снять очередь и индикатор; активный worker закончит без применения.
    void selectClipboardFormat(bool jpeg); // Выбрать готовый вариант, обновить preview/ревизию; сохранить текст и выбор шифрования.
    bool canAttachFile() const; // Enabled, выбран target, скрепка разрешена, нет edit/PTT/записи recorder.
    /* Проверить source/context, выбрать режим видео и подготовить preview; true — выбор принят, false — отмена/отказ.
     * Старый preview заменяется лишь после успеха; source остаётся неизменным, для групп/видео создаётся каталог назначения. */
    bool prepareAttachment(const QString& source, quint64 context);
    void rejectAttachment(const QString& reason, const QString& source = QString()); // Лог с target/source и пользовательский сигнал.
    QSet<QObject*> m_dropTargets; // Заимствованные виджеты с фильтром, включая self и viewport; destroyed удаляет внешние записи.
    QPointer<QLabel> m_dropHint; // Собственная drag-подсказка, QObject-parent — текущий drop target; может исчезнуть вместе с ним.
    quint64 m_attachmentContext = 0; // Ревизия контекста, проверяемая до/после модального выбора и в queued drop.
};
