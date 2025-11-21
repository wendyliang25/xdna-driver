/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file xvdna_debug.c
 * @brief Implementation of debug and logging utilities
 */

#include "xvdna_debug.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Global log level - default to INFO */
static enum xvdna_log_level g_log_level = XVDNA_LOG_INFO;

/* Check if logging is initialized from environment */
static int g_log_initialized = 0;

/**
 * @brief Initialize logging from environment variables
 *
 * Checks XVDNA_LOG_LEVEL environment variable:
 * - "ERROR" or "0" -> XVDNA_LOG_ERROR
 * - "INFO" or "1"  -> XVDNA_LOG_INFO
 * - "DEBUG" or "2" -> XVDNA_LOG_DEBUG
 */
static void
xvdna_log_init(void)
{
    const char *env_level;

    if (g_log_initialized)
        return;

    g_log_initialized = 1;

    env_level = getenv("XVDNA_LOG_LEVEL");
    if (!env_level)
        return;

    if (strcmp(env_level, "ERROR") == 0 || strcmp(env_level, "0") == 0) {
        g_log_level = XVDNA_LOG_ERROR;
    } else if (strcmp(env_level, "INFO") == 0 || strcmp(env_level, "1") == 0) {
        g_log_level = XVDNA_LOG_INFO;
    } else if (strcmp(env_level, "DEBUG") == 0 || strcmp(env_level, "2") == 0) {
        g_log_level = XVDNA_LOG_DEBUG;
    }
}

void
xvdna_set_log_level(enum xvdna_log_level level)
{
    g_log_level = level;
}

enum xvdna_log_level
xvdna_get_log_level(void)
{
    if (!g_log_initialized)
        xvdna_log_init();

    return g_log_level;
}

void
xvdna_log(enum xvdna_log_level level, const char *fmt, ...)
{
    va_list args;
    const char *level_str;
    FILE *output;

    /* Initialize logging on first use */
    if (!g_log_initialized)
        xvdna_log_init();

    /* Check if this message should be displayed */
    if (level > g_log_level)
        return;

    /* Select output stream and level string */
    switch (level) {
    case XVDNA_LOG_ERROR:
        level_str = "ERROR";
        output = stderr;
        break;
    case XVDNA_LOG_INFO:
        level_str = "INFO";
        output = stdout;
        break;
    case XVDNA_LOG_DEBUG:
        level_str = "DEBUG";
        output = stdout;
        break;
    default:
        level_str = "UNKNOWN";
        output = stdout;
        break;
    }

    /* Print message with prefix */
    fprintf(output, "[XVDNA] %s: ", level_str);
    va_start(args, fmt);
    vfprintf(output, fmt, args);
    va_end(args);
    fprintf(output, "\n");
    fflush(output);
}

