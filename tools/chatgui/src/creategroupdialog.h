/* Модальный диалог ввода имени канала, только GUI-поток: exec() → Accepted → groupName().
 * Поле ограничено 128 единицами UTF-16, пустое после trim имя не принимается.
 * Создание канала/выдача ID остаются вызывающему коду; reject не запускает никаких действий ядра. */
#pragma once

#include <QDialog>
#include <QLineEdit>

class QComboBox;
class CreateGroupDialog : public QDialog {
    Q_OBJECT
public:
    explicit CreateGroupDialog(QWidget* parent = nullptr);

    QString groupType() const;
    QString groupName() const; // Текущее имя с trim по краям; проверять результат диалога перед использованием.

private:
    QComboBox* m_type;
    QLineEdit* m_nameEdit; // Дочернее поле имени, освобождается Qt вместе с диалогом.
};
