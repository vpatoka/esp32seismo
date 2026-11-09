/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file captive_dns.c
 * @brief Provisioning-mode DNS responder. See captive_dns.h.
 *
 * The task blocks in recvfrom(); it costs nothing while idle. Replies are
 * built in place: header, the single question copied back, an optional A
 * record. Any EDNS OPT or other trailing records in the query are dropped.
 */

#include "captive_dns.h"
#include "io_map.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "DNS";

#define DNS_PORT      53
#define DNS_BUF       512
#define DNS_HDR       12
#define QTYPE_A       1
#define QTYPE_ANY     255
#define QCLASS_IN     1

static uint32_t s_ap_ip;      /* network byte order */
static bool     s_started;

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Captive DNS up on UDP %d", DNS_PORT);

    static uint8_t buf[DNS_BUF];
    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &clen);
        if (len < DNS_HDR + 5) continue;

        /* Standard query (QR=0, opcode 0) with exactly one question. */
        if ((buf[2] & 0x80) || (buf[2] & 0x78)) continue;
        if (buf[4] != 0 || buf[5] != 1) continue;

        /* Walk the QNAME labels. Queries never use compression. */
        int p = DNS_HDR;
        while (p < len && buf[p] != 0) {
            if (buf[p] & 0xC0) { p = len; break; }
            p += buf[p] + 1;
        }
        if (p + 5 > len) continue;
        p++;                                   /* root label */
        uint16_t qtype  = (uint16_t)((buf[p] << 8) | buf[p + 1]);
        uint16_t qclass = (uint16_t)((buf[p + 2] << 8) | buf[p + 3]);
        p += 4;                                /* end of question */

        bool answer = (qtype == QTYPE_A || qtype == QTYPE_ANY) && (qclass & 0x7FFF) == QCLASS_IN;

        buf[2] = (uint8_t)(0x84 | (buf[2] & 0x01));   /* QR=1, AA=1, keep RD */
        buf[3] = 0x80;                                /* RA=1, RCODE=NOERROR */
        buf[6] = 0; buf[7] = answer ? 1 : 0;          /* ANCOUNT */
        buf[8] = 0; buf[9] = 0;                       /* NSCOUNT */
        buf[10] = 0; buf[11] = 0;                     /* ARCOUNT: drop EDNS */

        if (answer) {
            if (p + 16 > (int)sizeof(buf)) continue;
            static const uint8_t rr[12] = {
                0xC0, 0x0C,             /* name: pointer to the question */
                0x00, 0x01, 0x00, 0x01, /* TYPE A, CLASS IN */
                0x00, 0x00, 0x00, 0x0A, /* TTL 10 s: forget us soon after setup */
                0x00, 0x04,             /* RDLENGTH */
            };
            memcpy(&buf[p], rr, sizeof(rr));
            p += sizeof(rr);
            memcpy(&buf[p], &s_ap_ip, 4);
            p += 4;
        }
        sendto(sock, buf, p, 0, (struct sockaddr *)&cli, clen);
    }
}

esp_err_t captive_dns_start(const char *ap_ip)
{
    if (s_started) return ESP_OK;
    s_ap_ip = inet_addr(ap_ip);
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, PRIO_CAPTIVE_DNS, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
