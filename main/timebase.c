/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file timebase.c
 * @brief SNTP, optional DS323x holdover, 1 Hz rate measurement. See timebase.h.
 *
 * Rate loop: at each reference edge the ADC position is interpolated
 *   pos = n + (t_edge - t_drdy) * fs_nominal
 * where n and t_drdy are the latest DRDY count and time. pos is a phase
 * measurement against reference second x. The rate is the slope of a
 * least-squares line through (x, pos) over the last RATE_WIN edges, not
 * an average of second-to-second differences: with white timing jitter
 * sigma_pos per point, differencing gives sigma_f ~ alpha * sigma_pos
 * (1.4 ppm at 10 us jitter, alpha 0.1), the fit gives
 *   sigma_f = sigma_pos * sqrt(12 / N^3)
 * which is about 0.01 ppm at N = 300 from the same edges. The same fit of
 * the edge times themselves gives the ESP crystal error.
 *
 * Edges at the wrong spacing (not a whole number of seconds) are ignored.
 * Edges at the right spacing but far off the fit are outliers; a run of
 * them means the rate itself moved, and the fit restarts. A restart of
 * acquisition (epoch change) or a reference gap over RATE_MAX_SKIP seconds
 * restarts it too.
 *
 * SNTP runs in immediate mode: each hourly sync may step the system clock
 * by a few ms, so log times are exact. Sample timestamps do not follow the
 * step. With GPS PPS they come from the sample count (see timebase.h);
 * without it, from an offset slewed toward the system clock at TB_SLEW_PPM.
 * IDF's own smooth mode is not used: it slews at 1/64 (15 625 ppm), which
 * would squeeze a 5 ms correction into 0.3 s of samples.
 */

#include "timebase.h"
#include "acq.h"
#include "ds323x.h"
#include "fault.h"
#include "io_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "TIME";

#define REF_LOST_MS        3000
#define REF_FIRST_RTC_US   (5 * 1000000LL)     /* no SQW edge this long after start: warn */
#define REF_FIRST_GPS_US   (120 * 1000000LL)   /* a cold GPS may take a minute or two to fix */
#define REF_MIN_GAP_US     900000              /* edges closer than this are glitches, dropped in the ISR */
#define RTC_POLL_S         60

#define RATE_WIN           300     /* reference edges in the fit window */
#define RATE_MIN_PTS       10      /* edges before the rate is reported */
#define RATE_MAX_SKIP      30      /* missed edges bridged without a restart */
#define RATE_SPACING_US    20000   /* edge spacing tolerance against whole seconds */
#define RATE_OUTLIER_SMP   0.5     /* off the fit by this many samples ... */
#define RATE_OUTLIER_US    200.0   /* ... or this much time, whichever is larger */
#define RATE_RESET_AFTER   5       /* consecutive outliers: the rate moved, restart */
#define RATE_SPACING_RESET 5       /* misspaced edges in a row: the reference phase moved, restart */
#define RATE_RESTART_LOG_US (60 * 1000000LL)   /* at most one restart warning a minute */

/* RTC writes. Each write restarts the DS3231 countdown, and with it the SQW
 * phase, so the RTC is written only when it needs it: the first SNTP sync of
 * a boot, after OSF, and once a day to keep its phase near UTC (2 ppm is
 * 0.17 s a day). */
#define RTC_REWRITE_US     (24 * 3600 * 1000000LL)
#define RTC_WRITE_LEAD_US  400     /* address, pointer and seconds bytes at 100 kHz */

/* SNTP sample checks. Once synced, the system clock can only have drifted by
 * the ESP crystal error since the last good sample. A bigger correction is
 * far more likely a reply that sat in an ISP queue, so it waits for a second
 * sample that agrees. */
#define NTP_MIN_EPOCH       ((time_t)(SEISMO_BUILD_EPOCH - 86400))
#define NTP_MAX_EPOCH       ((time_t)4294967295LL)   /* end of lwIP's NTP seconds, 2106 */
#define NTP_HOLD_US         200000  /* corrections above this (plus drift) are held ... */
#define NTP_HOLD_US_PER_S   50      /* ... drift allowance: 50 ppm since the last good sample */
#define NTP_AGREE_US        100000  /* a held sample is confirmed by one within this */
#define NTP_CAND_MAX_AGE_US (15 * 60 * 1000000LL)
#define NTP_REPOLL_S        30
#define NTP_FRESH_US        (2 * 3600 * 1000000LL)   /* SNTP this recent may rename PPS seconds */

static TaskHandle_t s_task;
static volatile int64_t s_edge_t;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static tb_status_t s_st = { .time_src = "none", .ntp_age_s = -1, .ref_age_ms = -1, .rtc_model = "none" };
static volatile tb_tq_t s_src_q = TB_TQ_NONE;   /* quality of the system-clock path */
static volatile bool s_rtc_write_req;   /* an SNTP sync happened: consider an RTC write */
static volatile bool s_rtc_write_due;   /* the second-boundary timer fired: write now */
static esp_timer_handle_t s_rtc_timer;
static int64_t s_rtc_last_write;        /* esp time of the last write, 0 = none this boot. Task only. */
static int64_t s_rtc_write_t;           /* write that moved the SQW phase, 0 = handled. Task only. */

/* Boot seed at the RTC seconds rollover. Task only, except s_seed_tick. */
#define SEED_POLL_US       2000
/* A rollover seen across a longer gap between two polls is not used: the
 * edge could be anywhere in it. The task runs at priority 10, and WiFi
 * start-up (PHY calibration at 23) can hold it off for most of a second;
 * a midpoint taken across that was 0.08 to 0.5 s off on the bench. */
#define SEED_MAX_GAP_US    6000
#define SEED_TIMEOUT_US    3500000      /* room for two rollovers skipped */
static esp_timer_handle_t s_seed_timer;
static volatile bool s_seed_tick;
static bool    s_seed_active;
static time_t  s_seed_utc;              /* RTC time read at the start */
static time_t  s_seed_base;             /* second the seconds register now shows */
static uint8_t s_seed_sec;              /* that register value */
static uint8_t s_seed_skipped;          /* rollovers passed over for a long gap */
static int64_t s_seed_t0, s_seed_t_last;

/* Sample clock without pps: utc = esp + off(esp), with
 *   off(t) = s_off_us + s_off_rate * (t - s_off_t)
 * a continuous piecewise-linear offset. Each tracking pass re-bases it at
 * "now" and picks a new rate toward the system clock offset, limited to
 * TB_SLEW_PPM, so corrections are slewed in, never stepped. Guarded by s_mux. */
#define OFF_HORIZON_US 4000000.0      /* aim to close the gap over this long, at most */
static int64_t s_off_us;
static double  s_off_rate;            /* us per us */
static bool    s_off_valid;
static int64_t s_off_t;
/* The segment before the last re-base. Samples are stamped up to a filter
 * delay in the past (0.1 s at 100 SPS, more at low rates); a time before the
 * re-base must use the line that was in force then, or every re-base would
 * leave a (rate change) x (delay) gap between neighbouring samples. */
static int64_t s_offp_us, s_offp_t;
static double  s_offp_rate;
/* While the pps model holds, the sample clock is steered to UTC from the PPS
 * edges instead of the system clock, so the handover at the end of holdover
 * starts from PPS time and slews toward SNTP: no jump. Guarded by s_mux. */
static int64_t s_pps_off_us;          /* UTC - esp at the last good edge ... */
static int64_t s_pps_off_t;           /* ... whose esp time was this ... */
static double  s_pps_ff;              /* ... drifting at -esp_ppm (us per us) */
static bool    s_pps_off_valid;

static inline int64_t off_at(int64_t t)
{
    if (t < s_off_t) return s_offp_us + (int64_t)llround(s_offp_rate * (double)(t - s_offp_t));
    return s_off_us + (int64_t)llround(s_off_rate * (double)(t - s_off_t));
}

/* pps model, published by on_edge, read by timebase_sample_utc_us. Guarded by s_mux. */
static struct {
    bool     valid;
    uint32_t epoch;
    double   pos0, fs, a, b;      /* pos(x) = pos0 + fs x + a + b x */
    int64_t  utc_x0_s;            /* UTC second at x = 0 */
    int64_t  last_edge_t;         /* esp time of the last accepted edge */
} s_model;
static volatile int64_t s_ntp_last_us;
static bool s_sntp_started;
static esp_timer_handle_t s_repoll_timer;
/* Held SNTP sample, tcpip thread only */
static bool    s_cand_valid;
static int64_t s_cand_delta_us, s_cand_t;
static bool s_use_rtc;

/* A reference edge at esp_timer time t. ISR context, from either path. */
static void IRAM_ATTR ref_edge(int64_t t)
{
    /* Glitch gate. A floating or miswired input (the 32 kHz pin of an
     * unconfigured DS3231) would otherwise wake the task tens of thousands
     * of times a second on a single core, next to acquisition. A glitch
     * just before a real edge can hide it; the fit bridges a missed edge. */
    const int64_t last = s_edge_t;
    if (last && t - last < REF_MIN_GAP_US) return;
    s_edge_t = t;
    BaseType_t woken = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void IRAM_ATTR ref_isr(void *arg)
{
    (void)arg;
    ref_edge(esp_timer_get_time());
}

const char *timebase_ref_name(tb_ref_t r)
{
    switch (r) {
        case TB_REF_GPS: return "gps";
        case TB_REF_RTC: return "rtc";
        default:         return "none";
    }
}

bool timebase_time_valid(void)
{
    return s_st.time_valid;
}

const char *timebase_tq_name(tb_tq_t q)
{
    switch (q) {
        case TB_TQ_PPS: return "pps";
        case TB_TQ_NTP: return "ntp";
        case TB_TQ_RTC: return "rtc";
        default:        return "none";
    }
}

/* System clock minus esp_timer, right now. */
static int64_t sys_offset_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t esp = esp_timer_get_time();
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - esp;
}

static inline bool model_usable(uint32_t epoch, int64_t now);

/* Move the sample clock toward UTC (PPS when it holds, else the system
 * clock), slew-limited. Called from the timebase task at least once a second. */
static void offset_track(void)
{
    if (!s_st.time_valid) return;
    int64_t sys = sys_offset_us();
    int64_t now = esp_timer_get_time();
    uint64_t n;
    int64_t t_d;
    uint32_t epoch;
    acq_get_position(&n, &t_d, &epoch);
    bool stepped = false;
    portENTER_CRITICAL(&s_mux);
    /* With PPS the target drifts with the ESP crystal error, which the rate
     * fit measures; feed that drift forward so the ramp tracks it without lag
     * and the holdover extrapolates it. The system clock runs on the ESP timer,
     * so its offset is constant between SNTP syncs: no feed-forward. */
    bool pps = s_pps_off_valid && model_usable(epoch, now);
    double ff = pps ? s_pps_ff : 0.0;
    int64_t target = pps ? s_pps_off_us + (int64_t)llround(s_pps_ff * (double)(now - s_pps_off_t)) : sys;
    int64_t cur = s_off_valid ? off_at(now) : target;
    int64_t d = target - cur;
    bool jump = !s_off_valid || llabs(d) > TB_STEP_US;
    if (jump) {
        stepped = s_off_valid;
        cur = target;
        d = 0;
        s_off_valid = true;
    }
    /* Re-base at now (continuous), then ramp toward the target: the target's
     * own drift plus a slew-limited correction. */
    double rate = (double)d / OFF_HORIZON_US;
    const double lim = TB_SLEW_PPM * 1e-6;
    if (rate > lim) rate = lim;
    if (rate < -lim) rate = -lim;
    rate += ff;
    if (jump) {                         /* a step: nothing before it to stay continuous with */
        s_offp_us = cur;
        s_offp_t = now;
        s_offp_rate = 0;
    } else {
        s_offp_us = s_off_us;
        s_offp_t = s_off_t;
        s_offp_rate = s_off_rate;
    }
    s_off_us = cur;
    s_off_t = now;
    s_off_rate = rate;
    s_st.slew_ms = (double)d / 1000.0;
    portEXIT_CRITICAL(&s_mux);
    if (stepped) ESP_LOGW(TAG, "System clock moved by more than %d ms: sample clock stepped", TB_STEP_US / 1000);
}

int64_t timebase_esp_to_utc_us(int64_t t_esp)
{
    if (!s_st.time_valid) return 0;
    portENTER_CRITICAL(&s_mux);
    bool ok = s_off_valid;
    int64_t off = ok ? off_at(t_esp) : 0;
    portEXIT_CRITICAL(&s_mux);
    if (!ok) off = sys_offset_us();        /* time just became valid; the task catches up */
    return t_esp + off;
}

/* The pps model applies to this epoch and is fresh enough. Caller holds s_mux. */
static inline bool model_usable(uint32_t epoch, int64_t now)
{
    return s_model.valid && s_model.epoch == epoch &&
           now - s_model.last_edge_t < (int64_t)TB_PPS_HOLDOVER_S * 1000000;
}

int64_t timebase_sample_utc_us(uint32_t epoch, double pos, int64_t t_esp_us, tb_tq_t *tq)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    bool use = model_usable(epoch, now);
    double pos0 = s_model.pos0, fs = s_model.fs, a = s_model.a, b = s_model.b;
    int64_t x0 = s_model.utc_x0_s;
    portEXIT_CRITICAL(&s_mux);
    if (use) {
        double x = (pos - pos0 - a) / (fs + b);    /* reference seconds since x = 0 */
        if (tq) *tq = TB_TQ_PPS;
        return x0 * 1000000 + (int64_t)llround(x * 1e6);
    }
    int64_t utc = timebase_esp_to_utc_us(t_esp_us);
    if (tq) *tq = utc ? s_src_q : TB_TQ_NONE;
    return utc;
}

void timebase_get_status(tb_status_t *out)
{
    int64_t now = esp_timer_get_time();
    uint32_t epoch = 0;
    uint64_t n;
    int64_t t_d;
    acq_get_position(&n, &t_d, &epoch);
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    out->quality = model_usable(epoch, now) ? TB_TQ_PPS : s_st.time_valid ? s_src_q : TB_TQ_NONE;
    portEXIT_CRITICAL(&s_mux);
    int64_t ntp = s_ntp_last_us;
    out->ntp_age_s = ntp ? (now - ntp) / 1000000 : -1;
    int64_t e = s_edge_t;
    out->ref_age_ms = (out->ref_seen && e) ? (now - e) / 1000 : -1;
}

/* ── SNTP ───────────────────────────────────────────────────────────────── */

/* An SNTP sample was applied to the system clock. tcpip thread. */
static void sntp_accepted(int64_t mono, int64_t delta_us, bool confirmed)
{
    portENTER_CRITICAL(&s_mux);
    bool was_valid = s_st.time_valid;
    uint32_t n = ++s_st.ntp_syncs;
    s_st.time_valid = true;
    s_st.time_src = "ntp";
    s_src_q = TB_TQ_NTP;
    portEXIT_CRITICAL(&s_mux);
    s_ntp_last_us = mono;
    fault_clear(FAULT_NO_TIME);
    s_rtc_write_req = true;
    if (s_task) xTaskNotifyGive(s_task);
    if (was_valid) {
        ESP_LOGI(TAG, "SNTP sync #%lu: %+lld ms%s", (unsigned long)n, (long long)(delta_us / 1000),
                 confirmed ? " (confirmed)" : "");
    } else {
        ESP_LOGI(TAG, "SNTP sync #%lu (clock was unset)", (unsigned long)n);
    }
}

static void sntp_reject_count(void)
{
    portENTER_CRITICAL(&s_mux);
    s_st.ntp_rejected++;
    portEXIT_CRITICAL(&s_mux);
}

/* lwIP calls this with every server reply. It replaces IDF's weak version,
 * which sets the clock unconditionally, so the IDF sync notification (and
 * any esp_netif sync_cb) no longer runs: sntp_accepted() does that job.
 *
 * The first sample after boot only has to be plausible: an RTC seed can be
 * a second off. After that, a sample that moves the clock by more than the
 * ESP crystal could have drifted is held until a second one agrees. One
 * reply stuck in an ISP queue (0.8 s seen on a congested uplink) must not
 * step the sample clock or rename PPS seconds. */
void sntp_sync_time(struct timeval *tv)
{
    const int64_t mono = esp_timer_get_time();
    if (tv->tv_sec < NTP_MIN_EPOCH || tv->tv_sec > NTP_MAX_EPOCH) {
        sntp_reject_count();
        ESP_LOGW(TAG, "SNTP sample %lld outside the plausible window: rejected", (long long)tv->tv_sec);
        return;
    }

    struct timeval now;
    gettimeofday(&now, NULL);
    const int64_t delta = ((int64_t)tv->tv_sec - (int64_t)now.tv_sec) * 1000000 +
                          ((int64_t)tv->tv_usec - (int64_t)now.tv_usec);

    bool confirmed = false;
    const int64_t last = s_ntp_last_us;
    if (last) {
        const int64_t limit = NTP_HOLD_US + (mono - last) / 1000000 * NTP_HOLD_US_PER_S;
        if (llabs(delta) > limit) {
            if (s_cand_valid && mono - s_cand_t < NTP_CAND_MAX_AGE_US &&
                llabs(delta - s_cand_delta_us) <= NTP_AGREE_US) {
                confirmed = true;
            } else {
                s_cand_valid = true;
                s_cand_delta_us = delta;
                s_cand_t = mono;
                sntp_reject_count();
                ESP_LOGW(TAG, "SNTP sample %+lld ms off a synced clock: held, re-polling in %d s",
                         (long long)(delta / 1000), NTP_REPOLL_S);
                if (s_repoll_timer) {
                    esp_timer_stop(s_repoll_timer);
                    esp_timer_start_once(s_repoll_timer, (uint64_t)NTP_REPOLL_S * 1000000);
                }
                return;
            }
        }
    }

    s_cand_valid = false;
    settimeofday(tv, NULL);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    sntp_accepted(mono, delta, confirmed);
}

static void repoll_cb(void *arg)
{
    (void)arg;
    esp_sntp_restart();
}

/* Server slots. Slot 0 belongs to DHCP option 42: a LAN server (usually the
 * router) is a far better sample source than the pool across a jittery
 * uplink. The Kconfig servers sit behind it as fallback. lwIP clears every
 * slot after the DHCP ones when a lease names a server, so the fallback
 * names are set again on each IP-up. An empty slot 0 (no option 42) costs
 * one retry timeout before lwIP moves on. */
#define NTP_SLOT_DHCP      0
#define NTP_SLOT_FALLBACK  1

static const char *const s_ntp_fallback[] = {
    CONFIG_SEISMO_NTP_SERVER_1, CONFIG_SEISMO_NTP_SERVER_2,
    CONFIG_SEISMO_NTP_SERVER_3, CONFIG_SEISMO_NTP_SERVER_4,
};
_Static_assert(NTP_SLOT_FALLBACK + sizeof(s_ntp_fallback) / sizeof(s_ntp_fallback[0])
               <= CONFIG_LWIP_SNTP_MAX_SERVERS, "raise CONFIG_LWIP_SNTP_MAX_SERVERS");

void timebase_net_init(void)
{
#if CONFIG_LWIP_DHCP_GET_NTP_SRV
    esp_sntp_servermode_dhcp(true);
#endif
}

static void set_fallback_servers(void)
{
    for (size_t i = 0; i < sizeof(s_ntp_fallback) / sizeof(s_ntp_fallback[0]); i++) {
        esp_sntp_setservername((u8_t)(NTP_SLOT_FALLBACK + i), s_ntp_fallback[i]);
    }
}

static void log_ntp_servers(const char *what)
{
    char dhcp[IPADDR_STRLEN_MAX] = "none";
    const ip_addr_t *a = esp_sntp_getserver(NTP_SLOT_DHCP);
    if (a && !ip_addr_isany(a)) ipaddr_ntoa_r(a, dhcp, sizeof(dhcp));
    ESP_LOGI(TAG, "%s: DHCP %s, then %s, %s, %s, %s", what, dhcp,
             s_ntp_fallback[0], s_ntp_fallback[1], s_ntp_fallback[2], s_ntp_fallback[3]);
}

void timebase_start_sntp(void)
{
    if (s_sntp_started) {
        /* Back from an outage: do not wait up to an hour for the next poll.
         * A new lease may have cleared the fallback slots. */
        set_fallback_servers();
        log_ntp_servers("Network address up, SNTP poll now");
        esp_sntp_restart();
        return;
    }
    s_sntp_started = true;
    if (!s_repoll_timer) {
        const esp_timer_create_args_t rp = { .callback = repoll_cb, .name = "ntp_repoll" };
        if (esp_timer_create(&rp, &s_repoll_timer) != ESP_OK) s_repoll_timer = NULL;
    }

    /* No servers through esp_netif: it would write them from slot 0 and
     * overwrite the DHCP server. Set them by slot, then start. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SEISMO_NTP_SERVER_1);
    cfg.num_of_servers = 0;
#if CONFIG_LWIP_DHCP_GET_NTP_SRV
    cfg.server_from_dhcp = true;
#endif
    cfg.sync_cb = NULL;            /* sntp_sync_time() above decides and reports */
    cfg.wait_for_sync = false;     /* nothing waits on the semaphore */
    cfg.start = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        set_fallback_servers();
        err = esp_netif_sntp_start();
    }
    if (err != ESP_OK) {
        s_sntp_started = false;
        ESP_LOGE(TAG, "SNTP init: %s", esp_err_to_name(err));
        return;
    }
    log_ntp_servers("SNTP started");
}

/* ── RTC ────────────────────────────────────────────────────────────────── */

/* Seed from the RTC at its seconds rollover. The registers only hold whole
 * seconds, so a plain seed lands 0 to 1 s behind. That makes rtc-quality
 * samples late and, with GPS and no network, can name every PPS second one
 * early. Instead, poll the seconds register every SEED_POLL_US (one byte,
 * about 0.4 ms at 100 kHz) until it changes, and set the clock from that
 * edge: within half the gap between the two reads, at most 3 ms. A change
 * seen across a longer gap is skipped, and the next rollover is used.
 * time_valid waits for it, at most SEED_TIMEOUT_US. */
static void seed_apply(int64_t utc_us, bool aligned, int64_t unc_us)
{
    if (s_st.time_valid) return;            /* SNTP got there first */
    struct timeval tv = { .tv_sec = (time_t)(utc_us / 1000000), .tv_usec = (suseconds_t)(utc_us % 1000000) };
    settimeofday(&tv, NULL);
    portENTER_CRITICAL(&s_mux);
    s_st.time_valid = true;
    s_st.time_src = "rtc";
    s_src_q = TB_TQ_RTC;
    portEXIT_CRITICAL(&s_mux);
    fault_clear(FAULT_NO_TIME);
    if (aligned) {
        ESP_LOGI(TAG, "System clock seeded from the RTC at its seconds rollover, +-%.1f ms "
                 "(%u skipped, holdover until SNTP)", (double)unc_us / 1000.0, s_seed_skipped);
    } else {
        ESP_LOGW(TAG, "RTC rollover not seen cleanly (%u skipped): seeded to the whole second (up to 1 s late)",
                 s_seed_skipped);
    }
}

static void seed_timer_cb(void *arg)
{
    (void)arg;
    s_seed_tick = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void seed_stop(void)
{
    if (s_seed_timer) esp_timer_stop(s_seed_timer);
    s_seed_active = false;
}

/* At boot: read and check the RTC, then start polling for the rollover. */
static void rtc_seed_begin(void)
{
    time_t utc;
    bool osf = true;
    if (ds323x_read_time(&utc, &osf) != ESP_OK) return;
    const int64_t t_read = esp_timer_get_time();
    s_st.rtc_osf = osf;
    if (osf) {
        fault_raise(FAULT_RTC_LOST_POWER);
        ESP_LOGW(TAG, "RTC oscillator stopped at some point: not seeding the clock");
        return;
    }
    if (utc < NTP_MIN_EPOCH) {
        ESP_LOGW(TAG, "RTC time %lld predates this build: ignored", (long long)utc);
        return;
    }
    if (s_st.time_valid) return;
    s_seed_utc = utc;
    s_seed_base = utc;
    s_seed_sec = (uint8_t)(utc % 60);
    s_seed_skipped = 0;
    s_seed_t0 = t_read;
    s_seed_t_last = t_read;
    if (!s_seed_timer || esp_timer_start_periodic(s_seed_timer, SEED_POLL_US) != ESP_OK) {
        seed_apply((int64_t)utc * 1000000, false, 0);
        return;
    }
    s_seed_active = true;
}

/* One poll, in the task (it owns the bus). */
static void seed_poll(void)
{
    if (s_st.time_valid) { seed_stop(); return; }   /* SNTP first: nothing to do */
    const int64_t t_start = esp_timer_get_time();
    uint8_t s;
    const bool ok = ds323x_online() && ds323x_read_seconds(&s) == ESP_OK;
    const int64_t t_end = esp_timer_get_time();
    if (ok && s != s_seed_sec) {
        /* The register changed between the last read and this one. */
        const int64_t gap = t_start - s_seed_t_last;
        if (gap <= SEED_MAX_GAP_US) {
            const int64_t t_roll = (s_seed_t_last + t_start) / 2;
            seed_stop();
            seed_apply(((int64_t)s_seed_base + 1) * 1000000 + (esp_timer_get_time() - t_roll), true, gap / 2);
            return;
        }
        /* Held off too long to place the edge: wait for the next one.
         * A gap past a whole second shows up as a step of more than one. */
        s_seed_base += (s >= s_seed_sec ? s - s_seed_sec : s + 60 - s_seed_sec);
        s_seed_sec = s;
        s_seed_skipped++;
    }
    if (!ok || t_end - s_seed_t0 > SEED_TIMEOUT_US) {
        seed_stop();
        seed_apply((int64_t)s_seed_utc * 1000000 + (esp_timer_get_time() - s_seed_t0), false, 0);
        return;
    }
    s_seed_t_last = t_end;
}

static void rtc_timer_cb(void *arg)
{
    (void)arg;
    s_rtc_write_due = true;
    if (s_task) xTaskNotifyGive(s_task);
}

/* Arm a one-shot for just before the next second boundary. The task stays
 * free in between; it used to sleep up to a second here. */
static void rtc_schedule_write(void)
{
    if (!s_rtc_timer || esp_timer_is_active(s_rtc_timer)) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t wait = 1000000 - (int64_t)tv.tv_usec - RTC_WRITE_LEAD_US;
    if (wait < 1000) wait += 1000000;
    esp_timer_start_once(s_rtc_timer, (uint64_t)wait);
}

/* Write the second that is about to start, so the seconds byte lands on the
 * boundary and the RTC's sub-second phase matches UTC. */
static void rtc_write_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const time_t sec = (time_t)(((int64_t)tv.tv_sec * 1000000 + tv.tv_usec + 500000) / 1000000);
    esp_err_t err = ds323x_write_time(sec);
    gettimeofday(&tv, NULL);
    const int64_t done = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC write failed: %s", esp_err_to_name(err));
        return;                    /* the next SNTP sync tries again */
    }
    s_rtc_last_write = esp_timer_get_time();
    s_st.rtc_osf = false;
    fault_clear(FAULT_RTC_LOST_POWER);
    /* The write restarted the countdown: the SQW reference phase moved. */
    if (s_st.ref == TB_REF_RTC) s_rtc_write_t = s_rtc_last_write;
    ESP_LOGI(TAG, "RTC set from SNTP, second boundary error %+lld us",
             (long long)(done - (int64_t)sec * 1000000));
}

static void rtc_poll(void)
{
    if (!ds323x_online()) {
        if (ds323x_recover() == ESP_OK && ds323x_configure() == ESP_OK) {
            fault_clear(FAULT_RTC_MISSING);
            ESP_LOGI(TAG, "RTC back online");
        }
    }
    int16_t q;
    if (ds323x_online() && ds323x_read_temp_q(&q) == ESP_OK) s_st.rtc_temp_c = q / 4.0f;
    s_st.rtc_online = ds323x_online();
    s_st.rtc_model = ds323x_model_name();
}

/* ── Rate loop ──────────────────────────────────────────────────────────── */

/* Fit state. Points are stored as residuals against the nominal rate so
 * the sums stay small: y = pos - pos0 - fs * x (samples), e = t - t0 - 1e6 x
 * (us). Slopes of y and e against x are then the rate error in samples per
 * second and the ESP timer error in us per second (= ppm). */
static struct {
    double   x[RATE_WIN], y[RATE_WIN], e[RATE_WIN];
    int      head, n;
    bool     active;
    uint32_t epoch;
    double   fs;            /* nominal rate the residuals are taken against */
    double   pos0;
    int64_t  t0;
    double   x_last;        /* reference second of the last accepted or outlier edge */
    int64_t  t_last;
    double   a, b;          /* y = a + b x from the last solve */
    bool     have_line;
    int      bad_run;
    bool     named;         /* utc_x0_s known */
    int64_t  utc_x0_s;      /* UTC second of the edge at x = 0 */
    int      name_bad;      /* consecutive edges whose SNTP name disagrees */
    int      spacing_bad;   /* consecutive edges off the whole-second grid */
} s_fit;

#define NAME_REANCHOR_AFTER 3   /* edges in a row with another second name: re-anchor */

static void model_invalidate(void)
{
    portENTER_CRITICAL(&s_mux);
    s_model.valid = false;
    portEXIT_CRITICAL(&s_mux);
}

/* Name the second of a GPS PPS edge at reference second x from the system
 * clock (good to well under 0.5 s with SNTP), and keep UTC of x = 0. One
 * disagreeing edge is ignored; a run of them (a leap second, or the first
 * SNTP sync after an RTC seed that was off by seconds) moves the anchor. */
static void name_edge(int64_t t_edge, double x)
{
    if (s_st.ref != TB_REF_GPS || !s_st.time_valid) return;
    int64_t utc_us = t_edge + sys_offset_us();
    int64_t sec = (utc_us + 500000) / 1000000;
    int64_t x0 = sec - (int64_t)llround(x);
    if (!s_fit.named) {
        s_fit.utc_x0_s = x0;
        s_fit.named = true;
        s_fit.name_bad = 0;
        return;
    }
    if (x0 == s_fit.utc_x0_s) { s_fit.name_bad = 0; return; }
    /* Only a recently synced clock may rename seconds. A stale one (network
     * down for hours, ESP crystal drifting) would rename them wrongly; the
     * edge count carries the names until SNTP is back. */
    const int64_t last = s_ntp_last_us;
    if (!last || t_edge - last > NTP_FRESH_US) { s_fit.name_bad = 0; return; }
    if (++s_fit.name_bad >= NAME_REANCHOR_AFTER) {
        ESP_LOGW(TAG, "PPS seconds renamed by %+lld s from the system clock", (long long)(x0 - s_fit.utc_x0_s));
        s_fit.utc_x0_s = x0;
        s_fit.name_bad = 0;
        portENTER_CRITICAL(&s_mux);
        s_st.pps_naming_errors++;
        portEXIT_CRITICAL(&s_mux);
    }
}

static void fit_add(double x, double y, double e)
{
    s_fit.x[s_fit.head] = x;
    s_fit.y[s_fit.head] = y;
    s_fit.e[s_fit.head] = e;
    s_fit.head = (s_fit.head + 1) % RATE_WIN;
    if (s_fit.n < RATE_WIN) s_fit.n++;
}

static void fit_start(double pos, int64_t t, double fs, uint32_t epoch)
{
    memset(&s_fit, 0, sizeof(s_fit));
    s_fit.active = true;
    s_fit.epoch = epoch;
    s_fit.fs = fs;
    s_fit.pos0 = pos;
    s_fit.t0 = t;
    s_fit.t_last = t;
    fit_add(0.0, 0.0, 0.0);
    model_invalidate();
    name_edge(t, 0.0);
}

/* Least-squares slopes of y and e against x over the window. Two passes
 * (means, then centred sums): cheap at 1 Hz and free of cancellation. */
static bool fit_solve(double *b_y, double *b_e, double *sigma_b)
{
    int n = s_fit.n;
    if (n < 3) return false;
    double mx = 0, my = 0, me = 0;
    for (int i = 0; i < n; i++) { mx += s_fit.x[i]; my += s_fit.y[i]; me += s_fit.e[i]; }
    mx /= n; my /= n; me /= n;
    double sxx = 0, sxy = 0, sxe = 0;
    for (int i = 0; i < n; i++) {
        double dx = s_fit.x[i] - mx;
        sxx += dx * dx;
        sxy += dx * (s_fit.y[i] - my);
        sxe += dx * (s_fit.e[i] - me);
    }
    if (sxx <= 0) return false;
    double b = sxy / sxx, a = my - b * mx, ss = 0;
    for (int i = 0; i < n; i++) {
        double r = s_fit.y[i] - a - b * s_fit.x[i];
        ss += r * r;
    }
    s_fit.a = a;
    s_fit.b = b;
    s_fit.have_line = true;
    *b_y = b;
    *b_e = sxe / sxx;
    *sigma_b = sqrt(ss / (n - 2) / sxx);
    return true;
}

static void rate_invalidate(void)
{
    portENTER_CRITICAL(&s_mux);
    s_st.rate_valid = false;
    s_st.rate_points = s_fit.n;
    s_model.valid = false;
    portEXIT_CRITICAL(&s_mux);
}

static void on_edge(int64_t t_edge)
{
    uint64_t n;
    int64_t t_d;
    uint32_t epoch;
    acq_get_position(&n, &t_d, &epoch);
    double fs = acq_fs_in();

    portENTER_CRITICAL(&s_mux);
    s_st.ref_edges++;
    s_st.ref_seen = true;
    portEXIT_CRITICAL(&s_mux);
    fault_clear(FAULT_REF_LOST);

    if (fs <= 0 || t_d == 0) { s_fit.active = false; s_fit.n = 0; rate_invalidate(); return; }
    double pos = (double)n + (double)(t_edge - t_d) * fs / 1e6;

    /* An RTC write restarted the SQW countdown. Edges from before the write
     * still belong to the old phase; the first one after it starts a new fit. */
    if (s_rtc_write_t && t_edge > s_rtc_write_t) {
        s_rtc_write_t = 0;
        if (s_fit.active) ESP_LOGI(TAG, "RTC written: SQW phase moved, rate fit restarted");
        fit_start(pos, t_edge, fs, epoch);
        rate_invalidate();
        return;
    }

    if (!s_fit.active || epoch != s_fit.epoch || fs != s_fit.fs) {
        fit_start(pos, t_edge, fs, epoch);
        rate_invalidate();
        return;
    }

    int64_t dt = t_edge - s_fit.t_last;
    int64_t k = (dt + 500000) / 1000000;               /* reference seconds since the last edge */
    if (k < 1 || llabs(dt - k * 1000000) >= RATE_SPACING_US) {
        /* One misspaced edge is a glitch: keep the last good edge. A run of
         * them means the reference phase moved (an RTC write, a GPS re-lock).
         * Rejected edges never advance t_last, so without a restart every
         * later edge would be rejected too and the fit would stall. */
        if (++s_fit.spacing_bad >= RATE_SPACING_RESET) {
            static int64_t s_last_restart_log;
            if (s_last_restart_log == 0 || t_edge - s_last_restart_log > RATE_RESTART_LOG_US) {
                s_last_restart_log = t_edge;
                ESP_LOGW(TAG, "%d reference edges in a row off the second grid: rate fit restarted",
                         RATE_SPACING_RESET);
            }
            fit_start(pos, t_edge, fs, epoch);
            rate_invalidate();
        } else if (s_fit.spacing_bad == 1) {
            ESP_LOGW(TAG, "Reference edge spacing %lld us: ignored", (long long)dt);
        } else {
            ESP_LOGD(TAG, "Reference edge spacing %lld us: ignored", (long long)dt);
        }
        return;
    }
    s_fit.spacing_bad = 0;
    if (k > RATE_MAX_SKIP) {
        ESP_LOGW(TAG, "Reference gap of %lld s: rate fit restarted", (long long)k);
        fit_start(pos, t_edge, fs, epoch);
        rate_invalidate();
        return;
    }

    double x = s_fit.x_last + (double)k;
    double y = pos - s_fit.pos0 - fs * x;
    double e = (double)(t_edge - s_fit.t0) - 1e6 * x;
    s_fit.x_last = x;
    s_fit.t_last = t_edge;
    name_edge(t_edge, x);           /* the second name only needs the edge, not its timing quality */

    if (s_fit.have_line && s_fit.n >= RATE_MIN_PTS) {
        double r = y - (s_fit.a + s_fit.b * x);
        double lim = fmax(RATE_OUTLIER_SMP, RATE_OUTLIER_US * 1e-6 * fs);
        if (fabs(r) > lim) {
            portENTER_CRITICAL(&s_mux);
            s_st.rate_rejects++;
            portEXIT_CRITICAL(&s_mux);
            if (++s_fit.bad_run >= RATE_RESET_AFTER) {
                ESP_LOGW(TAG, "%d edges in a row off the rate fit: restarted", RATE_RESET_AFTER);
                fit_start(pos, t_edge, fs, epoch);
                rate_invalidate();
            } else {
                ESP_LOGW(TAG, "Reference edge %.3f samples off the rate fit: ignored", r);
            }
            return;
        }
    }
    s_fit.bad_run = 0;
    fit_add(x, y, e);

    double by, be, sb;
    if (!fit_solve(&by, &be, &sb) || s_fit.n < RATE_MIN_PTS) { rate_invalidate(); return; }
    portENTER_CRITICAL(&s_mux);
    s_st.rate_valid = true;
    s_st.fs_measured = fs + by;
    s_st.adc_ppm = by / fs * 1e6;
    s_st.esp_ppm = be;
    s_st.rate_sigma_ppm = sb / fs * 1e6;
    s_st.rate_points = s_fit.n;
    /* Publish the pps model: the fitted line through the edges, named in UTC. */
    s_model.valid = s_st.ref == TB_REF_GPS && s_fit.named;
    s_model.epoch = s_fit.epoch;
    s_model.pos0 = s_fit.pos0;
    s_model.fs = s_fit.fs;
    s_model.a = s_fit.a;
    s_model.b = s_fit.b;
    s_model.utc_x0_s = s_fit.utc_x0_s;
    s_model.last_edge_t = t_edge;
    if (s_model.valid) {                /* this edge is UTC second utc_x0 + x exactly */
        s_pps_off_us = (s_fit.utc_x0_s + (int64_t)llround(x)) * 1000000 - t_edge;
        s_pps_off_t = t_edge;
        s_pps_ff = -be * 1e-6;          /* esp runs fast by be ppm: UTC - esp falls */
        s_pps_off_valid = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void timebase_task(void *arg)
{
    (void)arg;
    int64_t last_rtc_poll = 0;
    int64_t last_edge_seen = 0;
    const int64_t started = esp_timer_get_time();
    bool ref_warned = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        /* First, before the fit math: the boundary timer has a 400 us lead
         * and the seconds byte must not miss the boundary. */
        if (s_rtc_write_due) {
            s_rtc_write_due = false;
            if (ds323x_online()) rtc_write_now();
        }
        if (s_seed_tick) {
            s_seed_tick = false;
            if (s_seed_active) seed_poll();
        }
        int64_t now = esp_timer_get_time();
        offset_track();

        int64_t e = s_edge_t;
        if (s_st.ref != TB_REF_NONE && e && e != last_edge_seen) {
            last_edge_seen = e;
            on_edge(e);
        }
        /* FAULT_REF_LOST covers a reference that stops. One that never
         * starts (a miswired pin, no GPS fix) would otherwise be silent. */
        if (!ref_warned && s_st.ref != TB_REF_NONE && !s_st.ref_seen) {
            const bool rtc = s_st.ref == TB_REF_RTC;
            if (now - started > (rtc ? REF_FIRST_RTC_US : REF_FIRST_GPS_US)) {
                ref_warned = true;
                if (rtc) {
                    ESP_LOGW(TAG, "No SQW edge on GPIO%d %lld s after start: check the INT/SQW wire "
                             "(32K is the pin next to it)", (int)REF_1HZ_GPIO, REF_FIRST_RTC_US / 1000000);
                } else {
                    ESP_LOGW(TAG, "No GPS PPS on GPIO%d %lld s after start: no fix yet, or check the PPS wire",
                             (int)REF_1HZ_GPIO, REF_FIRST_GPS_US / 1000000);
                }
            }
        }
        if (s_st.ref_seen && now - last_edge_seen > (int64_t)REF_LOST_MS * 1000) {
            fault_raise(FAULT_REF_LOST);
            s_st.rate_valid = false;
        }

        if (s_use_rtc) {
            if (s_rtc_write_req && ds323x_online()) {
                s_rtc_write_req = false;
                if (s_rtc_last_write == 0 || s_st.rtc_osf || now - s_rtc_last_write > RTC_REWRITE_US) {
                    rtc_schedule_write();
                }
            }
            if (now - last_rtc_poll > (int64_t)RTC_POLL_S * 1000000) {
                last_rtc_poll = now;
                rtc_poll();
            }
        }
        acq_log_drain();                /* last: it may block on the console */
    }
}

esp_err_t timebase_init(tb_ref_t ref, bool rising, bool use_rtc, i2c_master_bus_handle_t bus)
{
    s_st.ref = ref;
    s_st.ref_rising = rising;
    s_st.rtc_enabled = use_rtc && bus;
    s_use_rtc = s_st.rtc_enabled;
    fault_raise(FAULT_NO_TIME);

    if (s_use_rtc) {
        const esp_timer_create_args_t wr = { .callback = rtc_timer_cb, .name = "rtc_wr" };
        if (esp_timer_create(&wr, &s_rtc_timer) != ESP_OK) s_rtc_timer = NULL;
        const esp_timer_create_args_t sd = { .callback = seed_timer_cb, .name = "rtc_seed" };
        if (esp_timer_create(&sd, &s_seed_timer) != ESP_OK) s_seed_timer = NULL;
        if (ds323x_init(bus) == ESP_OK && ds323x_configure() == ESP_OK) {
            s_st.rtc_online = true;
            s_st.rtc_model = ds323x_model_name();
            rtc_seed_begin();          /* the task finishes it at the seconds rollover */
        } else {
            fault_raise(FAULT_RTC_MISSING);
        }
    }

    if (ref != TB_REF_NONE) {
        /* On the acquisition capture timer when it runs, so PPS and DRDY share
         * one hardware counter; the GPIO interrupt otherwise. */
        esp_err_t cerr = acq_capture_add(REF_1HZ_GPIO, rising, ref_edge);
        if (cerr != ESP_OK) {
            if (cerr != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGE(TAG, "1 Hz reference capture channel: %s, using the GPIO interrupt", esp_err_to_name(cerr));
            }
            const gpio_config_t io = {
                .pin_bit_mask = 1ULL << REF_1HZ_GPIO,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,   /* SQW is open drain; GPS PPS ignores it */
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = rising ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
            };
            gpio_config(&io);
            gpio_isr_handler_add(REF_1HZ_GPIO, ref_isr, NULL);
        }
        ESP_LOGI(TAG, "1 Hz reference: %s, %s edge on GPIO%d, %s", timebase_ref_name(ref),
                 rising ? "rising" : "falling", REF_1HZ_GPIO, cerr == ESP_OK ? "MCPWM capture" : "GPIO interrupt");
    } else {
        ESP_LOGI(TAG, "No 1 Hz reference: sample rate is the nominal one");
    }

    if (xTaskCreate(timebase_task, "timebase", 3584, NULL, PRIO_TIMEBASE, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
