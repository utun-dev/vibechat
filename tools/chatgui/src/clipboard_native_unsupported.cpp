#include "clipboard_native.h"
std::shared_ptr<ClipboardNative> createClipboardNative(std::shared_ptr<ClipboardFileTree>,
    const std::string&, std::string& error, ClipboardNativeLog logs) {
    error = "Native clipboard file promises require Windows Shell or Linux FUSE";
    logs(true, error); return {};
}
