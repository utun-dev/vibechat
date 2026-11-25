#include "stream_screen_cursor.h"
#include "debug_config.h"
#include <QGuiApplication>
#include <QPainter>
#include <QtGui/qguiapplication_platform.h>
#include <memory>
#include <X11/extensions/Xfixes.h>

StreamScreenCursor::StreamScreenCursor(QScreen* screen) : m_screen(screen), m_origin(screen) {
    auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!screen || !native || !(m_display = native->display())) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: X11 display or screen unavailable"); return;
    }
    int event = 0, error = 0, major = 1, minor = 0;
    if (!XFixesQueryExtension(m_display, &event, &error) || !XFixesQueryVersion(m_display, &major, &minor) || major < 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: XFixes 1.0 unavailable"); return;
    }
    m_origin.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::BypassWindowManagerHint);
    m_origin.setGeometry(QRect(screen->geometry().topLeft(), QSize(1, 1)));
    m_origin.create(); // Не показываем: окно служит только для перевода координат X11.
    m_ready = true;
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop cursor opened screen=%s origin=%d,%d scale=%.3f XFixes=%d.%d",
        qPrintable(screen->name()), m_origin.x(), m_origin.y(), screen->devicePixelRatio(), major, minor);
}

StreamScreenCursor::~StreamScreenCursor() {
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop cursor closed ready=%d drawn=%d", m_ready, m_reported);
}

bool StreamScreenCursor::draw(QImage& frame) {
    if (!m_ready || !m_screen || frame.isNull()) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor unavailable ready=%d screen=%d frame=%dx%d",
            m_ready, !m_screen.isNull(), frame.width(), frame.height()); return false;
    }
    if (m_origin.position() != m_screen->geometry().topLeft()) m_origin.setPosition(m_screen->geometry().topLeft());
    Window root = 0, child = 0;
    int rootX = 0, rootY = 0, localX = 0, localY = 0; unsigned mask = 0;
    // False означает другой X11 root, а не ошибку: курсор на другом экране.
    if (!XQueryPointer(m_display, m_origin.winId(), &root, &child, &rootX, &rootY, &localX, &localY, &mask)) return true;
    std::unique_ptr<XFixesCursorImage, decltype(&XFree)> cursor(XFixesGetCursorImage(m_display), &XFree);
    if (!cursor) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: XFixesGetCursorImage failed"); return false; }
    QPoint position(cursor->x - rootX + localX, cursor->y - rootY + localY);
    if (!frame.rect().contains(position) || !cursor->width || !cursor->height) return true;
    QImage sprite(cursor->width, cursor->height, QImage::Format_ARGB32_Premultiplied);
    if (sprite.isNull() || !cursor->pixels) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor image unavailable size=%ux%u", cursor->width, cursor->height); return false;
    }
    // Xlib хранит каждый ARGB32 в unsigned long, который на LP64 занимает 8 байт.
    for (int y = 0; y < sprite.height(); y++) {
        auto* row = reinterpret_cast<QRgb*>(sprite.scanLine(y));
        for (int x = 0; x < sprite.width(); x++) row[x] = QRgb(cursor->pixels[y * sprite.width() + x]);
    }
    frame.setDevicePixelRatio(1); // Позиция, hotspot и bitmap уже в физических пикселях.
    QPainter painter(&frame);
    if (!painter.isActive()) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor painter initialization failed"); return false; }
    painter.drawImage(position - QPoint(cursor->xhot, cursor->yhot), sprite);
    if (!m_reported) {
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop cursor drawn root=%lu position=%d,%d hotspot=%u,%u sprite=%ux%u frame=%dx%d",
            root, position.x(), position.y(), cursor->xhot, cursor->yhot, cursor->width, cursor->height, frame.width(), frame.height());
        m_reported = true;
    }
    return true;
}
