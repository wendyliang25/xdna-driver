/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * OS File Utilities Implementation
 */

#include "os_file.h"
#include <unistd.h>

#ifndef WIN32
#include <fcntl.h>
#endif

/* Duplicate file descriptor with close-on-exec flag */
int
os_dupfd_cloexec(int fd)
{
    if (fd < 0)
        return -1;

#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    int new_fd = dup(fd);
    if (new_fd >= 0) {
        fcntl(new_fd, F_SETFD, FD_CLOEXEC);
    }
    return new_fd;
#endif
}

