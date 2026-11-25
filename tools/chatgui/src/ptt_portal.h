/* Wayland PTT через org.freedesktop.portal.GlobalShortcuts v2+, все вызовы/сигналы — поток Qt event loop владельца.
 * Портал хранит одно назначение ptt, объект владеет D-Bus-сессией. initialize не открывает первое назначение без configure.
 * Create/List/Bind выполняются последовательно; Response подключается до вызова метода. ConfigureShortcuts — отдельный вызов.
 * Выдаёт фронты удержания; фиксацией двойного нажатия владеет PttHotkeyManager. Сбои/настройка требуют сброса фиксации. */
#pragma once

#include <QObject>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusMessage>
#include <QTimer>
#include <QVariantMap>

/* D-Bus структура (sa{sv}): id сочетания и свойства, включая отображаемый trigger_description. */
struct PttPortalShortcut { QString id; QVariantMap properties; };
using PttPortalShortcuts = QList<PttPortalShortcut>;
Q_DECLARE_METATYPE(PttPortalShortcut)
Q_DECLARE_METATYPE(PttPortalShortcuts)
/* Сериализуют/читают одну структуру в arg и возвращают тот же аргумент; валидацию ptt-ID выполняет PttPortal. */
QDBusArgument& operator<<(QDBusArgument& arg, const PttPortalShortcut& shortcut);
const QDBusArgument& operator>>(const QDBusArgument& arg, PttPortalShortcut& shortcut);

class PttPortal : public QObject {
    Q_OBJECT
public:
    /* Подключает сигналы session bus; sessionToken — стабильный token идентичности сессии, не handle запроса. */
    explicit PttPortal(QObject* parent = nullptr, const QString& sessionToken = "utun_vibechat_ptt");
    /* Посылает асинхронные Close активному запросу/сессии, не ждёт ответов. */
    ~PttPortal() override;
    /* Проверяет версию, создаёт сессию, читает назначения и связывает известные сочетания.
     * При busy/Ready/Unbound ничего не делает; после Failed допускает явный повтор. */
    void initialize();
    /* Запрашивает системное назначение ptt; parentWindow — строковый portal window ID либо пустая строка.
     * В Ready открывает ConfigureShortcuts, иначе инициирует/продолжает путь к BindShortcuts. Возвращается сразу. */
    void configure(const QString& parentWindow);
    /* true очищает активные удержания и игнорирует Activated; false требует нового Activated для возобновления. */
    void setInhibited(bool inhibited);
    /* trigger_description единственного сочетания; пустая строка — назначение отсутствует. */
    QString description() const { return m_description; }
    /* Локализованный пользовательский статус; технические причины отказа находятся в категории radio. */
    QString status() const { return m_status; }
    /* Checking/Creating/Listing/Binding; не означает, что внешнее окно ConfigureShortcuts закрыто. */
    bool busy() const;

signals:
    /* Перечитать description; выдаётся и при сбросе после ошибки. */
    void descriptionsChanged();
    void statusChanged();
    /* Изменилось удержание ptt с учётом inhibited, независимо от доступности рации. */
    void talkingChanged(bool talking);
    /* Настройка/ошибка требуют снять также фиксацию и историю кликов; reason — диагностическая причина. */
    void resetRequested(const QString& reason);

private slots:
    /* Принимает только Response текущего пути, переводит Create -> List -> Bind -> Ready либо завершает с ошибкой. */
    void response(uint code, const QVariantMap& results, const QDBusMessage& message);
    /* Сигналы фильтруются по текущей сессии/ptt-ID; timestamp/options для удержания не используются. */
    void activated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp, const QVariantMap& options);
    void deactivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp, const QVariantMap& options);
    void shortcutsChanged(const QDBusObjectPath& session, const PttPortalShortcuts& shortcuts); // Снимает удержания и обновляет подписи.
    void sessionClosed(); // Переводит в Failed через fail.

private:
    enum class State { Idle, Checking, Creating, Listing, Unbound, Binding, Ready, Failed }; // Unbound — сессия без привязки.
    /* Один portal Request с заранее вычисленным handle; Create/List имеют таймер 30 с, Bind ждёт пользователя без этого таймера. */
    void request(const QString& method, const QVariantList& arguments, QVariantMap options);
    void bind(); // Связывает единственный ptt-ID; первое назначение может открыть системный диалог.
    void openConfiguration(); // Отдельный асинхронный ConfigureShortcuts, не создаёт Request и не меняет busy.
    void updateDescriptions(const PttPortalShortcuts& shortcuts); // Заменяет подпись ptt из trigger_description.
    void updateTalking(); // Вычисляет !inhibited && pressed, сигнал только при изменении.
    void setStatus(const QString& text); // Запоминает text и выдаёт statusChanged.
    void fail(const QString& detail); // Лог, Close, сброс удержаний/подписей, Failed и пользовательский статус.
    void disconnectRequest(); // Останавливает таймер, отсоединяет Response и очищает путь, без Request.Close.
    void closeSession(); // Инвалидирует старые callbacks, посылает Close запросу/сессии, очищает пути.
    static bool isPttId(const QString& id); // Только ptt.

    uint m_epoch = 0; // Поколение сессии для отбрасывания поздних asyncCall-ответов.
    bool m_configureAfterBind = false; // После восстановления сохранённого ID нужно открыть ConfigureShortcuts.
    State m_state = State::Idle; // Этап сериализованной инициализации/привязки.
    QString m_sessionToken, m_session, m_request, m_parentWindow, m_status; // Token, два D-Bus-пути, window ID и UI-статус.
    QString m_description; // Подпись единственного сочетания ptt.
    bool m_pressed = false; // Удержание ptt текущей сессии.
    QTimer m_requestTimer; // Однократный предел ожидания Create/List Response.
    bool m_configureRequested = false, m_inhibited = false, m_talking = false; // Запрос назначения, запрет, последний talking.
};
