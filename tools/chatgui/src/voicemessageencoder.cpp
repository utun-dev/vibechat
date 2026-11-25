#include "voicemessageencoder.h"

#include "../../lib/debug_config.h"
#include <QDir>

static int s_preset = 1; /* 0=Low, 1=Standard, 2=High */

void VoiceEncoder::setPreset(int preset) { s_preset = qBound(0, preset, 2); }
int VoiceEncoder::preset() { return s_preset; }

void VoiceEncoder::ensureMediaDir(const QString& dir) {
    QDir d;
    if (!d.mkpath(dir)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "VoiceEncoder: cannot create media dir %s", qPrintable(dir));
    }
}
