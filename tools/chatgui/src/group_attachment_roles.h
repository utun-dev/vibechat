/* Общие роли file/voice/image/video для MessageDelegate в чате и сообщениях карточки.
 * GUI-only: descriptor/localAttrs копируются в роли; db заимствован, нужен только кеш расшифрованных метаданных.
 * mediaDirBase — корень media, channel — группа; функция не запускает передач/декодеров/SQL. */
#pragma once
#include <QByteArray>
#include <QString>
class QStandardItem;
class DbManager;
void setGroupAttachmentRoles(QStandardItem* item, const QByteArray& descriptor, const QString& type,
    const QString& mediaDirBase, const QString& channel, const QByteArray& localAttrs, DbManager* db, const QByteArray& message);
