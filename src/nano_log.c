/*
 * nanortc — Logging subsystem implementation
 *
 * Process-global callback storage. If multiple nanortc_t instances
 * coexist, the last nanortc_init() call determines the active
 * log callback. This is acceptable because logging is a diagnostic
 * facility, not functional state.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_log.h"

#ifndef NANORTC_LOG_DISABLED

/* Process-global log state */
static nanortc_log_fn_t g_log_fn;
static void *g_log_ctx;
static nanortc_log_level_t g_log_level;

void nano_log_init(const nanortc_log_config_t *cfg)
{
    if (!cfg || !cfg->callback) {
        g_log_fn = (nanortc_log_fn_t)0;
        g_log_ctx = (void *)0;
        g_log_level = NANORTC_LOG_ERROR;
        return;
    }

    g_log_fn = cfg->callback;
    g_log_ctx = cfg->user_data;
    g_log_level = cfg->level;

    /* Cap runtime level at compile-time maximum */
    if ((int)g_log_level > NANORTC_LOG_LEVEL) {
        g_log_level = (nanortc_log_level_t)NANORTC_LOG_LEVEL;
    }
}

void nano_log_cleanup(void)
{
    g_log_fn = (nanortc_log_fn_t)0;
    g_log_ctx = (void *)0;
}

void nano_log_emit(nanortc_log_level_t level, const char *subsystem, const char *message,
                   const char *file, uint32_t line, const char *func)
{
    if (!g_log_fn || (int)level > (int)g_log_level) {
        return;
    }

    nanortc_log_message_t msg;
    msg.level = level;
    msg.subsystem = subsystem;
    msg.message = message;
    msg.file = file;
    msg.line = line;
    msg.function = func;

    g_log_fn(&msg, g_log_ctx);
}

#else /* NANORTC_LOG_DISABLED */

void nano_log_init(const nanortc_log_config_t *cfg)
{
    (void)cfg;
}
void nano_log_cleanup(void)
{
}
void nano_log_emit(nanortc_log_level_t level, const char *subsystem, const char *message,
                   const char *file, uint32_t line, const char *func)
{
    (void)level;
    (void)subsystem;
    (void)message;
    (void)file;
    (void)line;
    (void)func;
}

#endif /* NANORTC_LOG_DISABLED */
