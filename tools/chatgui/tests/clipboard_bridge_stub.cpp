/* Clipboard bridge для тестов, которые создают ChannelList и подменяют остальные команды ядру.
 * Регистрация callbacks не доставляет события; случайная отправка получает явный отказ, без фиктивного успеха.
 * Не подключать вместе с настоящим gui_bridge_impl.cpp или тестом самого clipboard. */
#include "../transport/gui_bridge.h"
extern "C" {
#include "debug_config.h"
}

extern "C" {
void gui_bridge_set_clipboard_received_cb(gui_dm_messages_fn) {}
void gui_bridge_set_clipboard_result_cb(gui_dm_messages_fn) {}
int gui_bridge_send_clipboard(uint64_t, int, const uint8_t*, size_t, const uint8_t[32], const uint8_t[32]) {
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "test clipboard bridge: send unavailable"); return -1;
}
int gui_bridge_clipboard_applied(const uint8_t*, int) {
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "test clipboard bridge: apply unavailable"); return -1;
}
int gui_bridge_offer_clipboard_files(uint64_t, const struct chat_clipboard_source*, size_t, const uint8_t*, size_t, const uint8_t[32], const uint8_t[32]) {
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "test clipboard bridge: file offer unavailable"); return -1;
}
int gui_bridge_fetch_clipboard_file(const uint8_t[32], const uint8_t[32], const uint8_t[16], uint64_t, const char*, void (*)(void*, int), void*) {
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "test clipboard bridge: file fetch unavailable"); return -1;
}
}
