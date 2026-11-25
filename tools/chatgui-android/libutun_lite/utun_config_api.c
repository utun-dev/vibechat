/*
 * utun_config_api.c — configuration provider implementation
 */
#include "utun_config_api.h"
#include "../../../lib/debug_config.h"

static utun_config_provider_t g_provider;

void utun_config_set_provider(const utun_config_provider_t* provider) {
    g_provider = provider ? *provider : (utun_config_provider_t){0};
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "config provider: callbacks string=%d int64=%d int=%d bool=%d",
                !!g_provider.get_string, !!g_provider.get_int64, !!g_provider.get_int, !!g_provider.get_bool);
}

const char* cfg_get_string(const char* key, const char* def) {
    if (g_provider.get_string)
        return g_provider.get_string(key, def);
    return def;
}

int64_t cfg_get_int64(const char* key, int64_t def) {
    if (g_provider.get_int64)
        return g_provider.get_int64(key, def);
    return def;
}

int cfg_get_int(const char* key, int def) {
    if (g_provider.get_int)
        return g_provider.get_int(key, def);
    return def;
}

int cfg_get_bool(const char* key, int def) {
    if (g_provider.get_bool)
        return g_provider.get_bool(key, def);
    return def;
}
