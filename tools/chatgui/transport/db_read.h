/* GUI → общий DB worker → GUI: самостоятельные снимки без доступа worker к форме/ядру.
 * Work захватывает только собственные значения (числа, QString/QByteArray и подобные данные),
 * не this/QObject/QWidget/QPixmap/runtime/SQLite statements.
 * Принятый запрос получает один complete, пока context жив; уничтожение context снимает callback.
 * Остановка до SQL также доставляет CLOSED. Вызовы фабрики/complete — только GUI-поток. */
#pragma once
#include <QVariant>
#include <functional>

class QObject;
struct sqlite3;
struct db_async_result;

/* Worker: db заимствован до возврата; value — собственный результат. Вернуть SQLITE_OK/код SQLite.
 * READ управляет транзакцией; закрыть statements, не писать и не сохранять SQLite-указатели.
 * Исключения из work преобразуются в ошибку; внутри SQLite C-callback исключения нужно перехватить до выхода в C. */
using GuiDbReadWork = std::function<int(sqlite3* db, QVariant* value)>;
/* GUI: result/value заимствованы на время complete; ошибка означает, что value использовать нельзя. */
using GuiDbReadDone = std::function<void(const db_async_result& result, const QVariant& value)>;
using GuiDbReadBackend = bool (*)(sqlite3* expected, QObject* context, GuiDbReadWork work, GuiDbReadDone complete);
/* expected — заимствованный handle только для проверки принадлежности текущему ядру, SQL на нём не выполняется.
 * true — владение work/complete принято; false — callback не будет, caller показывает отказ постановки.
 * context обязателен; отменённый результат не обращается к уничтоженному виджету/DbManager. */
bool gui_bridge_db_read(sqlite3* expected, QObject* context, GuiDbReadWork work, GuiDbReadDone complete);
