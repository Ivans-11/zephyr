/*
 * Copyright (c) 2023 Intel Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#ifndef CONFIG_ACPI
uint32_t x86_cpu_loapics[] = { 0x00, 0x02, 0x04, 0x06 };
#endif
