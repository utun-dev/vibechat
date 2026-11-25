#include "animtimer.h"
#include "lottieicon.h"
#include "debug_ui.h"

AnimTimer::AnimTimer() {
    m_timer.setInterval(33);
    connect(&m_timer, &QTimer::timeout, this, &AnimTimer::onTimer);
}

AnimTimer *AnimTimer::instance() {
    static AnimTimer s;
    return &s;
}

void AnimTimer::shutdown() {
    instance()->m_timer.stop();
    instance()->m_playback.invalidate();
    instance()->m_icons.clear();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "AnimTimer: shutdown");
}

void AnimTimer::registerIcon(const QString &key, LottieIcon *icon) {
    if (!icon || !icon->isValid()) { GUI_WARN("AnimTimer: invalid icon registration key=%s", qPrintable(key)); return; }
    m_icons.insert(key, icon);
}

void AnimTimer::unregisterIcon(const QString &key) {
    m_icons.remove(key);
}

LottieIcon *AnimTimer::icon(const QString &key) const {
    return m_icons.value(key, nullptr);
}

void AnimTimer::play() {
    if (m_icons.isEmpty()) {
        GUI_WARN("AnimTimer: playback requested without registered icons");
        return;
    }
    GUI_DEBUG("AnimTimer: hover trigger icons=%d running=%d playback_ms=%d", (int)m_icons.size(), m_timer.isActive(), PlaybackMs);
    m_playback.start();
    m_timer.start();
}

void AnimTimer::onTimer() {
    const qint64 elapsed = m_playback.elapsed();
    const bool finishing = elapsed >= PlaybackMs;
    bool finished = true;
    for (auto *icon : m_icons) {
        if (!finishing || icon->currentFrame() != 0) icon->advanceFrame();
        if (icon->currentFrame() != 0) finished = false;
    }
    if (finishing && finished) {
        m_timer.stop();
        m_playback.invalidate();
        GUI_DEBUG("AnimTimer: playback finished elapsed_ms=%lld icons=%d all_frames=0", (long long)elapsed, (int)m_icons.size());
    }
    emit ticked();
}

void AnimTimer::logRegisteredIcons() const {
    for (auto it = m_icons.constBegin(); it != m_icons.constEnd(); ++it) {
        LottieIcon *icon = it.value();
        icon->logWarnings();
        GUI_DEBUG("LottieIcon: path=%s frames=%d size=%dx%d tgsSize=%d valid=%d",
                 qPrintable(it.key()), icon->frameCount(),
                 icon->originalSize().width(), icon->originalSize().height(),
                 icon->tgsSize(), (int)icon->isValid());
    }
}
