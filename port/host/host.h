// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* tlggpu-host internals shared by host/main.c and host/host_runtime.c.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "tlggpu_protocol.h"
#include "tlggpu_shared.h"
#include <pthread.h>

/* One line to stdout, prefixed "tlggpu-host: " (one write: the runtime shares the log). */
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* The shared control block (mapped once the handshake is done). */
extern TlgGpuShared *g_shared;

/* Guest view mirror (host_runtime.c). */
void host_runtime_init(mach_port_t guest_entry, uint64_t guest_entry_size);
void host_view_apply(uint32_t op, uint32_t prot, uint64_t address, uint64_t size, uint64_t phys);
int host_view_covered(uint64_t start, uint64_t end);
void host_view_stats(uint64_t *count, uint64_t *bytes, uint64_t *applied, uint64_t *failed);
