/* Общие права приглашения: асинхронный preview, выбор переподписания и подтверждённый commit ядра. */
#pragma once
#include <QString>
class QWidget;
void openGroupPolicyDialog(const QString& channel, QWidget* parent);
/* Переименование через подтверждённую команду ядра; неизвестные JSON-поля сохраняются. */
void openGroupRenameDialog(const QString& channel, QWidget* parent);
