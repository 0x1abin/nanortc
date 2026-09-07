/* Instance-scoped synchronous diagnostics. SPDX-License-Identifier: MIT */
#include "nano_log.h"

void nano_log_emit(const nanortc_log_config_t *cfg, nanortc_log_level_t level,
                   const char *subsystem, const char *message, const char *file, uint32_t line,
                   const char *func)
{
#ifndef NANORTC_LOG_DISABLED
    if (!cfg || !cfg->callback || level > cfg->level || (int)level > NANORTC_LOG_LEVEL)
        return;
    nanortc_log_message_t msg;
    msg.level = level;
    msg.subsystem = subsystem;
    msg.message = message;
    msg.file = file;
    msg.line = line;
    msg.function = func;
    cfg->callback(&msg, cfg->user_data);
#else
    (void)cfg;
    (void)level;
    (void)subsystem;
    (void)message;
    (void)file;
    (void)line;
    (void)func;
#endif
}
