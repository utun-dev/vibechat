/* Подготовка immutable snapshot файлов/каталогов и чтение полученного обещанного дерева.
 * prepare вызывается worker: обходит дерево, копирует regular-файлы, считает SHA256; открытие/обход не проходит по ссылкам.
 * receive/publish/drag — GUI; read native provider работает в своём потоке и ждёт только uasync callback.
 * Локальные snapshot/cache/mount принадлежат shared owners; провайдер снимается до удаления mount-каталога. */
#pragma once
#include "clipboard_native.h"
#include <QByteArray>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>
#include <vector>
class QWidget;
extern "C" {
#include "chat/chat_clipboard.h"
}
struct PreparedClipboardFiles {
    std::shared_ptr<QTemporaryDir> snapshot; // Собственные файлы, доступные до отзыва опубликованного offer.
    std::vector<chat_clipboard_source> sources; // Проверенные пути snapshot; после send копируются в ядро.
    QByteArray manifest; // Только относительные имена/типы/размеры/UUID/SHA256, без путей отправителя.
};
class ClipboardFiles {
public:
    // Worker-only, NULL с error. cancel проверяется перед объектами и каждым блоком копирования 1 МиБ.
    static std::shared_ptr<PreparedClipboardFiles> prepare(const QStringList& paths, QString& error,
        std::shared_ptr<std::atomic<bool>> cancel = {});
    static std::shared_ptr<ClipboardFiles> receive(const QByteArray& manifest, const QByteArray& group, const QByteArray& peer, QString& error);
    bool publish(); // Публикует file promises в системный clipboard; не скачивает содержимое.
    void drag(QWidget* source); // Native copy drag; вызывается из GUI при удерживаемой левой кнопке.
    bool directory() const { return m_directory; } // Единственный каталог либо набор с каталогами.
    int roots() const { return int(m_tree->roots.size()); }
    bool active() const { return m_tree->users.load() != 0; } // Открытые handles проводника удерживают provider старого буфера.
private:
    std::shared_ptr<QTemporaryDir> m_mount; // Объявлен раньше native: снимается после unmount.
    std::shared_ptr<ClipboardFileTree> m_tree; // Неизменяемый manifest + cancelable cache.
    std::shared_ptr<ClipboardNative> m_native; // FUSE/Windows Shell, отдельный worker.
    bool m_directory = false;
};
