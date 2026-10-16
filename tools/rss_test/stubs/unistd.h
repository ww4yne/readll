/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#define fileno _fileno
#else
#include_next <unistd.h>
#endif
