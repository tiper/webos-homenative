/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included ahead of src/home.c: what it asks the file system about goes to fake.c, where
 * made-up paths exist. */
#pragma once
#include <sys/stat.h>
#include <unistd.h>
#define access(p, m) fake_access(p, m)
#define stat(p, b) fake_stat(p, b)
#ifdef __APPLE__
#define st_mtim st_mtimespec
#endif
int fake_access(const char *, int); int fake_stat(const char *, struct stat *);
