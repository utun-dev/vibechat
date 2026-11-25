/* Временная политика Linux/UVC: отключает EXPOSURE_AUTO_PRIORITY, позволяющий снижать частоту при автоэкспозиции.
 * Не задаёт FPS и не меняет режим самой автоэкспозиции. activate — при активной камере, restore — при остановке.
 * Объект не копируется, вызовы сериализует владелец; вне Linux методы ничего не делают. */
#pragma once
#include <QByteArray>
#include "debug_config.h"
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <cerrno>
#include <cstring>
#endif

class CallCameraRate {
#ifdef Q_OS_LINUX
    int fd = -1; /* Открытый device только после успешного изменения флага; -1 — восстанавливать нечего. */
#endif
public:
    CallCameraRate() = default;
    CallCameraRate(const CallCameraRate&) = delete;
    CallCameraRate& operator=(const CallCameraRate&) = delete;
    ~CallCameraRate() { restore(); }
    /* Если activate изменил ненулевой флаг на 0, пытается записать 1 и закрывает fd даже при ошибке.
     * Когда флаг уже был 0, restore его не трогает; повтор безопасен. */
    void restore() {
#ifdef Q_OS_LINUX
        if (fd < 0) return;
        v4l2_control control{}; control.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY; control.value = 1;
        if (ioctl(fd, VIDIOC_S_CTRL, &control) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call camera dynamic fps restore failed: %s", std::strerror(errno));
        else DEBUG_INFO(DEBUG_CATEGORY_CALL, "call camera dynamic fps restored=1");
        ::close(fd); fd = -1;
#endif
    }
    /* Сначала restore прежнего устройства; device — путь Qt/V4L2 вида /dev/video*.
     * Читает флаг и при ненулевом значении ставит 0; неподдерживаемое управление/ошибка не запрещают захват, логируются. */
    void activate(const QByteArray& device) {
        restore();
#ifdef Q_OS_LINUX
        if (!device.startsWith("/dev/video")) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call camera fps policy unavailable device=%s", device.constData()); return;
        }
        int camera = ::open(device.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (camera < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call camera fps control open failed device=%s: %s", device.constData(), std::strerror(errno));
            return;
        }
        v4l2_control control{}; control.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
        if (ioctl(camera, VIDIOC_G_CTRL, &control) < 0) {
            if (errno == EINVAL) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call camera dynamic fps control unsupported device=%s", device.constData());
            else DEBUG_WARN(DEBUG_CATEGORY_CALL, "call camera fps control read failed: %s", std::strerror(errno));
        } else if (!control.value) {
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "call camera dynamic fps already disabled device=%s", device.constData());
        } else {
            control.value = 0;
            if (ioctl(camera, VIDIOC_S_CTRL, &control) < 0)
                DEBUG_WARN(DEBUG_CATEGORY_CALL, "call camera dynamic fps disable failed: %s", std::strerror(errno));
            else {
                fd = camera;
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "call camera dynamic fps disabled device=%s; auto exposure retained", device.constData());
                return;
            }
        }
        ::close(camera);
#else
        Q_UNUSED(device);
#endif
    }
};
