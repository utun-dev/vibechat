/**
 * Тест настройки категорий отладки из аргументов командной строки
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/debug_config.h"

int main(int argc, char *argv[]) {
    DEBUG_INFO(DEBUG_CATEGORY_ALL, "=== Тест настройки категорий из аргументов ===");
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "Запуск с аргументами: ");
    for (int i = 0; i < argc && pos < (int)sizeof(buf) - 1; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s ", argv[i]);
    }
    DEBUG_INFO(DEBUG_CATEGORY_ALL, "%s", buf);
    
    // Инициализация отладки
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    
    // Анализ аргументов командной строки
    debug_category_t debug_categories = 0;
    
    // Если есть специфические категории, сначала отключаем все
    int has_specific_categories = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug-etcp") == 0) {
            has_specific_categories = 1;
        } else if (strcmp(argv[i], "--debug-queue") == 0) {
            has_specific_categories = 1;
        } else if (strcmp(argv[i], "--debug-memory") == 0) {
            has_specific_categories = 1;
        } else if (strcmp(argv[i], "--debug-none") == 0) {
            has_specific_categories = 1;
        }
    }
    
    // Если есть специфические категории, отключаем все по умолчанию
    if (has_specific_categories) {
        debug_set_categories(DEBUG_CATEGORY_NONE);
        debug_categories = DEBUG_CATEGORY_NONE;
    }
    
    // Теперь обрабатываем аргументы
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug-etcp") == 0) {
            debug_enable_category(DEBUG_CATEGORY_ETCP);
            debug_categories |= DEBUG_CATEGORY_ETCP;
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "✅ Включена категория ETCP");
        } else if (strcmp(argv[i], "--debug-queue") == 0) {
            debug_enable_category(DEBUG_CATEGORY_SYS);
            debug_categories |= DEBUG_CATEGORY_SYS;
            DEBUG_INFO(DEBUG_CATEGORY_SYS, "✅ Включена категория LL_QUEUE");
        } else if (strcmp(argv[i], "--debug-memory") == 0) {
            debug_enable_category(DEBUG_CATEGORY_SYS);
            debug_categories |= DEBUG_CATEGORY_SYS;
            DEBUG_INFO(DEBUG_CATEGORY_SYS, "✅ Включена категория MEMORY");
        } else if (strcmp(argv[i], "--debug-all") == 0) {
            debug_set_categories(DEBUG_CATEGORY_ALL);
            debug_categories = DEBUG_CATEGORY_ALL;
            DEBUG_INFO(DEBUG_CATEGORY_ALL, "✅ Включены все категории");
            break; // Все категории уже включены
        } else if (strcmp(argv[i], "--debug-none") == 0) {
            debug_set_categories(DEBUG_CATEGORY_NONE);
            debug_categories = DEBUG_CATEGORY_NONE;
            DEBUG_INFO(DEBUG_CATEGORY_ALL, "✅ Выключены все категории");
            break; // Все категории уже выключены
        } else if (strcmp(argv[i], "--level") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "error") == 0) {
                debug_set_level(DEBUG_LEVEL_ERROR);
                DEBUG_INFO(DEBUG_CATEGORY_ALL, "✅ Установлен уровень ERROR");
            } else if (strcmp(argv[i], "info") == 0) {
                debug_set_level(DEBUG_LEVEL_INFO);
                DEBUG_INFO(DEBUG_CATEGORY_ALL, "✅ Установлен уровень INFO");
            } else if (strcmp(argv[i], "debug") == 0) {
                debug_set_level(DEBUG_LEVEL_DEBUG);
                DEBUG_INFO(DEBUG_CATEGORY_ALL, "✅ Установлен уровень DEBUG");
            }
        }
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_ALL, "=== Тестовые сообщения ===");
    
    // Сообщения от разных категорий
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Сообщение от ETCP категории");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Сообщение от LL_QUEUE категории");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Сообщение от MEMORY категории");
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "Сообщение от CONNECTION категории");
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "Сообщение от CRYPTO категории");
    
    DEBUG_INFO(DEBUG_CATEGORY_ALL, "=== Информация о текущих настройках ===");
    
    char cat_buf[256];
    int cat_pos = snprintf(cat_buf, sizeof(cat_buf), "Активные категории: ");
    if (debug_categories & DEBUG_CATEGORY_ETCP) cat_pos += snprintf(cat_buf + cat_pos, sizeof(cat_buf) - cat_pos, "ETCP ");
    if (debug_categories & DEBUG_CATEGORY_SYS) cat_pos += snprintf(cat_buf + cat_pos, sizeof(cat_buf) - cat_pos, "LL_QUEUE ");
    if (debug_categories & DEBUG_CATEGORY_SYS) cat_pos += snprintf(cat_buf + cat_pos, sizeof(cat_buf) - cat_pos, "MEMORY ");
    if (debug_categories & DEBUG_CATEGORY_CONNECTION) cat_pos += snprintf(cat_buf + cat_pos, sizeof(cat_buf) - cat_pos, "CONNECTION ");
    if (debug_categories & DEBUG_CATEGORY_CRYPTO) cat_pos += snprintf(cat_buf + cat_pos, sizeof(cat_buf) - cat_pos, "CRYPTO ");
    if (debug_categories == DEBUG_CATEGORY_ALL) cat_pos += snprintf(cat_buf + cat_pos, sizeof(cat_buf) - cat_pos, "ALL ");
    if (debug_categories == DEBUG_CATEGORY_NONE) cat_pos += snprintf(cat_buf + cat_pos, sizeof(cat_buf) - cat_pos, "NONE ");
    DEBUG_INFO(DEBUG_CATEGORY_ALL, "%s", cat_buf);
    
    return 0;
}