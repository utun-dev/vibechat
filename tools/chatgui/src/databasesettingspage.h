/* GUI-страница обслуживания SQLite: информация о файле/WAL, quick_check, integrity_check и optimize через DbManager.
 * Quick/full читают через общий DB worker; callbacks приходят в GUI и снимаются при уничтожении страницы.
 * Optimize пока использует прежний синхронный writer; его перенос требует передачи единственного писателя worker.
 * DbManager заимствован через QPointer; его уничтожение/смена БД не запускает отложенную операцию на новой базе.
 * Кнопки блокируются до результата/ошибки постановки, при отсутствии менеджера остаются выключены. */
#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>

class DbManager;

class DatabaseSettingsPage : public QWidget {
    Q_OBJECT
public:
    /* Создать форму и прочитать информацию о файлах; nullptr/неоткрытая БД отключают кнопки операций. */
    explicit DatabaseSettingsPage(DbManager* db, QWidget* parent = nullptr);
    /* Обновить доступность кнопок и доступную информацию о файлах; PRAGMA не запускает.
     * Подробности последней проверки сохраняются; при отсутствии БД подпись статуса сообщает о неподключённой БД. */
    void refreshStatus();

private slots:
    void onQuickCheck(); // Запросить quick_check после проверки наличия handle; результат показывается после выполнения SQL.
    void onFullCheck(); // Аналогично для integrity_check со сверкой индексов.
    void onOptimize(); // Запросить обслуживание статистики планировщика, успех не означает проверку целостности.

private:
    void showResult(const QString& checkName, const struct IntegrityResult& result); // Показать ok/ошибку, время и сообщения; включить кнопки.
    void setBusy(bool busy); // Переключить кнопки; busy=true также показывает Checking. SQL/worker не запускает.

    QPointer<DbManager> m_db; // Не владеет менеджером; обнуляется при его уничтожении.
    quint64 m_dbGeneration = 0; // Смена привязки отменяет действие отложенного legacy optimize.
    bool m_busy = false; // Не разрешает повторный запуск при refreshStatus/переподключении БД до завершения операции.
    QLabel*         m_filePathLabel; // Дочерняя подпись пути sqlite3_db_filename(main), доступна для копирования.
    QLabel*         m_fileSizeLabel; // Дочерняя подпись размера основного файла по QFileInfo.
    QLabel*         m_walSizeLabel; // Дочерняя подпись известного размера -wal; n/a для отсутствующего WAL у найденного файла БД.
    QPushButton*    m_quickBtn; // Дочерняя кнопка быстрой проверки.
    QPushButton*    m_fullBtn; // Дочерняя кнопка полной проверки.
    QPushButton*    m_optimizeBtn; // Дочерняя кнопка PRAGMA optimize.
    QLabel*         m_statusLabel; // Дочерняя подпись состояния/последнего результата и времени SQL.
    QPlainTextEdit* m_resultText; // Дочерний read-only текст первого и дополнительных сообщений результата.
};
