/* Восстановление при остановленном чате. Вид отказа определяет допустимые действия.
 * Закрытие окна, Enter по умолчанию и Escape выбирают выход. */
#pragma once
#include <QMessageBox>

class StartupFailureDialog final : public QMessageBox {
public:
    enum class Kind { Configuration, Identity, Database, Service };
    enum class Action { Exit, Retry, Edit, ReplaceKey, RecreateDatabase };

    // reason — понятная причина, details — полная диагностика, kind — источник отказа.
    StartupFailureDialog(const QString& reason, const QString& details, Kind kind, QWidget* parent = nullptr);
    Action choice() const; // Выбор пользователя после exec().
private:
    QAbstractButton* m_edit = nullptr;
    QAbstractButton* m_repair = nullptr;
    QAbstractButton* m_retry = nullptr;
    Kind m_kind;
};
