/* Подтверждённое desktop-сохранение участника: GUI передаёт снимок, SQL/подпись выполняет uasync.
 * Callback вызывается в GUI только пока жив context; уничтожение context не отменяет уже принятую запись. */
#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

class QObject;
struct sqlite3;

/* GUI-only. db — ожидаемый handle текущего ядра, без разыменования в GUI; channel — канонический ID.
 * ed25519 — 32 байта ключа из открытой формы, повторно проверяется перед записью.
 * nickname сохраняется как UTF-8, пустой снимает псевдоним; ownerJson — частичный JSON или пусто для одного ника.
 * false — отказ до постановки, callback не будет; true — мост владеет копиями и вернёт результат, включая отмену при stop.
 * result: 0=commit, -1=ошибка, -2=конфликт, -3=отмена/другая БД. При уничтожении context callback удаляется.
 * Аргументы/ошибки логируются без ника, ключей и содержимого JSON. */
bool gui_bridge_save_member(sqlite3* db, const QString& channel, const QByteArray& ed25519, const QString& nickname,
                            const QByteArray& ownerJson, QObject* context, std::function<void(int)> complete);

/* GUI-only: скопировать команду передачи admin-ключа текущему ядру; db должен совпасть при выполнении.
 * true — очередь приняла запрос, не подтверждение доставки ключа; false — отказ с логом без оставшейся памяти. */
bool gui_bridge_transfer_admin(sqlite3* db, const QString& channel, const QByteArray& ed);
