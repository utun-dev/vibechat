/* Native file promises для проводника: Linux read-only FUSE, Windows Shell virtual files.
 * Дерево неизменно. read вызывается в native worker, может ждать сеть; GUI/uasync не блокируются.
 * cancel обязан разблокировать read. Владелец уничтожает provider вне его callbacks, перед остановкой ядра. */
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
struct ClipboardFileItem {
    std::string path; // UTF-8 относительно корня обещанного дерева, '/' между компонентами.
    std::uint64_t size = 0; // Для каталога 0.
    bool directory = false;
};
class ClipboardFileTree {
public:
    virtual ~ClipboardFileTree() = default;
    std::vector<ClipboardFileItem> items; // Не меняется после передачи native provider.
    std::vector<std::string> roots; // Верхние объекты clipboard/drag.
    std::atomic<unsigned> users{0}; // Открытые handles ОС; старое дерево не снимается во время копирования.
    virtual int read(std::size_t index, char* data, std::size_t size, std::uint64_t offset) = 0; // Число байтов либо -1.
    virtual void cancel() = 0; // Thread-safe; отменяет ожидания без уничтожения объекта из callback.
};
class ClipboardNative {
public:
    virtual ~ClipboardNative() = default;
    virtual std::vector<std::string> paths() const = 0; // Linux: локальные FUSE URI; Windows: пусто.
    virtual bool publish() = 0; // Windows: OleSetClipboard; Linux: публикация URI принадлежит Qt.
    virtual bool drag() = 0; // Windows: native DoDragDrop в GUI-потоке; Linux: QDrag с paths().
};
using ClipboardNativeLog = std::function<void(bool error, const std::string& message)>;
/* mountPath — существующий пустой собственный каталог (Linux); живёт до destructor provider.
 * Возвращает NULL с error при недоступности механизма ОС. logs не обращается к GUI. */
std::shared_ptr<ClipboardNative> createClipboardNative(std::shared_ptr<ClipboardFileTree> tree,
    const std::string& mountPath, std::string& error, ClipboardNativeLog logs);
