/* Файловый протокол участников CHAT-группы. Codec не проверяет членство/подпись/шифрование: это делает сервис.
 * Все числа на проводе little-endian.
 * Группа и подписанный отправитель приходят от router; owner — Ed25519 владельца каталога.
 * version — непрозрачный снимок объекта, upload — случайный ID временного файла.
 * Нулевая version в BEGIN означает создание; замена требует version из STAT.
 * LIST: cursor — число уже выданных записей; version фиксирует снимок каталога.
 * READ/WRITE: offset и size задают диапазон; WRITE.data_len обязан равняться size.
 * COMMIT/MKDIR/RENAME/REMOVE: RESPONSE BUSY.cursor — срок до освобождения квитанции в0.1мс;
 * 0 означает обычный повтор через5с. Общий клиентский срок120с не продлевается.
 * Данные LIST — последовательность entry: name_len:2,type:1,size:8,mtime:8,version:16,name.
 * Токены меняются при перезапуске сервера. Никаких ABI-зависимых структур в wire. */
#ifndef MEMBER_FILES_PROTOCOL_H
#define MEMBER_FILES_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MF_BLOCK_SIZE 8192
#define MF_PROTOCOL_VERSION 5
#define MF_PATH_MAX 1024
#define MF_NAME_MAX 255
#define MF_ADDRESS_SIZE 32
#define MF_PATH_STORAGE_MAX (MF_PATH_MAX + 131)
#define MF_WIRE_HEADER 149
#define MF_EVENT_RESPONSE_PREFIX (2 * MF_ADDRESS_SIZE)
#define MF_EVENT_TRANSFER_GROUP 8
#define MF_EVENT_TRANSFER_SERVER (MF_EVENT_TRANSFER_GROUP + MF_ADDRESS_SIZE)
#define MF_EVENT_TRANSFER_OWNER (MF_EVENT_TRANSFER_SERVER + MF_ADDRESS_SIZE)
#define MF_EVENT_TRANSFER_DONE (MF_EVENT_TRANSFER_OWNER + MF_ADDRESS_SIZE)
#define MF_EVENT_TRANSFER_TOTAL (MF_EVENT_TRANSFER_DONE + 8)
#define MF_EVENT_TRANSFER_STATUS (MF_EVENT_TRANSFER_TOTAL + 8)
#define MF_EVENT_TRANSFER_STATE (MF_EVENT_TRANSFER_STATUS + 2)
#define MF_EVENT_TRANSFER_SIZE (MF_EVENT_TRANSFER_STATE + 1)
#define MF_WIRE_MAX (MF_WIRE_HEADER + 2 * MF_PATH_MAX + MF_BLOCK_SIZE)
#define MF_ENTRY_HEADER 35
#define MF_QUOTA_BYTES 40 /* used/limit/reserved в байтах, files/directories; пять u64 LE */
#define MF_AVATAR_MAX_BYTES (1024 * 1024)
#define MF_AVATAR_ORIGINAL_MAX_BYTES (32 * 1024 * 1024)
#define MF_AVATAR_MANIFEST_MAX_BYTES 4096
/* Имя объекта аватарки:64 lowercase hex и .png/.jpg/.webp/.json; вход — NUL-строка, NULL даёт0.
 * valid/manifest_valid возвращают1/0; file_limit — лимит32MiB для изображения,4096 для JSON,0 при неверном имени. */
int mf_avatar_file_valid(const char* file);
int mf_avatar_manifest_valid(const char* file);
uint64_t mf_avatar_file_limit(const char* file);
#define MF_IDLE_TB 100000ULL /* 10 секунд */
#define MF_REPLAY_TB 1200000ULL /* Окно повторов запроса/успешной квитанции:120с. */
#define MF_REQUEST 1
#define MF_RESPONSE 2
#define MF_FILE 1
#define MF_DIRECTORY 2

/* MF_REMOVE — системный макрос Win32; для удаления файла/каталога используем отдельное имя. */
enum mf_command {
    MF_LIST = 1, MF_STAT, MF_READ, MF_BEGIN_WRITE, MF_WRITE, MF_COMMIT, MF_ABORT, MF_MKDIR, MF_RENAME, MF_REMOVE_PATH,
    MF_STREAM_WRITE, MF_STREAM_READ, MF_STREAM_DATA, MF_STREAM_END, MF_STREAM_CANCEL, MF_QUOTA
};
enum mf_status {
    MF_OK, MF_ACCESS_DENIED, MF_NOT_FOUND, MF_INVALID_PATH, MF_INVALID_RANGE, MF_EXISTS,
    MF_BUSY, MF_STALE, MF_NO_SPACE, MF_IO_ERROR, MF_BAD_PACKET, MF_HASH_MISMATCH, MF_CANCELLED, MF_TIMEOUT, MF_DIRECTORY_NOT_EMPTY
};

struct mf_message {
    uint8_t kind, command; // MF_REQUEST/RESPONSE и mf_command; group/src берутся отдельно из router.
    uint8_t type, more; // MF_FILE/DIRECTORY и признак продолжения LIST/READ (0/1).
    uint16_t status; // mf_status; у запроса всегда MF_OK.
    uint64_t request_id; // Ненулевой ID одной неизменной команды; у DATA/END/CANCEL — ID потока.
    uint8_t owner[MF_ADDRESS_SIZE]; // Ed25519 владельца; нули обозначают виртуальный корень группы.
    uint64_t offset, size; // Диапазон READ/WRITE/DATA либо полный размер BEGIN/END/STAT.
    uint64_t cursor; // LIST: число выданных записей; открытие STREAM:ID потока; mutation BUSY:retry_after_tb.
    uint64_t mtime; // Unix seconds в ответах/записях каталога.
    uint8_t version[16]; // Непрозрачный снимок объекта; нули BEGIN означают создание.
    uint8_t upload[16]; // Случайный токен temp/потока; проверяется вместе с group/peer/owner/path.
    uint8_t hash[32]; // SHA256 полного файла для BEGIN/COMMIT/END.
    char path[MF_PATH_MAX + 1], target[MF_PATH_MAX + 1]; // NUL-строки; target используется RENAME.
    uint32_t data_len; // Длина data; фактический предел MF_BLOCK_SIZE, wire использует u16.
    uint8_t data[MF_BLOCK_SIZE]; // Байты файла, LIST entries или QUOTA, по команде.
};

/* Прочитать/записать ровно8 байт LE; буферы обязательны и имеют достаточный размер. */
uint64_t mf_get64(const uint8_t* p);
void mf_put64(uint8_t* p, uint64_t value);
/* 1 — все n байт нулевые, иначе0. */
int mf_zero(const uint8_t* p, size_t n);
/* 1/0: строгий UTF-8, путь<=1024 байт, до60 компонентов по1..255 байт; root_allowed разрешает пустую строку.
 * Отвергает leading '.', trailing '.'/' ', ASCII C0/DEL и Windows device names.
 * Запрещены обратная косая черта, двоеточие, '*', '?', '"', '<', '>', '|'. */
int mf_path_valid(const char* path, int root_allowed);
/* Записать wire: длина>0 либо-1 при переполнении; выход не меняется при отказе длины.
 * Проверяет размеры, но не семантику command/ID/path. message/wire обязательны и не перекрываются. */
int mf_encode(const struct mf_message* message, uint8_t* wire, size_t capacity);
/* 0/-1: проверить формат/точную длину/общие поля и декодировать. При ошибке message может быть изменён.
 * Невалидный UTF-8/путь и смысл полей отдельных команд проверяет вызывающий сервис/storage. */
int mf_decode(const uint8_t* wire, size_t length, struct mf_message* message);
/* Заимствованные статические строки; неизвестное значение возвращает Unknown error/UNKNOWN. */
const char* mf_status_text(int status);
const char* mf_command_text(int command);
#ifdef __cplusplus
}
#endif
#endif
