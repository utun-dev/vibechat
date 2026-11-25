/* GUI-страница известных узлов SQLite: таблица и подробности собираются из одного LEFT JOIN nodes/node_addresses.
 * refreshNodes читает самостоятельный снимок в общем DB worker; виджеты/переводы форматируются только в GUI.
 * Одновременно один READ; повторные обновления объединяются, после смены БД прежний результат не применяется.
 * Ошибка чтения/данных очищает таблицу и показывает Query error, отсутствие БД — Database not connected; повторный Refresh допустим. */
#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>

class DbManager;
class DbReadView;
class QVariant;
struct NodesSnapshot;

class NodesPage : public QWidget {
    Q_OBJECT
public:
    /* db заимствован через QPointer, nullptr допустим. Создание не читает список узлов. */
    explicit NodesPage(DbManager* db, QWidget* parent = nullptr);
    /* GUI: заменить таблицу/подробности полным результатом либо явным состоянием ошибки; этапы/причины логируются в CONFIG. */
    void refreshNodes();

private slots:
    void onRefreshClicked(); // Ручное обновление; также refreshNodes вызывает диалог при выборе категории.

private:
    static int readNodes(DbReadView& view, QVariant* value); // Worker: один LEFT JOIN, проверка IP/SQL, возврат SQLITE_OK/кода ошибки.
    void applyNodes(const NodesSnapshot& snapshot); // GUI: таблица и переведённый текст из одного готового снимка, без SQL.
    static QString shortenKey(const QByteArray& key); // HEX uppercase: первые/последние 4 байта, при длине <8 целиком.
    static QString addrStr(const void* addr, int family); // Проверенный сетевой адрес: family=4/6, обязательные 4/16 байт до возврата.
    static QString protoStr(int protocol); // UDP/TCP/REALITY из битмаски; неизвестный обычный protocol — число.

    QPointer<DbManager> m_db; // Не владеет менеджером/SQLite; закрытие отменяет применение прежнего результата.
    quint64 m_generation = 0; // Токен READ, меняется также при явной очистке из-за отсутствия БД.
    bool m_pending = false, m_reload = false; // Один READ и необходимость повторить после нового запроса обновления.
    QTableWidget*    m_table; // Дочерняя read-only таблица: ID, имя, X25519, Ed25519, число адресов.
    QPlainTextEdit*  m_dumpText; // Дочерние подробности того же результата либо сообщение об ошибке.
    QPushButton*     m_refreshBtn; // Дочерняя кнопка повторного чтения.
};
