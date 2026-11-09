/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file captive_dns.h
 * @brief Tiny DNS responder for the provisioning AP.
 *
 * Every A (and ANY) query resolves to the AP address, so a phone's
 * connectivity probe lands on our HTTP server and the OS opens the portal.
 * Every other type (AAAA, HTTPS/SVCB, ...) gets NOERROR with no answers.
 * Answering AAAA with an A record, as a naive hijack does, makes some
 * clients drop the whole reply and the portal never pops up.
 */

#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include "esp_err.h"

/** @brief Start the responder task on UDP 53. ap_ip is dotted quad. */
esp_err_t captive_dns_start(const char *ap_ip);

#endif /* CAPTIVE_DNS_H */
