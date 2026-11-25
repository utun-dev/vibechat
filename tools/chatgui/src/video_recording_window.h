/* Панель локальной записи и просмотра. Владеет engine/player и временным черновиком до подтверждения отправки.
 * Адресат, карточка Kanban, поколение DM и reply фиксируются конструктором; смена текущего чата их не меняет.
 * close во время записи вызывает stop и открывает просмотр; discard удаляет окно/черновик.
 * submitAttachment передаёт собственный request (включая независимый временный снимок) единственному обработчику. */
#pragma once
#include "video_recording_engine.h"
#include <QWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QJsonObject>
class QMenu;
class QLabel;
class QPushButton;
class QCheckBox;
class QSlider;
class VideoRenderWidget;
class VideoPlayerEngine;
struct attachment_send_req;
class VideoRecordingWindow : public QWidget {
    Q_OBJECT
public:
    VideoRecordingWindow(const QString& target, const QString& recipient, quint64 generation, const QByteArray& reply,
                         const QString& settings, const QString& card = {}, QWidget* parent = nullptr, const QByteArray& quote = {});
    ~VideoRecordingWindow() override;
    void stopForPhone() { m_engine->stopForPhone(); } // Закрыть capture перед входом в телефон.
    bool released() const { return m_engine->released(); }
    void submissionResult(quint64 request, bool success, bool tooLarge = false); // Только свой request; отказ сохраняет черновик.
    void invalidate(const QString& target, quint64 generation); // Удалённое поколение DM: остановить, запретить отправку.
signals:
    void settingsChanged(const QString& settings);
    void microphoneChanged(const QString& id);
    void submitAttachment(const QString& target, attachment_send_req* request);
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    QString m_target; // Исходный канал либо dm:<id>.
    QString m_card; // Постоянный hex ID карточки Kanban; пусто для CHAT/DM, никогда не берётся из текущего UI при отправке.
    quint64 m_generation, m_request = 0; // Зафиксированный DM epoch и ожидающий submission ID.
    QByteArray m_reply; // Исходный ответ канального сообщения.
    QByteArray m_quote; // UTF-8 снимок цитаты, зафиксированный при открытии записи.
    bool m_invalid = false, m_discard = false, m_preparing = false, m_restored = false;
    QTemporaryDir m_directory; // Только собственный черновик; снимки отправки создаются вне каталога.
    QThreadPool m_jobs; // Единственный worker копирования; destructor ждёт без доступа к GUI.
    VideoRecordingEngine* m_engine;
    VideoPlayerEngine* m_player;
    VideoRenderWidget* m_preview;
    QMenu *m_source, *m_resolution, *m_fps, *m_codec, *m_bitrate, *m_qp, *m_rotation, *m_microphone;
    QCheckBox *m_cursor, *m_adaptive, *m_mute, *m_encrypt;
    QLabel *m_status, *m_stats;
    QPushButton *m_start, *m_pause, *m_send, *m_delete, *m_play;
    QSlider* m_seek;
    QList<StreamVideoSource> m_sources;
    QJsonObject m_saved;
    QString m_path; // Готовый, закрытый файл; пуст до finished.
    StreamPublishConfig selection() const;
    void refreshSources();
    void refreshModes();
    void synchronize();
    void saveSettings();
    void send(); // Асинхронный снимок → собственный request, без автоматической отправки по stop.
};
