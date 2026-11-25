# utun.c — Главная точка входа

## 1. Назначение
Точка входа процесса uTun. Парсинг CLI, запуск в режиме демона, создание и жизненный цикл корневого инстанса `UTUN_INSTANCE`, главный цикл обработки событий `mainloop()`, обработка сигналов (graceful shutdown/reload).

## 2. Как пользоваться
```bash
utun -f -c myconfig.conf -p /var/run/utun.pid -l /var/log/utun.log -d "etcp=debug"
```
- `-f` — foreground (без демонизации)
- `-c` — путь к конфигу (по умолчанию `utun.conf`)
- `-p` — PID-файл (по умолчанию `/var/run/utun.pid`, Windows — нет)
- `-l` — лог-файл (по умолчанию `utun.log`)
- `-d` — отладочная конфигурация (формат: `категория=уровень,...`)
- `-h` — справка

**Порядок запуска:**
1. Парсинг аргументов → инициализация системы отладки
2. `uasync_create()` → `utun_instance_create()` → `utun_instance_init()`
3. Демонизация (`fork`+`setsid`) если не `-f`
4. Запись PID-файла
5. `mainloop`: `uasync_poll(ua, 100)` в цикле пока `instance->running`
6. По `SIGTERM`/`SIGINT`: `g_shutdown=1` → выход из цикла → `utun_instance_destroy()` → `uasync_destroy()` → `u_report_unfreed_blocks()`
7. По `SIGHUP`: `g_reload=1` → `utun_instance_reload()` (селективный или полный)

## 3. API

### Функции
| Функция | Описание |
|---------|----------|
| `parse_args(argc, argv, args)` | Разбор CLI-аргументов через `getopt_long`, заполняет `cmd_args_t` |
| `print_usage(progname)` | Вывод справки по использованию |
| `write_pidfile(pidfile)` | Запись PID текущего процесса в файл |
| `remove_pidfile(pidfile)` | Удаление PID-файла при завершении |
| `daemonize()` | Демонизация: `fork()`→родитель выходит, `setsid()`, перенаправление fd 0/1/2 в `/dev/null` |
| `open_logfile(logfile)` | Открытие лог-файла с line-buffering |
| `signal_handler(sig)` | Обработчик: `SIGINT`/`SIGTERM`→shutdown, `SIGHUP`→reload, вызов `uasync_wakeup()` |
| `main(argc, argv)` | Главная функция: инициализация, демонизация, mainloop, cleanup |

### Структуры
| Структура | Описание |
|-----------|----------|
| `cmd_args_t` | Аргументы командной строки: config_file, pid_file, log_file, debug_config, foreground, help |

### Глобальные переменные
| Переменная | Описание |
|------------|----------|
| `g_shutdown` (sig_atomic_t) | Флаг завершения, выставляется из signal handler |
| `g_reload` (sig_atomic_t) | Флаг перезагрузки конфига по SIGHUP |
| `main_ua` | Указатель на главный UASYNC (для signal handler) |
