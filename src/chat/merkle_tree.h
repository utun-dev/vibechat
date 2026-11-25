/* merkle_tree — производный индекс хешей в SQLite для сравнения записей модели.
 * В каждом namespace дерево SHA256: корень (уровень 0), 5 уровней по 32 ветви.
 * Лист выбирается по старшим 25 битам ключа; его содержимое хеширует модель.
 * Пустое поддерево имеет нулевой хеш. БД принадлежит вызывающему.
 * Таблица merkle_tree_hash общая для всех наборов; движок передаёт namespace dataset:channel.
 * Все вызовы в потоке владельца SQLite; db/ns/выходные буферы обязательны. */
#ifndef MERKLE_TREE_H
#define MERKLE_TREE_H

#include <stdint.h>
#include <openssl/evp.h>
#include <sqlite3.h>

#define MT_MAX_LEVEL 5
#define MT_BUCKETS 32
#define MT_HASH_SIZE 32

/* Добавить записи листа в уже начатый hash через EVP_DigestUpdate; не завершать hash.
 * Возвращает число записей (0 = пустой лист), отрицательный код SQLite при ошибке
 * (для crypto/прочих ошибок -SQLITE_ERROR). */
typedef int (*merkle_leaf_hash_fn)(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash);

/* Начинает канонический SHA256 узла либо хеширует 32 дочерних хеша (1/0). */
int merkle_tree_digest_begin(EVP_MD_CTX* hash, uint8_t level);
int merkle_tree_branch_hash(uint8_t level, const uint8_t* children, uint8_t hash[MT_HASH_SIZE]);

/* DROP/CREATE всей merkle_tree_hash, включая чужие namespace; не создаёт свою транзакцию.
 * Вызывающий затем восстанавливает индекс из моделей. 0/-1. */
int merkle_tree_init(sqlite3* db);
/* Прочитать хеш уровня 0..5 с нормализованным prefix; отсутствующий узел даёт нули.
 * 0 либо отрицательный код SQLite; при ошибке выходной hash не меняется, включая ошибку finalize. */
int merkle_tree_get(sqlite3* db, const char* ns, uint8_t level, uint64_t prefix, uint8_t hash[MT_HASH_SIZE]);
/* Прочитать 32 дочерних хеша узла уровня 0..4 с нормализованным prefix; отсутствующие — нули.
 * Индекс массива — следующие 5 бит ключа. 0 либо отрицательный код SQLite; при ошибке массив не меняется.
 * Проверяются типы/размеры хешей и нормализованные префиксы из индекса. */
int merkle_tree_children(sqlite3* db, const char* ns, uint8_t level, uint64_t prefix,
        uint8_t hashes[MT_BUCKETS][MT_HASH_SIZE]);
/* Пересчитать лист ключа и родителей в транзакции вызывающего.
 * SHA256(0x4d || level:1 || данные): leaf_hash добавляет данные листа, родитель — 32 дочерних хеша.
 * Пустые узлы удаляются; обход прекращается на первом неизменившемся хеше.
 * 1 = индекс изменился, 0 = прежний хеш, отрицательный код SQLite = ошибка (транзакцию нужно откатить).
 * Код leaf/чтения/записи сохраняется до caller, включая IOERR/CORRUPT для остановки DB worker. */
int merkle_tree_update(sqlite3* db, const char* ns, uint64_t key, merkle_leaf_hash_fn leaf_hash, void* ctx);
/* Оставить старшие level*5 бит ключа, остальные обнулить; уровень 0 — корень. */
uint64_t merkle_sync_level_prefix(uint64_t key, uint8_t level);
/* Число байтов для старших level*5 бит; 0 для корня или недопустимого уровня. */
uint8_t merkle_sync_prefix_bytes(uint8_t level);

#endif
