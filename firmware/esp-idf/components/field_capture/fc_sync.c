/**
 * @file fc_sync.c
 * @brief PC-mediated ECG↔Kompic USB sync protocol handler (Stage 15).
 *
 * Implements the text protocol run by sync.py over the USB-Serial-JTAG CDC
 * endpoint. Called from the CLI task (task_rtc_cli_fn) whenever the device is
 * in sync mode or a SYNC_START line arrives.
 *
 * Protocol summary:
 *   PC → SYNC_START          Kompic → SYNC_READY <fw> <rtc_iso>
 *   PC → PING <i>            Kompic → PONG <i> <t_recv_us>   (8 rounds)
 *   PC → SET_OFFSET <us> <iso>  Kompic → SYNC_OK ...
 *   PC → SET_STEP <us>       Kompic → STEP_OK ...            (optional)
 *   PC → SYNC_END            Kompic → (no reply)
 *
 * Offset persistence: RAM primary, NVS backup (24 h staleness cutoff on boot).
 * PCF RAM byte is only 8 bits — cannot hold int64_t offset.
 */

#define FC_TAG "FC_SYNC"

#include "fc_internal.h"
#include "firmware_version.h"
#include "nvs_cfg.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

// Priority to use during sync burst (above task_field_capture=4, below task_shutdn=6).
#define SYNC_BURST_PRIORITY     5
#define SYNC_NORMAL_PRIORITY    2   // task_rtccli's boot priority

#define PING_COUNT              8
#define STEP_US_DEFAULT      5000   // 200 Hz — agreed target for PPG+BCG raw mode
#define STEP_US_MIN          1000
#define STEP_US_MAX        100000
#define OFFSET_STALENESS_US  (24LL * 3600LL * 1000000LL)   // 24 h in µs

typedef enum {
    SYNC_ST_IDLE = 0,
    SYNC_ST_ACTIVE,
} sync_state_t;

static struct {
    sync_state_t state;
    int          next_ping_i;       // expected next PING index (1-based)
    bool         offset_valid;      // SET_OFFSET received this session or loaded from NVS
    int64_t      offset_us;         // current signed offset (t_pc_us = t_local_us + offset)
    char         ref_rtc_iso[32];   // ref_rtc_iso from last SET_OFFSET
    char         kompic_rtc_iso[32]; // Kompic RTC at the time offset was received
    bool         step_host_set;     // true if SET_STEP was received this session
    int64_t      step_us;           // current row cadence (default STEP_US_DEFAULT)
} s;

// ── Init ────────────────────────────────────────────────────────────────────

void fc_sync_init(void)
{
    memset(&s, 0, sizeof(s));
    s.step_us = STEP_US_DEFAULT;

    // Load persisted offset from NVS if not stale.
    int64_t off_us = 0, upt_us = 0;
    if (nvs_cfg_sys_get_pc_sync(&off_us, &upt_us) == ESP_OK) {
        int64_t age = esp_timer_get_time() - upt_us;
        if (age >= 0 && age < OFFSET_STALENESS_US) {
            s.offset_valid = true;
            s.offset_us    = off_us;
            ESP_LOGI(FC_TAG, "loaded offset=%" PRId64 " µs from NVS (age=%" PRId64 " µs)",
                     off_us, age);
        } else {
            ESP_LOGI(FC_TAG, "NVS offset stale (age=%" PRId64 " µs) -- ignored", age);
        }
    }
}

// ── Public accessors ────────────────────────────────────────────────────────

bool    fc_sync_is_active(void)      { return s.state == SYNC_ST_ACTIVE; }
bool    fc_sync_offset_valid(void)   { return s.offset_valid; }
int64_t fc_sync_get_offset_us(void)  { return s.offset_us; }
int64_t fc_sync_get_step_us(void)    { return s.step_us; }
bool    fc_sync_step_host_set(void)  { return s.step_host_set; }

// ── CSV header writer (called by PPG+BCG mode when opening a CSV) ────────────

void fc_sync_write_csv_headers(FILE *f)
{
    if (!f) return;
    if (s.offset_valid) {
        fprintf(f, "# pc_sync_applied=1\n");
        fprintf(f, "# pc_sync_offset_us=%" PRId64 "\n", s.offset_us);
        fprintf(f, "# pc_sync_ref_iso=%s\n",
                s.ref_rtc_iso[0] ? s.ref_rtc_iso : "unknown");
        fprintf(f, "# pc_sync_kompic_rtc_iso=%s\n",
                s.kompic_rtc_iso[0] ? s.kompic_rtc_iso : "unknown");
    } else {
        fprintf(f, "# pc_sync_applied=0\n");
    }
    fprintf(f, "# pc_sync_step_us=%" PRId64 "\n", s.step_us);
    fprintf(f, "# pc_sync_step_host_set=%d\n", s.step_host_set ? 1 : 0);
}

// ── Internal helpers ─────────────────────────────────────────────────────────

static void sync_enter(void)
{
    s.state        = SYNC_ST_ACTIVE;
    s.next_ping_i  = 1;
    s.step_host_set = false;
    s.step_us      = STEP_US_DEFAULT;
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskPrioritySet(NULL, SYNC_BURST_PRIORITY);
}

static void sync_exit(void)
{
    s.state = SYNC_ST_IDLE;
    vTaskPrioritySet(NULL, SYNC_NORMAL_PRIORITY);
    esp_log_level_set("*", ESP_LOG_INFO);
}

// ── Command handlers (called from fc_sync_handle_line) ───────────────────────

static void handle_sync_start(void)
{
    if (g_recording_active) {
        printf("SYNC_BUSY recording_active\n");
        return;
    }
    sync_enter();
    char iso[32];
    rtc_iso_now(iso, sizeof(iso));
    printf("SYNC_READY " KOMPIC_HW_VERSION ".f" KOMPIC_FW_VERSION " %s\n", iso);
}

static void handle_ping(const char *line, int64_t t_recv_us)
{
    if (s.state != SYNC_ST_ACTIVE) {
        printf("ERR ping_before_ready\n");
        return;
    }
    int i = 0;
    // Parse the index; space-skip manually since sscanf %d handles leading space.
    if (sscanf(line + 4, " %d", &i) != 1) {
        printf("ERR malformed_line: PING parse fail\n");
        return;
    }
    if (i != s.next_ping_i) {
        printf("ERR seq_out_of_order: got %d expected %d\n", i, s.next_ping_i);
        return;
    }
    // t_recv_us was latched at \n detection in the CLI main loop — emit immediately.
    printf("PONG %d %" PRId64 "\n", i, t_recv_us);
    s.next_ping_i++;
}

static void handle_set_offset(const char *line)
{
    if (s.state != SYNC_ST_ACTIVE) {
        printf("ERR unknown_cmd: SET_OFFSET\n");
        return;
    }
    const char *arg = line + 10;   // skip "SET_OFFSET"
    while (*arg == ' ' || *arg == '\t') arg++;

    long long off_ll = 0;
    char ref_iso[32] = {0};
    if (sscanf(arg, " %lld %31s", &off_ll, ref_iso) < 1) {
        printf("ERR malformed_line: SET_OFFSET parse fail\n");
        return;
    }

    int64_t off_us = (int64_t)off_ll;

    // Latch to RAM.
    s.offset_valid = true;
    s.offset_us    = off_us;
    if (ref_iso[0]) {
        strncpy(s.ref_rtc_iso, ref_iso, sizeof(s.ref_rtc_iso) - 1);
        s.ref_rtc_iso[sizeof(s.ref_rtc_iso) - 1] = 0;
    }
    rtc_iso_now(s.kompic_rtc_iso, sizeof(s.kompic_rtc_iso));

    // Reply before NVS write so the PC isn't waiting on flash latency.
    printf("SYNC_OK offset_us=%" PRId64 " stored=ram+nvs_queued\n", off_us);
    fflush(stdout);

    // NVS write (blocking, ~10 ms — reply is already in TX buffer).
    int64_t write_uptime = esp_timer_get_time();
    esp_err_t r = nvs_cfg_sys_set_pc_sync(off_us, write_uptime);
    if (r != ESP_OK) {
        // Log silenced during sync, so use printf for this.
        printf("# NVS write failed: %s -- offset in RAM only\n", esp_err_to_name(r));
    }
}

static void handle_set_step(const char *line)
{
    if (s.state != SYNC_ST_ACTIVE) {
        printf("ERR unknown_cmd: SET_STEP\n");
        return;
    }
    const char *arg = line + 8;   // skip "SET_STEP"
    while (*arg == ' ' || *arg == '\t') arg++;

    long long step_ll = 0;
    if (sscanf(arg, " %lld", &step_ll) != 1) {
        printf("ERR malformed_line: SET_STEP parse fail\n");
        return;
    }
    if (step_ll < STEP_US_MIN || step_ll > STEP_US_MAX) {
        printf("STEP_ERR out_of_range: min=%d max=%d\n", STEP_US_MIN, STEP_US_MAX);
        return;
    }

    s.step_us       = (int64_t)step_ll;
    s.step_host_set = true;

    // effective_us == step_us: no ODR rounding until PPG+BCG mode is implemented.
    printf("STEP_OK step_us=%" PRId64 " effective_us=%" PRId64 " mode_default=%d\n",
           s.step_us, s.step_us, STEP_US_DEFAULT);
}

static void handle_sync_end(void)
{
    sync_exit();
    // No reply.
}

// ── Main dispatch ────────────────────────────────────────────────────────────

void fc_sync_handle_line(const char *line, int64_t t_recv_us)
{
    if (startswith_ci(line, "SYNC_START")) { handle_sync_start();              return; }
    if (startswith_ci(line, "SYNC_END"))   { handle_sync_end();                return; }
    if (startswith_ci(line, "PING"))       { handle_ping(line, t_recv_us);     return; }
    if (startswith_ci(line, "SET_OFFSET")) { handle_set_offset(line);          return; }
    if (startswith_ci(line, "SET_STEP"))   { handle_set_step(line);            return; }

    if (s.state == SYNC_ST_ACTIVE) {
        // Echo first 32 bytes of unrecognised line.
        char snippet[33];
        strncpy(snippet, line, 32);
        snippet[32] = 0;
        printf("ERR unknown_cmd: %s\n", snippet);
    }
}
