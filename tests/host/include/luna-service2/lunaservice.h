/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The luna-service2 the launcher uses, faked: see fake.c. */
#pragma once
#include <stdbool.h>
#include <glib.h>
typedef struct LSHandle LSHandle; typedef struct LSMessage LSMessage; typedef unsigned long LSMessageToken;
typedef struct { const char *message; } LSError;
typedef bool (*LSFilterFunc)(LSHandle *, LSMessage *, void *);
static inline void LSErrorInit(LSError *e) { e->message = ""; }
static inline void LSErrorFree(LSError *e) { (void)e; }
bool LSRegister(const char *, LSHandle **, LSError *); bool LSGmainContextAttach(LSHandle *, GMainContext *, LSError *);
bool LSCall(LSHandle *, const char *, const char *, LSFilterFunc, void *, LSMessageToken *, LSError *);
bool LSCallOneReply(LSHandle *, const char *, const char *, LSFilterFunc, void *, LSMessageToken *, LSError *);
bool LSCallCancel(LSHandle *, LSMessageToken, LSError *); const char *LSMessageGetPayload(LSMessage *);
