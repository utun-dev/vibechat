/* Режимы трансляции: экран масштабируется пропорционально, камера сохраняет аппаратные размеры.
 * Пустой размер экрана означает ожидание первого кадра системного выбора Wayland.
 * FPS — предел частоты трансляции; Qt захватывает с максимальной частотой выбранного формата.
 * Функции работают только с переданными значениями/списками; сами не опрашивают устройства и не меняют захват.
 * Для звонка VideoCaptureProtocol::validSize дополнительно ограничивает камеру до Full HD. */
#pragma once
#include <QList>
#include <QSize>
#include <algorithm>
#include <cmath>

struct StreamVideoMode {
    QSize size;  /* Размер выходного кадра; пустой допустим для ещё не выбранного системного экрана. */
    int fps = 30; /* Предел выдачи 1..60; конструктор структуры сам его не проверяет. */
    /* Совпадают оба поля; используется для дедупликации и сохранения ручного выбора. */
    bool operator==(const StreamVideoMode&) const = default;
};

/* Вписывает source в длинную сторону 4096 и короткую shortSide, не увеличивая исходник, округляет размеры вниз до чётных.
 * Возвращает пустой QSize при source/shortSide <16 либо если после уменьшения одна из сторон <16. */
inline QSize stream_video_screen_size(QSize source, int shortSide = 4096) {
    if (source.width() < 16 || source.height() < 16 || shortSide < 16) return {};
    double scale = std::min({1.0, 4096.0 / std::max(source.width(), source.height()),
        double(shortSide) / std::min(source.width(), source.height())});
    QSize size(int(std::floor(source.width() * scale)) & ~1, int(std::floor(source.height() * scale)) & ~1);
    return size.width() >= 16 && size.height() >= 16 ? size : QSize();
}

/* Строит максимум и ступени короткой стороны 2160/1440/1080/720/480/360, убирая дубли размеров, с FPS 15/24/25/30/50/60.
 * Первый размер максимальный; для пустого source возвращает шесть режимов с пустым размером, для слишком малого — пустой список. */
inline QList<StreamVideoMode> stream_video_screen_modes(QSize source) {
    QList<StreamVideoMode> modes;
    if (source.isEmpty()) {
        for (int fps : {15, 24, 25, 30, 50, 60}) modes.append({QSize(), fps});
        return modes;
    }
    QSize maximum = stream_video_screen_size(source);
    if (maximum.isEmpty()) return modes;
    QList<QSize> sizes{maximum};
    for (int side : {2160, 1440, 1080, 720, 480, 360}) {
        QSize size = stream_video_screen_size(source, side);
        if (!size.isEmpty() && !sizes.contains(size)) sizes.append(size);
    }
    for (QSize size : sizes) for (int fps : {15, 24, 25, 30, 50, 60}) modes.append({size, fps});
    return modes;
}

/* FPS режима — предел передачи, не команда драйверу. Быстрый capture ограничивается до encode.
 * Проверяет точный size, чётные стороны 16..4096, корректный диапазон minimum..maximum и FPS 1..60 <=maximum+0.1.
 * FPS может быть ниже minimum, поскольку лишние кадры отбрасываются. 29.97/59.94 допускают пределы 30/60. */
inline bool stream_video_camera_supports(QSize size, double minimum, double maximum, const StreamVideoMode& mode) {
    return size == mode.size && size.width() >= 16 && size.height() >= 16 && size.width() <= 4096 && size.height() <= 4096
        && !(size.width() % 2) && !(size.height() % 2) && std::isfinite(minimum) && std::isfinite(maximum)
        && minimum >= 0 && maximum > 0 && minimum <= maximum && mode.fps >= 1 && mode.fps <= 60
        && mode.fps <= maximum + 0.1;
}

/* Добавляет в modes допустимые FPS из 15/24/25/30/50/60 и округлённых с допуском 0.1 границ minimum/maximum.
 * Объединяет дубли разных pixel format; сортирует весь modes по убыванию площади/ширины, затем возрастанию FPS.
 * Невалидный аппаратный формат ничего не добавляет; сама функция не ограничивает камеры до Full HD. */
inline void stream_video_add_camera_modes(QList<StreamVideoMode>& modes, QSize size, double minimum, double maximum) {
    QList<int> rates{15, 24, 25, 30, 50, 60};
    if (std::isfinite(minimum) && minimum >= 1 && minimum <= 60.1) rates.append(int(std::ceil(minimum - 0.1)));
    if (std::isfinite(maximum) && maximum >= 0.9 && maximum <= 60.1) rates.append(int(std::floor(maximum + 0.1)));
    for (int fps : rates) {
        StreamVideoMode mode{size, fps};
        if (stream_video_camera_supports(size, minimum, maximum, mode) && !modes.contains(mode)) modes.append(mode);
    }
    std::sort(modes.begin(), modes.end(), [](const auto& left, const auto& right) {
        if (left.size != right.size) {
            int area = left.size.width() * left.size.height(), other = right.size.width() * right.size.height();
            return area != other ? area > other : left.size.width() > right.size.width();
        }
        return left.fps < right.fps;
    });
}

/* Возвращает индекс режима или -1 для пустого списка, сам список не меняет.
 * При !maximum сначала сохраняет точный previous; иначе/при отсутствии ищет FPS previous/30 с размером
 * modes.front().size (maximum) либо 1280x720. Затем берёт первый режим 30 FPS, в крайнем случае индекс 0.
 * Для выбора максимума вызывающий должен передать список с максимальным размером в начале. */
inline int stream_video_select_mode(const QList<StreamVideoMode>& modes, StreamVideoMode previous, bool maximum) {
    if (modes.isEmpty()) return -1;
    if (!maximum) {
        int index = modes.indexOf(previous);
        if (index >= 0) return index;
    }
    QSize size = maximum ? modes.front().size : QSize(1280, 720);
    for (int fps : {previous.fps, 30}) {
        int index = modes.indexOf({size, fps});
        if (index >= 0) return index;
    }
    for (int i = 0; i < modes.size(); i++) if (modes[i].fps == 30) return i;
    return 0;
}
