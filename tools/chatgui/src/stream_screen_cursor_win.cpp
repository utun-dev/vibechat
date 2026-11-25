#include "stream_screen_cursor.h"
#include "debug_config.h"
#include <QPainter>
#include <QScopeGuard>
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#include <cstring>

StreamScreenCursor::StreamScreenCursor(QScreen* screen) {
    if (!screen) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: Windows screen unavailable"); return; }
    m_screenName = screen->name();
    auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    if (!native || !(m_monitor = native->handle())) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: native monitor unavailable screen=%s", qPrintable(m_screenName)); return;
    }
    m_ready = true;
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop cursor opened screen=%s scale=%.3f backend=Win32",
        qPrintable(m_screenName), screen->devicePixelRatio());
}

StreamScreenCursor::~StreamScreenCursor() {
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop cursor closed ready=%d drawn=%d backend=Win32", m_ready, m_reported);
}

/* DrawIconEx работает с фоном кадра: это сохраняет и alpha, и AND/XOR системных курсоров. */
bool StreamScreenCursor::draw(QImage& frame) {
    if (!m_ready || !m_monitor || frame.isNull()) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor unavailable ready=%d monitor=%p frame=%dx%d",
            m_ready, m_monitor, frame.width(), frame.height()); return false;
    }
    CURSORINFO cursor{}; cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: GetCursorInfo failed error=%lu", GetLastError()); return false;
    }
    if (!(cursor.flags & CURSOR_SHOWING)) return true;
    MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(static_cast<HMONITOR>(m_monitor), reinterpret_cast<MONITORINFO*>(&monitor))) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: GetMonitorInfo failed screen=%s error=%lu",
            qPrintable(m_screenName), GetLastError()); return false;
    }
    const QSize monitorSize(monitor.rcMonitor.right - monitor.rcMonitor.left,
        monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    if (frame.size() != monitorSize) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: captured frame does not match monitor screen=%s frame=%dx%d monitor=%dx%d",
            qPrintable(m_screenName), frame.width(), frame.height(), monitorSize.width(), monitorSize.height());
        return false;
    }
    // RECT использует физические координаты общего виртуального desktop, включая отрицательные origins.
    QPoint origin(monitor.rcMonitor.left, monitor.rcMonitor.top);
    QPoint position(cursor.ptScreenPos.x - origin.x(), cursor.ptScreenPos.y - origin.y());
    ICONINFO icon{};
    if (!GetIconInfo(cursor.hCursor, &icon)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: GetIconInfo failed error=%lu", GetLastError()); return false;
    }
    auto releaseIcon = qScopeGuard([&] {
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
    });
    BITMAP bitmap{};
    if (!GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(bitmap), &bitmap)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: bitmap size unavailable error=%lu", GetLastError()); return false;
    }
    // У монохромного курсора обе маски расположены друг под другом в hbmMask.
    QSize size(bitmap.bmWidth, icon.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2);
    if (size.isEmpty()) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: invalid bitmap size=%dx%d", size.width(), size.height()); return false;
    }
    QPoint topLeft = position - QPoint(icon.xHotspot, icon.yHotspot);
    QRect area = QRect(topLeft, size).intersected(frame.rect());
    if (area.isEmpty()) return true;
    QImage background = frame.copy(area).convertToFormat(QImage::Format_RGB32);
    if (background.isNull()) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: background allocation failed size=%dx%d", area.width(), area.height()); return false;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: CreateCompatibleDC failed error=%lu", GetLastError()); return false; }
    auto releaseDc = qScopeGuard([&] { DeleteDC(dc); });
    BITMAPINFO dib{}; dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = area.width(); dib.bmiHeader.biHeight = -area.height();
    dib.bmiHeader.biPlanes = 1; dib.bmiHeader.biBitCount = 32; dib.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP target = CreateDIBSection(dc, &dib, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!target) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: CreateDIBSection failed error=%lu", GetLastError()); return false; }
    auto releaseTarget = qScopeGuard([&] { DeleteObject(target); });
    HGDIOBJ previous = SelectObject(dc, target);
    if (!previous || previous == HGDI_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: SelectObject failed error=%lu", GetLastError()); return false;
    }
    auto restoreTarget = qScopeGuard([&] { SelectObject(dc, previous); });
    const int stride = area.width() * 4;
    for (int y = 0; y < area.height(); y++) std::memcpy(static_cast<uchar*>(pixels) + y * stride, background.constScanLine(y), stride);
    if (!DrawIconEx(dc, topLeft.x() - area.x(), topLeft.y() - area.y(), cursor.hCursor, size.width(), size.height(), 0, nullptr, DI_NORMAL)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: DrawIconEx failed error=%lu", GetLastError()); return false;
    }
    if (!GdiFlush()) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor: GdiFlush failed"); return false; }
    QImage composed(static_cast<uchar*>(pixels), area.width(), area.height(), stride, QImage::Format_RGB32);
    // GDI может обнулить alpha; фон экрана остаётся непрозрачным.
    for (int y = 0; y < composed.height(); y++) {
        auto* row = reinterpret_cast<QRgb*>(composed.scanLine(y));
        for (int x = 0; x < composed.width(); x++) row[x] |= 0xff000000u;
    }
    frame.setDevicePixelRatio(1);
    QPainter painter(&frame);
    if (!painter.isActive()) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop cursor painter initialization failed"); return false; }
    painter.drawImage(area.topLeft(), composed);
    if (!m_reported) {
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop cursor drawn screen=%s device=%ls origin=%d,%d position=%d,%d hotspot=%lu,%lu sprite=%dx%d frame=%dx%d",
            qPrintable(m_screenName), monitor.szDevice, origin.x(), origin.y(), position.x(), position.y(), icon.xHotspot,
            icon.yHotspot, size.width(), size.height(), frame.width(), frame.height());
        m_reported = true;
    }
    return true;
}
