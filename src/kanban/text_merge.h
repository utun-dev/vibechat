/* Построчное трёхстороннее слияние UTF-8 текста карточки в общем C-ядре.
 * base — общий предок, left/right — полные версии. Входные строки заимствованы.
 * 0 — *out принадлежит caller/u_free; 1 — конфликт, *out=NULL; -1 — ошибка с логом.
 * Ограничение текста относится к каждой входной версии, результат может быть длиннее. */
#ifndef KANBAN_TEXT_MERGE_H
#define KANBAN_TEXT_MERGE_H
#define KANBAN_TEXT_MAX 4000
int kanban_text_merge(const char* base, const char* left, const char* right, char** out);
#endif
