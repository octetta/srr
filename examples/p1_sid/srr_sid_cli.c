#define _POSIX_C_SOURCE 200809L
#include "srr/srr_sid.h"
#include "srr/srr_serial.h"
#include "srr/srr_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>

static void sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int sid_write_reg(srr_serial_port_t *port, uint8_t reg, uint8_t val) {
    srr_sid_reg_write_t payload = { .reg = reg, .val = val };
    uint8_t reply[16];
    size_t actual_len = 0;
    return srr_serial_request(
        port, SRR_SVC_SID, SRR_SID_OP_WRITE_REG,
        &payload, sizeof(payload),
        reply, sizeof(reply), &actual_len, 1000
    );
}

static int sid_pulse_reg(srr_serial_port_t *port, uint8_t reg, uint8_t val) {
    uint8_t payload[2] = { reg, val };
    return srr_serial_pulse(port, SRR_SVC_SID, reg, payload, sizeof(payload));
}

static int sid_write_batch(srr_serial_port_t *port, const srr_sid_reg_write_t *pairs, size_t count) {
    uint8_t reply[16];
    size_t actual_len = 0;
    return srr_serial_request(
        port, SRR_SVC_SID, SRR_SID_OP_WRITE_BATCH,
        pairs, count * sizeof(srr_sid_reg_write_t),
        reply, sizeof(reply), &actual_len, 1000
    );
}

static int sid_reset(srr_serial_port_t *port) {
    uint8_t reply[16];
    size_t actual_len = 0;
    return srr_serial_request(
        port, SRR_SVC_SID, SRR_SID_OP_RESET,
        NULL, 0,
        reply, sizeof(reply), &actual_len, 1000
    );
}

static int sid_read_regs(srr_serial_port_t *port, uint8_t *out_regs, size_t cap) {
    size_t actual_len = 0;
    int rc = srr_serial_request(
        port, SRR_SVC_SID, SRR_SID_OP_READ_REGS,
        NULL, 0,
        out_regs, cap, &actual_len, 1000
    );
    if (rc != 0 || actual_len < 25) return -1;
    return 0;
}

static void sid_print_status(const uint8_t *regs) {
    printf("\n=== MOS 6581 SID Register State ($00..$18) ===\n");
    for (int v = 0; v < 3; v++) {
        int base = v * 7;
        uint16_t freq = regs[base] | ((uint16_t)regs[base + 1] << 8);
        uint16_t pw = (regs[base + 2] | ((uint16_t)regs[base + 3] << 8)) & 0x0FFF;
        uint8_t cr = regs[base + 4];
        uint8_t ad = regs[base + 5];
        uint8_t sr = regs[base + 6];

        printf("Voice %d: Freq=0x%04X (%5u) | PW=0x%03X | CR=0x%02X [", v + 1, freq, freq, pw, cr);
        if (cr & SRR_SID_CR_GATE) printf(" GATE");
        if (cr & SRR_SID_CR_TRI) printf(" TRI");
        if (cr & SRR_SID_CR_SAW) printf(" SAW");
        if (cr & SRR_SID_CR_PULSE) printf(" PULSE");
        if (cr & SRR_SID_CR_NOISE) printf(" NOISE");
        printf(" ] | Att=%u Dec=%u Sus=%u Rel=%u\n",
               ad >> 4, ad & 0x0F, sr >> 4, sr & 0x0F);
    }
    uint16_t fc = ((uint16_t)regs[SRR_SID_FC_HI] << 3) | (regs[SRR_SID_FC_LO] & 0x07);
    uint8_t res_filt = regs[SRR_SID_RES_FILT];
    uint8_t mode_vol = regs[SRR_SID_MODE_VOL];
    printf("Filter : Cutoff=0x%03X (%u) | Res=%u | Voices=[%s%s%s ] | Mode=[%s%s%s ] | Vol=%u/15\n",
           fc, fc, res_filt >> 4,
           (res_filt & 1) ? " V1" : "", (res_filt & 2) ? " V2" : "", (res_filt & 4) ? " V3" : "",
           (mode_vol & SRR_SID_FILT_LP) ? " LP" : "",
           (mode_vol & SRR_SID_FILT_BP) ? " BP" : "",
           (mode_vol & SRR_SID_FILT_HP) ? " HP" : "",
           mode_vol & SRR_SID_VOL_MASK);
    printf("==============================================\n");
}

/* Play classic chiptune arpeggio (C minor: C-4, Eb-4, G-4, Bb-4) */
static void run_arpeggio_demo(srr_serial_port_t *port) {
    printf("[DEMO] Playing classic Commodore 64 arpeggio lead on Voice 1...\n");
    sid_reset(port);

    /* Setup Master Volume = 15, Voice 1 ADSR: Attack 2ms, Decay 100ms, Sustain 12, Release 200ms */
    srr_sid_reg_write_t init_batch[] = {
        { SRR_SID_MODE_VOL, 0x0F },       /* Volume 15 */
        { SRR_SID_V1_AD, 0x14 },          /* Attack 1, Decay 4 */
        { SRR_SID_V1_SR, 0xC4 },          /* Sustain 12, Release 4 */
        { SRR_SID_V1_CR, SRR_SID_CR_SAW | SRR_SID_CR_GATE } /* Sawtooth + Gate */
    };
    sid_write_batch(port, init_batch, sizeof(init_batch) / sizeof(init_batch[0]));

    /* C Minor chord frequencies: C4 (60), D#4 (63), G4 (67), A#4 (70) */
    uint8_t notes[] = { 60, 63, 67, 70, 72, 70, 67, 63 };
    size_t num_notes = sizeof(notes) / sizeof(notes[0]);

    for (int cycle = 0; cycle < 16; cycle++) {
        for (size_t n = 0; n < num_notes; n++) {
            uint16_t f = srr_sid_freq_midi(notes[n]);
            /* Update frequency via ultra-fast asynchronous SRR pulses */
            sid_pulse_reg(port, SRR_SID_V1_FREQ_LO, (uint8_t)(f & 0xFF));
            sid_pulse_reg(port, SRR_SID_V1_FREQ_HI, (uint8_t)(f >> 8));
            sleep_ms(30); /* 30ms per note -> fast 16th note chiptune arp */
        }
    }

    /* Release voice */
    sid_write_reg(port, SRR_SID_V1_CR, SRR_SID_CR_SAW);
    sleep_ms(150);
    sid_reset(port);
    printf("[DEMO] Arpeggio finished.\n");
}

/* Play Bassline with Low-Pass Filter Resonance Sweep */
static void run_filter_sweep_demo(srr_serial_port_t *port) {
    printf("[DEMO] Playing bass note with resonant low-pass filter sweep...\n");
    sid_reset(port);

    uint16_t bass_freq = srr_sid_freq_midi(36); /* C2 Bass */
    srr_sid_reg_write_t setup[] = {
        { SRR_SID_MODE_VOL, 0x0F | SRR_SID_FILT_LP }, /* Volume 15 + Low-Pass Filter */
        { SRR_SID_RES_FILT, 0xC1 },                   /* Resonance 12, Route Voice 1 through filter */
        { SRR_SID_V1_AD, 0x00 },                      /* Instant attack */
        { SRR_SID_V1_SR, 0xF0 },                      /* Max sustain */
        { SRR_SID_V1_FREQ_LO, (uint8_t)(bass_freq & 0xFF) },
        { SRR_SID_V1_FREQ_HI, (uint8_t)(bass_freq >> 8) },
        { SRR_SID_V1_CR, SRR_SID_CR_SAW | SRR_SID_CR_GATE }
    };
    sid_write_batch(port, setup, sizeof(setup) / sizeof(setup[0]));

    /* Sweep filter cutoff up and down */
    for (int dir = 0; dir < 2; dir++) {
        for (int step = 0; step <= 128; step++) {
            uint16_t fc = (dir == 0) ? (step * 15) : ((128 - step) * 15);
            sid_pulse_reg(port, SRR_SID_FC_LO, (uint8_t)(fc & 0x07));
            sid_pulse_reg(port, SRR_SID_FC_HI, (uint8_t)((fc >> 3) & 0xFF));
            sleep_ms(10);
        }
    }

    sid_reset(port);
    printf("[DEMO] Filter sweep finished.\n");
}

int main(int argc, char *argv[]) {
    const char *port_path = "--sim";
    const char *cmd = "demo";

    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("Usage: %s [--sim | /dev/ttyUSB0] [command] [args...]\n", argv[0]);
            printf("Commands:\n");
            printf("  demo                  Run arpeggio & filter sweep demonstration\n");
            printf("  reset                 Silence all voices and reset registers\n");
            printf("  read                  Display formatted register dump\n");
            printf("  set <reg> <val>       Write single register (decimal or 0x hex)\n");
            printf("  pulse <reg> <val>     Send asynchronous pulse write\n");
            printf("  arp                   Play chord arpeggio\n");
            printf("  sweep                 Play resonant filter sweep\n");
            return 0;
        }
        port_path = argv[1];
    }
    if (argc > 2) {
        cmd = argv[2];
    }

    srr_serial_config_t cfg;
    srr_serial_default_config(&cfg);
    cfg.baud_rate = 115200;

    srr_serial_port_t *port = NULL;
    pid_t sim_pid = -1;

    if (strcmp(port_path, "--sim") == 0) {
        printf("--- Launching Integrated Parallax Propeller P1 Multi-Cog SID Simulator ---\n");
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            perror("socketpair");
            return 1;
        }
        sim_pid = fork();
        if (sim_pid == 0) {
            close(fds[0]);
            dup2(fds[1], STDIN_FILENO);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            execl("./examples/p1_sid/srr_p1_sid_sim", "./examples/p1_sid/srr_p1_sid_sim", (char *)NULL);
            perror("execl");
            exit(1);
        }
        close(fds[1]);
        sleep_ms(50); /* Let simulator initialize */
        port = srr_serial_from_fd(fds[0], true, &cfg);
    } else {
        printf("Connecting to SRR Parallax SID device at '%s' @ %u baud...\n", port_path, cfg.baud_rate);
        port = srr_serial_open(port_path, &cfg);
    }

    if (!port) {
        fprintf(stderr, "Failed to connect to SID device. (Ensure Propeller board is plugged in or use --sim)\n");
        if (sim_pid > 0) {
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, NULL, 0);
        }
        return 1;
    }

    if (strcmp(cmd, "reset") == 0) {
        if (sid_reset(port) == 0) {
            printf("[OK] SID reset completed.\n");
        } else {
            fprintf(stderr, "[FAIL] SID reset failed.\n");
        }
    } else if (strcmp(cmd, "read") == 0) {
        uint8_t regs[32];
        if (sid_read_regs(port, regs, sizeof(regs)) == 0) {
            sid_print_status(regs);
        } else {
            fprintf(stderr, "[FAIL] Could not read SID registers.\n");
        }
    } else if (strcmp(cmd, "set") == 0 && argc >= 5) {
        uint8_t reg = (uint8_t)strtoul(argv[3], NULL, 0);
        uint8_t val = (uint8_t)strtoul(argv[4], NULL, 0);
        if (sid_write_reg(port, reg, val) == 0) {
            printf("[OK] Register 0x%02X set to 0x%02X\n", reg, val);
        } else {
            fprintf(stderr, "[FAIL] Failed to set register.\n");
        }
    } else if (strcmp(cmd, "pulse") == 0 && argc >= 5) {
        uint8_t reg = (uint8_t)strtoul(argv[3], NULL, 0);
        uint8_t val = (uint8_t)strtoul(argv[4], NULL, 0);
        sid_pulse_reg(port, reg, val);
        printf("[OK] Pulse sent for register 0x%02X = 0x%02X\n", reg, val);
    } else if (strcmp(cmd, "arp") == 0) {
        run_arpeggio_demo(port);
    } else if (strcmp(cmd, "sweep") == 0) {
        run_filter_sweep_demo(port);
    } else {
        /* Default demo */
        run_arpeggio_demo(port);
        sleep_ms(250);
        run_filter_sweep_demo(port);
        uint8_t regs[32];
        if (sid_read_regs(port, regs, sizeof(regs)) == 0) {
            sid_print_status(regs);
        }
    }

    srr_serial_close(port);

    if (sim_pid > 0) {
        kill(sim_pid, SIGTERM);
        waitpid(sim_pid, NULL, 0);
    }
    return 0;
}
