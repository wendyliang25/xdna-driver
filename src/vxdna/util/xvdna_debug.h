/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file xvdna_debug.h
 * @brief Debug and logging utilities for XVDNA
 *
 * Provides logging functions with different severity levels and
 * consistent [XVDNA] prefix.
 */

#ifndef XVDNA_DEBUG_H
#define XVDNA_DEBUG_H

#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log level enumeration
 */
enum xvdna_log_level {
    XVDNA_LOG_ERROR = 0,   /**< Error messages (always shown) */
    XVDNA_LOG_INFO  = 1,   /**< Informational messages */
    XVDNA_LOG_DEBUG = 2,   /**< Debug messages (only when enabled) */
};

/**
 * @brief Set global log level
 *
 * Messages with level higher than this will be suppressed.
 * Default is XVDNA_LOG_INFO.
 *
 * @param level Maximum log level to display
 */
void xvdna_set_log_level(enum xvdna_log_level level);

/**
 * @brief Get current log level
 *
 * @return Current log level setting
 */
enum xvdna_log_level xvdna_get_log_level(void);

/**
 * @brief Generic logging function
 *
 * Internal function used by convenience wrappers.
 *
 * @param level Log level
 * @param fmt Printf-style format string
 * @param ... Variable arguments
 */
void xvdna_log(enum xvdna_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief Log an error message
 *
 * Error messages are always displayed regardless of log level.
 * Format: [XVDNA] ERROR: <message>
 *
 * @param fmt Printf-style format string
 * @param ... Variable arguments
 */
static inline void xvdna_err(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline void xvdna_err(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[XVDNA] ERROR: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

/**
 * @brief Log an informational message
 *
 * Displayed when log level >= XVDNA_LOG_INFO.
 * Format: [XVDNA] INFO: <message>
 *
 * @param fmt Printf-style format string
 * @param ... Variable arguments
 */
#define xvdna_info(fmt, ...) \
    xvdna_log(XVDNA_LOG_INFO, fmt, ##__VA_ARGS__)

/**
 * @brief Log a debug message
 *
 * Displayed when log level >= XVDNA_LOG_DEBUG.
 * Format: [XVDNA] DEBUG: <message>
 *
 * @param fmt Printf-style format string
 * @param ... Variable arguments
 */
#define xvdna_dbg(fmt, ...) \
    xvdna_log(XVDNA_LOG_DEBUG, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* XVDNA_DEBUG_H */

