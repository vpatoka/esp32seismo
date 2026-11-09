/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file fault.h
 * @brief Fault taxonomy for the seismograph.
 *
 *   Class A  controller integrity. Latched until reboot, steady red LED.
 *            "This firmware is suspect": a pending OTA image rolls back.
 *   Process  a peripheral or time source is degraded but the controller is
 *            healthy. Web UI and log only; clears when the cause clears.
 *
 * Nothing on this board can hurt anyone, so there are no safety outputs.
 * fault_safe_outputs() pauses acquisition instead: the OTA writer calls it
 * before flash writes stall the cache, and the data gap is then explicit
 * (a restart of the sample counter) rather than a silent glitch.
 */

#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FAULT_NONE            = 0,
    FAULT_ABNORMAL_RESET  = 1u << 0,   /* last reset was a watchdog or a panic       */
    FAULT_INIT_I2C        = 1u << 1,   /* I2C master bus could not be created        */
    FAULT_INIT_TASK       = 1u << 2,   /* a core task failed to start                */
    FAULT_ADC_MISSING     = 1u << 7,   /* ADS131M0x did not answer with a valid ID   */
    FAULT_RTC_MISSING     = 1u << 8,   /* DS323x selected but not answering          */
    FAULT_RTC_LOST_POWER  = 1u << 9,   /* OSF set: RTC time not trustworthy          */
    FAULT_I2C_RECOVERED   = 1u << 10,  /* bus was stuck and needed a reset           */
    FAULT_REF_LOST        = 1u << 11,  /* 1 Hz reference stopped after being seen    */
    FAULT_ADC_CRC         = 1u << 12,  /* frame CRC errors in the last second        */
    FAULT_ADC_OVERRUN     = 1u << 13,  /* a DRDY was served late and a frame was lost */
    FAULT_ADC_CLIP        = 1u << 14,  /* a channel hit full scale in the last second */
    FAULT_NO_TIME         = 1u << 15,  /* no SNTP sync yet: timestamps are relative  */
} fault_id_t;

/* A missing ADC is a process fault on purpose: firmware tested on a bare
 * DevKit must still pass the OTA self-test, or every update rolls back. */
#define FAULT_CLASS_A_MASK  (FAULT_ABNORMAL_RESET | FAULT_INIT_I2C | FAULT_INIT_TASK)

/** @brief Pause acquisition before flash writes or a restart. Any task. */
void fault_safe_outputs(void);

/** @brief Clear all faults. Call first in app_main. */
void fault_init(void);

/** @brief Raise a fault bit. Idempotent; logs on the rising edge. */
void fault_raise(fault_id_t id);

/** @brief Clear a fault bit (process faults only; Class A stays latched). */
void fault_clear(fault_id_t id);

/** @brief True if any Class A fault is latched. */
bool fault_active(void);

/** @brief Raw bitmask for the web UI. */
uint32_t fault_get_mask(void);

#endif /* FAULT_H */
