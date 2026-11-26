#define _POSIX_C_SOURCE 200809L
#include "srr/srr_sid.h"
#include "srr/srr_p1_hub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

static volatile sig_atomic_t g_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_running = 0;
}

/* 6 SID chips in Hub RAM (32 bytes per chip = 192 bytes total) */
static uint8_t g_sid_regs[SRR_SID_MAX_CHIPS][SRR_SID_TOTAL_REGS] = {{0}};

static void print_voice_bar(int v, uint16_t freq, uint8_t cr) {
    char wave_char = '.';
    if (cr & SRR_SID_CR_SAW) wave_char = '/';
    else if (cr & SRR_SID_CR_TRI) wave_char = '^';
    else if (cr & SRR_SID_CR_PULSE) wave_char = '#';
    else if (cr & SRR_SID_CR_NOISE) wave_char = '?';

    int bar_len = (freq > 0) ? (freq * 12 / 65535) : 0;
    if (bar_len > 12) bar_len = 12;

    char bar[16];
    for (int i = 0; i < bar_len; i++) bar[i] = wave_char;
    bar[bar_len] = '\0';

    fprintf(stderr, " [V%d:%5u%c|%-12s]", v + 1, freq, (cr & SRR_SID_CR_GATE) ? '*' : ' ', bar);
}

static void render_sid_state(void) {
    /* Only render active chips */
    for (int c = 0; c < (int)SRR_SID_MAX_CHIPS; c++) {
        bool active = false;
        for (int v = 0; v < 3; v++) {
            int base = v * 7;
            uint16_t f = g_sid_regs[c][base] | ((uint16_t)g_sid_regs[c][base + 1] << 8);
            uint8_t cr = g_sid_regs[c][base + 4];
            if (f > 0 || (cr & SRR_SID_CR_GATE)) {
                active = true;
                break;
            }
        }
        if (active || c < 2) {
            fprintf(stderr, "\r[SID%d %s]", c, (c == 0) ? "LEFT " : (c == 1) ? "RIGHT" : "SURR ");
            for (int v = 0; v < 3; v++) {
                int base = v * 7;
                uint16_t freq = g_sid_regs[c][base] | ((uint16_t)g_sid_regs[c][base + 1] << 8);
                uint8_t cr = g_sid_regs[c][base + 4];
                print_voice_bar(v, freq, cr);
            }
            uint8_t vol = g_sid_regs[c][SRR_SID_MODE_VOL] & SRR_SID_VOL_MASK;
            fprintf(stderr, " Vol:%2u\n", vol);
        }
    }
    fflush(stderr);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    fprintf(stderr, "=================================================================\n");
    fprintf(stderr, " Parallax Propeller P1 Multi-SID Server Simulator (6 SIDs / 18 Voices)\n");
    fprintf(stderr, "=================================================================\n");
    fprintf(stderr, "  Cog 0: SRR Wire Router & Frame Dispatcher\n");
    fprintf(stderr, "  Cog 1: PASM High-Speed UART (FullDuplexSerial)\n");
    fprintf(stderr, "  Cog 2: MOS 6581 SID #0 (Stereo Left  - Pins P10/P11)\n");
    fprintf(stderr, "  Cog 3: MOS 6581 SID #1 (Stereo Right - Pins P12/P13)\n");
    fprintf(stderr, "  Cog 4: MOS 6581 SID #2 (Pins P14/P15)\n");
    fprintf(stderr, "  Cog 5: MOS 6581 SID #3 (Pins P16/P17)\n");
    fprintf(stderr, "  Cog 6: MOS 6581 SID #4 (Pins P18/P19)\n");
    fprintf(stderr, "  Cog 7: MOS 6581 SID #5 (Pins P20/P21)\n");
    fprintf(stderr, "  Hub RAM footprint: 6 SIDs x 32 bytes = 192 bytes total\n");
    fprintf(stderr, "Ready. Streaming over stdin/stdout...\n\n");

    srr_p1_hub_t hub;
    srr_p1_hub_init(&hub, 0x53494431, 0);

    for (int c = 0; c < (int)SRR_SID_MAX_CHIPS; c++) {
        g_sid_regs[c][SRR_SID_MODE_VOL] = 0x0F;
    }

    uint8_t in_buf[128];

    while (g_running) {
        /* 1. Simulate Cog 1 PASM UART RX from stdin */
        ssize_t n = read(STDIN_FILENO, in_buf, sizeof(in_buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                hub.rx_ring[hub.rx_head] = in_buf[i];
                hub.rx_head = (hub.rx_head + 1) % SRR_P1_RING_SIZE;
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno != EAGAIN && errno != EINTR) break;
        }

        /* 2. Cog 0 executes step: parses frames, loads into Hub mailbox */
        srr_p1_io_cog_step(&hub);

        /* 3. Check for requests directed to individual SIDs (0x0050..0x0055) */
        for (uint16_t c = 0; c < SRR_SID_MAX_CHIPS; c++) {
            uint16_t target_svc = SRR_SVC_SID_BASE + c;
            uint16_t op = 0;
            uint64_t tid = 0;
            uint8_t payload[SRR_P1_MAX_PAYLOAD];
            uint8_t req_len = 0;

            if (srr_p1_worker_claim(&hub, (uint8_t)(c + 2), target_svc, &op, &tid, payload, &req_len) ||
                (c == 0 && srr_p1_worker_claim(&hub, 2, SRR_SVC_SID_LEGACY, &op, &tid, payload, &req_len)))
            {
                uint8_t reply_buf[32];
                uint8_t rep_len = 0;
                bool is_error = false;

                switch (op) {
                case SRR_SID_OP_WRITE_REG:
                    if (req_len >= 2) {
                        uint8_t reg = payload[0];
                        uint8_t val = payload[1];
                        if (reg < SRR_SID_TOTAL_REGS) {
                            g_sid_regs[c][reg] = val;
                        }
                    }
                    reply_buf[0] = 0;
                    rep_len = 1;
                    break;

                case SRR_SID_OP_WRITE_BATCH: {
                    int count = 0;
                    for (int i = 0; i + 1 < req_len; i += 2) {
                        uint8_t reg = payload[i];
                        uint8_t val = payload[i + 1];
                        if (reg < SRR_SID_TOTAL_REGS) {
                            g_sid_regs[c][reg] = val;
                            count++;
                        }
                    }
                    reply_buf[0] = (uint8_t)count;
                    rep_len = 1;
                    break;
                }

                case SRR_SID_OP_WRITE_DUMP:
                    if (req_len > 0) {
                        size_t copy_sz = req_len < SRR_SID_NUM_REGS ? req_len : SRR_SID_NUM_REGS;
                        memcpy(g_sid_regs[c], payload, copy_sz);
                    }
                    reply_buf[0] = 0;
                    rep_len = 1;
                    break;

                case SRR_SID_OP_RESET:
                    memset(g_sid_regs[c], 0, sizeof(g_sid_regs[c]));
                    g_sid_regs[c][SRR_SID_MODE_VOL] = 0x0F;
                    reply_buf[0] = 0;
                    rep_len = 1;
                    break;

                case SRR_SID_OP_READ_REGS:
                    memcpy(reply_buf, g_sid_regs[c], SRR_SID_NUM_REGS);
                    rep_len = SRR_SID_NUM_REGS;
                    break;

                /* Multi-chip collective commands received on base */
                case SRR_SID_OP_MULTI_WRITE_REG:
                    if (req_len >= 3) {
                        uint8_t chip = payload[0];
                        uint8_t reg  = payload[1];
                        uint8_t val  = payload[2];
                        if (chip < SRR_SID_MAX_CHIPS && reg < SRR_SID_TOTAL_REGS) {
                            g_sid_regs[chip][reg] = val;
                        }
                    }
                    reply_buf[0] = 0;
                    rep_len = 1;
                    break;

                case SRR_SID_OP_MULTI_WRITE_BATCH: {
                    int count = 0;
                    for (int i = 0; i + 2 < req_len; i += 3) {
                        uint8_t chip = payload[i];
                        uint8_t reg  = payload[i + 1];
                        uint8_t val  = payload[i + 2];
                        if (chip < SRR_SID_MAX_CHIPS && reg < SRR_SID_TOTAL_REGS) {
                            g_sid_regs[chip][reg] = val;
                            count++;
                        }
                    }
                    reply_buf[0] = (uint8_t)count;
                    rep_len = 1;
                    break;
                }

                case SRR_SID_OP_RESET_ALL:
                    for (int ch = 0; ch < (int)SRR_SID_MAX_CHIPS; ch++) {
                        memset(g_sid_regs[ch], 0, sizeof(g_sid_regs[ch]));
                        g_sid_regs[ch][SRR_SID_MODE_VOL] = 0x0F;
                    }
                    reply_buf[0] = 0;
                    rep_len = 1;
                    break;

                default:
                    is_error = true;
                    break;
                }

                srr_p1_worker_reply(&hub, (uint8_t)(c + 2), tid, reply_buf, rep_len, is_error);
                render_sid_state();
                break; /* Processed transaction */
            }
        }

        /* 4. Cog 0 encodes reply from mailbox into TX ring */
        srr_p1_io_cog_step(&hub);

        /* 5. Transmit pending bytes from hub.tx_ring to stdout */
        while (hub.tx_tail != hub.tx_head) {
            uint8_t b = hub.tx_ring[hub.tx_tail];
            hub.tx_tail = (hub.tx_tail + 1) % SRR_P1_RING_SIZE;
            ssize_t written = write(STDOUT_FILENO, &b, 1);
            (void)written;
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000L };
        nanosleep(&ts, NULL);
    }

    fprintf(stderr, "\nSimulator stopped cleanly.\n");
    return 0;
}
