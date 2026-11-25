// Linux: регистрация приложения в меню/desktop portal и пользовательский XDG Autostart.
#pragma once
#include <QString>

class QWidget;
// Открывает INI в редакторе платформы. При ошибке показывает путь в окне parent.
void openConfigurationEditor(const QString& configPath, QWidget* parent);

// Создаёт отсутствующий org.utun.vibechat.desktop в XDG data applications.
// executable — абсолютный путь к GUI; готовые системные/user записи не заменяет.
// При наличии системной записи удаляет собственный старый user-ярлык, перекрывающий иконку и Exec из DEB.
// Возвращает false с предупреждением в логе при ошибке записи; вне Linux возвращает true.
bool registerDesktopApplication(const QString& executable);

#ifdef Q_OS_LINUX
// Читает эффективный autostart/org.utun.vibechat.desktop из XDG config, учитывая пользовательское отключение.
// Возвращает false при отсутствии записи, Hidden=true, отключении GNOME или ошибке чтения (с логом).
bool desktopAutostartEnabled();

// Включает запуск executable (абсолютный путь) с --minimized при входе пользователя в графический сеанс.
// При выключении удаляет user-запись; системную запись при наличии перекрывает через Hidden=true.
// Возвращает false с логом при ошибке, true после записи/удаления; прав администратора не требует.
bool setDesktopAutostartEnabled(bool enabled, const QString& executable);
#endif
