/*
 * nanortc — Internal logging macros
 *
 * Include this header in src/ .c files that need logging.
 * The user-facing types are in nanortc.h; this header provides
 * the NANO_LOG_* convenience macros used inside the library.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_LOG_H_
#define NANO_LOG_H_

#include "nanortc.h" /* nano_log_level_t, nanortc_config.h (NANO_LOG_LEVEL) */

/* ----------------------------------------------------------------
 * Internal emit function (implemented in nano_log.c)
 * ---------------------------------------------------------------- */

/**
 * @brief Emit a log message through the registered callback.
 *
 * Called by the NANO_LOG_* macros. Do not call directly.
 *
 * @param level     Severity level.
 * @param subsystem Component tag (static string).
 * @param message   Log message (static string).
 * @param file      Source file (__FILE__), or NULL.
 * @param line      Source line (__LINE__), or 0.
 * @param func      Function name (__func__), or NULL.
 */
void nano_log_emit(nano_log_level_t level, const char *subsystem,
                   const char *message, const char *file, uint32_t line,
                   const char *func);

/**
 * @brief Install the log callback (called from nano_rtc_init).
 */
void nano_log_init(const nano_log_config_t *cfg);

/**
 * @brief Clear the log callback (called from nano_rtc_destroy).
 */
void nano_log_cleanup(void);

/* ----------------------------------------------------------------
 * Source location (can be compiled out with NANO_LOG_NO_LOC)
 * ---------------------------------------------------------------- */

#ifdef NANO_LOG_NO_LOC
#define NANO_LOG_FILE_ ((const char *)0)
#define NANO_LOG_LINE_ 0u
#define NANO_LOG_FUNC_ ((const char *)0)
#else
#define NANO_LOG_FILE_ __FILE__
#define NANO_LOG_LINE_ ((uint32_t)__LINE__)
#define NANO_LOG_FUNC_ __func__
#endif

/* ----------------------------------------------------------------
 * Public macros — NANO_LOG_ERROR / WARN / INFO / DEBUG / TRACE
 *
 * Usage:
 *   NANO_LOG_INFO("SCTP", "association established");
 *   NANO_LOG_ERROR("DTLS", "handshake failed");
 *
 * Messages above NANO_LOG_LEVEL are removed at compile time.
 * If NANO_LOG_DISABLED is defined, all macros expand to nothing.
 * ---------------------------------------------------------------- */

#ifdef NANO_LOG_DISABLED

#define NANO_LOGE(subsys, msg) ((void)0)
#define NANO_LOGW(subsys, msg) ((void)0)
#define NANO_LOGI(subsys, msg) ((void)0)
#define NANO_LOGD(subsys, msg) ((void)0)
#define NANO_LOGT(subsys, msg) ((void)0)

#else /* !NANO_LOG_DISABLED */

#define NANO_LOG_(lvl, subsys, msg)                                                              \
    nano_log_emit((lvl), (subsys), (msg), NANO_LOG_FILE_, NANO_LOG_LINE_, NANO_LOG_FUNC_)

#if NANO_LOG_LEVEL >= 0 /* NANO_LOG_ERROR */
#define NANO_LOGE(subsys, msg) NANO_LOG_(NANO_LOG_ERROR, subsys, msg)
#else
#define NANO_LOGE(subsys, msg) ((void)0)
#endif

#if NANO_LOG_LEVEL >= 1 /* NANO_LOG_WARN */
#define NANO_LOGW(subsys, msg) NANO_LOG_(NANO_LOG_WARN, subsys, msg)
#else
#define NANO_LOGW(subsys, msg) ((void)0)
#endif

#if NANO_LOG_LEVEL >= 2 /* NANO_LOG_INFO */
#define NANO_LOGI(subsys, msg) NANO_LOG_(NANO_LOG_INFO, subsys, msg)
#else
#define NANO_LOGI(subsys, msg) ((void)0)
#endif

#if NANO_LOG_LEVEL >= 3 /* NANO_LOG_DEBUG */
#define NANO_LOGD(subsys, msg) NANO_LOG_(NANO_LOG_DEBUG, subsys, msg)
#else
#define NANO_LOGD(subsys, msg) ((void)0)
#endif

#if NANO_LOG_LEVEL >= 4 /* NANO_LOG_TRACE */
#define NANO_LOGT(subsys, msg) NANO_LOG_(NANO_LOG_TRACE, subsys, msg)
#else
#define NANO_LOGT(subsys, msg) ((void)0)
#endif

#endif /* NANO_LOG_DISABLED */

#endif /* NANO_LOG_H_ */
