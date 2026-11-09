/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file fault.c
 * @brief Fault state. See fault.h.
 *
 * Single core, but tasks still preempt each other, so the read-modify-write
 * of the mask sits in a critical section.
 */

#include "fault.h"
#include "acq.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "FAULT";

static portMUX_TYPE      s_mux  = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_mask = FAULT_NONE;

void fault_safe_outputs(void)
{
    acq_pause();
}

void fault_init(void)
{
    portENTER_CRITICAL(&s_mux);
    s_mask = FAULT_NONE;
    portEXIT_CRITICAL(&s_mux);
}

void fault_raise(fault_id_t id)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t before = s_mask;
    s_mask |= (uint32_t)id;
    uint32_t after = s_mask;
    portEXIT_CRITICAL(&s_mux);

    if (after != before) {
        bool class_a = ((uint32_t)id & FAULT_CLASS_A_MASK) != 0;
        ESP_LOGE(TAG, "%s fault raised: 0x%04X (mask 0x%04X)",
                 class_a ? "Class-A" : "Process", (unsigned)id, (unsigned)after);
    }
}

void fault_clear(fault_id_t id)
{
    uint32_t bits = (uint32_t)id & ~(uint32_t)FAULT_CLASS_A_MASK;
    portENTER_CRITICAL(&s_mux);
    uint32_t before = s_mask;
    s_mask &= ~bits;
    uint32_t after = s_mask;
    portEXIT_CRITICAL(&s_mux);

    if (after != before) {
        ESP_LOGI(TAG, "Process fault cleared: 0x%04X (mask 0x%04X)",
                 (unsigned)bits, (unsigned)after);
    }
}

bool fault_active(void)
{
    return (s_mask & FAULT_CLASS_A_MASK) != 0;
}

uint32_t fault_get_mask(void)
{
    return s_mask;
}
