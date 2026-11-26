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

/* 32-byte SID Register bank in Hub RAM */
static uint8_t g_sid_regs[SRR_SID_TOTAL_REGS] = {0};

static void print_voice_bar(int v, uint16_t freq, uint8_t cr) {
    char wave_char = '.';
    if (cr & SRR_SID_CR_SAW) wave_char = '/';
    else if (cr & SRR_SID_CR_TRI) wave_char = '^';
    else if (cr & SRR_SID_CR_PULSE) wave_char = '#';
    else if (cr & SRR_SID_CR_NOISE) wave_char = '?';

    int bar_len = (freq > 0) ? (freq * 20 / 65535) : 0;
    if (bar_len > 20) bar_len = 20;

    char bar[24];
    for (int i = 0; i < bar_len; i++) bar[i] = wave_char;
    bar[bar_len] = '\0';

    fprintf(stderr, " [V%d: %5u Hz %c |%-20s]", v + 1, freq, (cr & SRR_SID_CR_GATE) ? '*' : ' ', bar);
}

static void render_sid_state(void) {
    fprintf(stderr, "\r[SID]");
    for (int v = 0; v < 3; v++) {
        int base = v * 7;
        uint16_t freq = g_sid_regs[base] | ((uint16_t)g_sid_regs[base + 1] << 8);
        uint8_t cr = g_sid_regs[base + 4];
        print_voice_bar(v, freq, cr);
    }
    uint8_t vol = g_sid_regs[SRR_SID_MODE_VOL] & SRR_SID_VOL_MASK;
    fprintf(stderr, " Vol:%2u", vol);
    fflush(stderr);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    fprintf(stderr, "=== Parallax Propeller P1 Multi-Cog SID Server Simulator ===\n");
    fprintf(stderr, "  Cog 0: High-Speed UART I/O Engine\n");
    fprintf(stderr, "  Cog 1: SRR SID Service Dispatcher (Hub Lock 0)\n");
    fprintf(stderr, "  Cog 2: MOS 6581 SIDcog Audio Synthesis Core\n");
    fprintf(stderr, "Ready. Streaming over stdin/stdout...\n\n");

    srr_p1_hub_t hub;
    srr_p1_hub_init(&hub, 0x53494431, 0); /* Session "SID1", Lock 0 */
    g_sid_regs[SRR_SID_MODE_VOL] = 0x0F;   /* Volume 15 */

    uint8_t in_buf[128];

    while (g_running) {
        /* 1. Simulate Cog 0 PASM UART RX from stdin */
        ssize_t n = read(STDIN_FILENO, in_buf, sizeof(in_buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                hub.rx_ring[hub.rx_head] = in_buf[i];
                hub.rx_head = (hub.rx_head + 1) % SRR_P1_RING_SIZE;
            }
        } else if (n == 0) {
            /* EOF */
            break;
        } else {
            if (errno != EAGAIN && errno != EINTR) break;
        }

        /* 2. Cog 0 executes step: parses frames, loads into Hub mailbox */
        srr_p1_io_cog_step(&hub);

        /* 3. Cog 1 (SID Worker) checks mailbox for SVC_SID requests */
        uint16_t op = 0;
        uint64_t tid = 0;
        uint8_t payload[SRR_P1_MAX_PAYLOAD];
        uint8_t req_len = 0;

        if (srr_p1_worker_claim(&hub, 1, SRR_SVC_SID, &op, &tid, payload, &req_len)) {
            uint8_t reply_buf[32];
            uint8_t rep_len = 0;
            bool is_error = false;

            switch (op) {
            case SRR_SID_OP_WRITE_REG:
                if (req_len >= 2) {
                    uint8_t reg = payload[0];
                    uint8_t val = payload[1];
                    if (reg < SRR_SID_TOTAL_REGS) {
                        g_sid_regs[reg] = val;
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
                        g_sid_regs[reg] = val;
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
                    memcpy(g_sid_regs, payload, copy_sz);
                }
                reply_buf[0] = 0;
                rep_len = 1;
                break;

            case SRR_SID_OP_RESET:
                memset(g_sid_regs, 0, sizeof(g_sid_regs));
                g_sid_regs[SRR_SID_MODE_VOL] = 0x0F;
                reply_buf[0] = 0;
                rep_len = 1;
                break;

            case SRR_SID_OP_READ_REGS:
                memcpy(reply_buf, g_sid_regs, SRR_SID_NUM_REGS);
                rep_len = SRR_SID_NUM_REGS;
                break;

            default:
                is_error = true;
                break;
            }

            srr_p1_worker_reply(&hub, 1, tid, reply_buf, rep_len, is_error);
            render_sid_state();
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
