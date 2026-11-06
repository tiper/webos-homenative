/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included ahead of src/home.c: what it asks the file system about goes to fake.c, where
 * made-up paths exist. */
#pragma once
#define _GNU_SOURCE /* first, as in home.c: the headers below would otherwise settle without it (Dl_info) */
#include <sys/stat.h>
#include <unistd.h>
#include <glib.h>
#define g_timeout_add(ms, cb, p) fake_timeout(ms, cb, p)
#define access(p, m) fake_access(p, m)
#define stat(p, b) fake_stat(p, b)
#ifdef __APPLE__
#define st_mtim st_mtimespec
#endif
guint fake_timeout(guint, GSourceFunc, gpointer); int fake_access(const char *, int); int fake_stat(const char *, struct stat *);
