/* Панель буфера своих устройств: Send и отдельные значки типов принятого содержимого.
 * Единственный desktop-владелец bridge callbacks; снимки/drag — в GUI, PNG codec — в одном worker.
 * Деструктор отменяет snapshot и ждёт workers до уничтожения QObject; queued-результаты после destroy удаляет Qt. */
#pragma once
#include <QWidget>
#include <QImage>
#include <QThreadPool>
#include <QTimer>
#include <memory>
#include <vector>
#include <atomic>
#include <QHash>
#include <QPointer>
#include <QIcon>
#include <functional>
class QPushButton;
class QToolButton;
class QMimeData;
class QDialog;
class ClipboardFiles;
struct PreparedClipboardFiles;

class ClipboardExchange : public QWidget {
    Q_OBJECT
public:
    explicit ClipboardExchange(QWidget* parent = nullptr); // parent владеет панелью; bridge должен быть инициализирован.
    ~ClipboardExchange() override;
    void receive(const uint8_t* data, int length); // Полный borrowed CHAT_EVT_CLIPBOARD_RECEIVED; копируется до возврата.
    void result(const uint8_t* data, int length); // Borrowed RESULT; NULL/0 отменяет ожидающие codec/команды при stop сервиса.
    void send(); // Снимок системного clipboard по явному нажатию; выбранный канал не используется.
    void sendTo(const QString& group, const QByteArray& peer, const QString& name); // Адресный снимок участнику; права повторно проверяет ядро.
    void openMember(const QString& group); // Немодальное окно последнего буфера этой группы; системный clipboard меняется только по кнопке.
    void setNameResolver(std::function<QString(const QString&, const QByteArray&)> resolver) { m_nameResolver = std::move(resolver); }
    QString memberSender(const QString& group) const; // Имя последнего отправителя либо пусто; caller обновляет роли списка групп.
    int memberType(const QString& group) const; // TEXT/PNG/FILES/4=каталог;0 нет уведомления.
    static QIcon contentIcon(int type, const QColor& color); // Общие контурные значки содержимого для панели и строки группы.
signals:
    void memberClipboardChanged(const QString& group, const QString& sender, int type); //0 очищает уведомление группы.
protected:
    bool eventFilter(QObject* object, QEvent* event) override; // Drag принятого снимка; обычный клик снова помещает его в clipboard.
    void changeEvent(QEvent* event) override; // Перерисовать контуры при изменении палитры.
private:
    void submit(int type, const QByteArray& data); // Готовый UTF-8/PNG в owned-команду ядра; GUI-only.
    void sendSnapshot(); // Снимает clipboard для цели, зафиксированной send()/sendTo().
    struct Received;
    bool storeMember(const std::shared_ptr<Received>& content); // Ограниченный бюджет ожидающих буферов, ACK после сохранения без копирования.
    void status(const QString& text, bool error = false); // Tooltip и видимое сообщение по результату ручного действия.
    void updateIcon(); // Разные контуры текста/картинки/файла/каталога, без зависимости от шрифтов.
    void retireFiles(bool own = true); // Удерживает четыре старых дерева плюс активные handles;own=false не меняет свой текущий clipboard.
    QMimeData* mime() const; // Новый owned-снимок последнего принятого буфера для clipboard либо QDrag.
    QPushButton* m_send; // Кнопка-значок буфера со стрелкой наружу; disabled при подготовке и отправке.
    QToolButton* m_received; // Дочерний значок типа; до первой доставки скрыт.
    QThreadPool m_worker; // Один codec worker; не читает clipboard/виджеты.
    QThreadPool m_fileWorker; // Отдельный snapshot worker: большие каталоги не задерживают входящий текст/PNG.
    std::shared_ptr<std::atomic<bool>> m_fileCancel; // Stop/destructor отменяет snapshot между блоками, без доступа worker к GUI.
    QTimer m_flash; // Шесть переключений подсветки за 1,5 секунды.
    int m_flashes = 0, m_type = 0;
    quint64 m_request = 0, m_generation = 0; // Корреляция рассылки и поколения входящего codec.
    quint64 m_sendGeneration = 0; // Stop отменяет публикацию готовящегося snapshot без ожидания worker в GUI.
    QString m_text; // Собственный принятый текст.
    QByteArray m_png; // Собственный принятый PNG для drag.
    QImage m_image; // Декодированный принятый PNG для системного буфера.
    std::shared_ptr<ClipboardFiles> m_files; // File promises принятого буфера, без предварительной загрузки.
    std::vector<std::shared_ptr<ClipboardFiles>> m_previousFiles; // Четыре прошлых дерева плюс деревья с открытыми handles ОС.
    std::vector<std::shared_ptr<PreparedClipboardFiles>> m_snapshots; // Исходники предложенных файлов живут до остановки приложения.
    std::shared_ptr<PreparedClipboardFiles> m_pendingSnapshot; // До STARTED держит snapshot; REJECTED освобождает неподтверждённую копию.
    QPoint m_press; // Начало native drag.
    QByteArray m_targetGroup;
    QByteArray m_targetPeer; // Пустой ключ — свои устройства.
    QString m_targetName;
    quint64 m_memberSequence = 0;
    QHash<QString, quint64> m_memberGeneration; // Независимые codec-поколения групп; stop удаляет их и инвалидирует queued results.
    QHash<QString, std::shared_ptr<Received>> m_members; // Последний буфер в каждой из максимум8 групп; суммарно не более64МиБ.
    QHash<QObject*, std::shared_ptr<Received>> m_memberActions; // Кнопки открытых окон удерживают неизменяемое полученное содержимое.
    QHash<QString, QPointer<QDialog>> m_memberDialogs; // Не более одного окна для каждой группы.
    std::function<QString(const QString&, const QByteArray&)> m_nameResolver; // GUI-only lookup локального ника/имени без зависимости от DbManager.
};
