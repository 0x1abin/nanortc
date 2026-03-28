/*
 * nanortc — Internal logging macros
 *
 * Include this header in src/ .c files that need logging.
 * The user-facing types are in nanortc.h; this header provides
 * the NANORTC_LOG_* convenience macros used inside the library.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_LOG_H_
#define NANORTC_LOG_H_

#include "nanortc.h" /* nanortc_log_level_t, nanortc_config.h (NANORTC_LOG_LEVEL) */

/* ----------------------------------------------------------------
 * Internal emit function (implemented in nano_log.c)
 * ---------------------------------------------------------------- */

/**
 * @brief Emit a log message through the registered callback.
 *
 * Called by the NANORTC_LOG_* macros. Do not call directly.
 *
 * @param level     Severity level.
 * @param subsystem Component tag (static string).
 * @param message   Log message (static string).
 * @param file      Source file (__FILE__), or NULL.
 * @param line      Source line (__LINE__), or 0.
 * @param func      Function name (__func__), or NULL.
 */
void nano_log_emit(nanortc_log_level_t level, const char *subsystem, const char *message,
                   const char *file, uint32_t line, const char *func);

/**
 * @brief Install the log callback (called from nanortc_init).
 */
void nano_log_init(const nanortc_log_config_t *cfg);

/**
 * @brief Clear the log callback (called from nanortc_destroy).
 */
void nano_log_cleanup(void);

/* ----------------------------------------------------------------
 * Source location (can be compiled out with NANORTC_LOG_NO_LOC)
 * ---------------------------------------------------------------- */

#ifdef NANORTC_LOG_NO_LOC
#define NANORTC_LOG_FILE_ ((const char *)0)
#define NANORTC_LOG_LINE_ 0u
#define NANORTC_LOG_FUNC_ ((const char *)0)
#else
#define NANORTC_LOG_FILE_ __FILE__
#define NANORTC_LOG_LINE_ ((uint32_t)__LINE__)
#define NANORTC_LOG_FUNC_ __func__
#endif

/* ----------------------------------------------------------------
 * Public macros — NANORTC_LOG_ERROR / WARN / INFO / DEBUG / TRACE
 *
 * Usage:
 *   NANORTC_LOG_INFO("SCTP", "association established");
 *   NANORTC_LOG_ERROR("DTLS", "handshake failed");
 *
 * Messages above NANORTC_LOG_LEVEL are removed at compile time.
 * If NANORTC_LOG_DISABLED is defined, all macros expand to nothing.
 * ---------------------------------------------------------------- */

#ifdef NANORTC_LOG_DISABLED

#define NANORTC_LOGE(subsys, msg) ((void)0)
#define NANORTC_LOGW(subsys, msg) ((void)0)
#define NANORTC_LOGI(subsys, msg) ((void)0)
#define NANORTC_LOGD(subsys, msg) ((void)0)
#define NANORTC_LOGT(subsys, msg) ((void)0)

#else /* !NANORTC_LOG_DISABLED */

#define NANORTC_LOG_(lvl, subsys, msg) \
    nano_log_emit((lvl), (subsys), (msg), NANORTC_LOG_FILE_, NANORTC_LOG_LINE_, NANORTC_LOG_FUNC_)

#if NANORTC_LOG_LEVEL >= 0 /* NANORTC_LOG_ERROR */
#define NANORTC_LOGE(subsys, msg) NANORTC_LOG_(NANORTC_LOG_ERROR, subsys, msg)
#else
#define NANORTC_LOGE(subsys, msg) ((void)0)
#endif

#if NANORTC_LOG_LEVEL >= 1 /* NANORTC_LOG_WARN */
#define NANORTC_LOGW(subsys, msg) NANORTC_LOG_(NANORTC_LOG_WARN, subsys, msg)
#else
#define NANORTC_LOGW(subsys, msg) ((void)0)
#endif

#if NANORTC_LOG_LEVEL >= 2 /* NANORTC_LOG_INFO */
#define NANORTC_LOGI(subsys, msg) NANORTC_LOG_(NANORTC_LOG_INFO, subsys, msg)
#else
#define NANORTC_LOGI(subsys, msg) ((void)0)
#endif

#if NANORTC_LOG_LEVEL >= 3 /* NANORTC_LOG_DEBUG */
#define NANORTC_LOGD(subsys, msg) NANORTC_LOG_(NANORTC_LOG_DEBUG, subsys, msg)
#else
#define NANORTC_LOGD(subsys, msg) ((void)0)
#endif

#if NANORTC_LOG_LEVEL >= 4 /* NANORTC_LOG_TRACE */
#define NANORTC_LOGT(subsys, msg) NANORTC_LOG_(NANORTC_LOG_TRACE, subsys, msg)
#else
#define NANORTC_LOGT(subsys, msg) ((void)0)
#endif

#endif /* NANORTC_LOG_DISABLED */

#endif /* NANORTC_LOG_H_ */
