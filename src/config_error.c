#include "config_error.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static __thread char config_errors[4096];

static __thread int error_line;
static __thread char error_option[1024];

void config_error_clear(void) { config_errors[0] = 0; error_line = 0; error_option[0] = 0; }
void config_error_set_location(int line, const char *option) {
    error_line = line; snprintf(error_option, sizeof(error_option), "%s", option ? option : "");
}
int config_error_line(void) { return error_line; }
const char *config_error_option(void) { return error_option; }
const char *config_error_text(void) { return config_errors; }

void config_error_append(const char *format, ...) {
    const int saved_errno = errno;
    size_t used = strlen(config_errors);
    if (used < sizeof(config_errors) - 2) {
        if (used) config_errors[used++] = '\n';
        va_list args;
        va_start(args, format);
        vsnprintf(config_errors + used, sizeof(config_errors) - used, format, args);
        va_end(args);
    }
    errno = saved_errno;
}
