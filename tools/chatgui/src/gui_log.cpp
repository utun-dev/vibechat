#include "gui_log.h"
#include "debug_config.h"
#include <QByteArray>
#include <QString>
#include <cstring>

static void guiLog(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const debug_level_t level = type == QtDebugMsg ? DEBUG_LEVEL_DEBUG : type == QtInfoMsg ? DEBUG_LEVEL_INFO
        : type == QtWarningMsg ? DEBUG_LEVEL_WARN : DEBUG_LEVEL_ERROR;
    const char* name = context.category ? context.category : "default";
    const int category = !strncmp(name, "qt.multimedia", 13) ? DEBUG_CATEGORY_VIDEO : DEBUG_CATEGORY_GENERAL;
    if (!debug_should_output(level, category)) return;
    const auto text = message.toUtf8();
    debug_output(level, category, context.function, context.file, context.line, "Qt[%s]: %s", name, text.constData());
}

QtMessageHandler installGuiLogHandler() { return qInstallMessageHandler(guiLog); }
