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

static int sid_write_reg(srr_serial_port_t *port, uint8_t chip, uint8_t reg, uint8_t val) {
    srr_sid_reg_write_t payload = { .reg = reg, .val = val };
    uint8_t reply[16];
    size_t actual_len = 0;
    return srr_serial_request(
        port, SRR_SVC_SID_CHIP(chip), SRR_SID_OP_WRITE_REG,
        &payload, sizeof(payload),
        reply, sizeof(reply), &actual_len, 1000
    );
}

static int sid_pulse_reg(srr_serial_port_t *port, uint8_t chip, uint8_t reg, uint8_t val) {
    uint8_t payload[2] = { reg, val };
    return srr_serial_pulse(port, SRR_SVC_SID_CHIP(chip), reg, payload, sizeof(payload));
}

static int sid_write_batch(srr_serial_port_t *port, uint8_t chip, const srr_sid_reg_write_t *pairs, size_t count) {
    uint8_t reply[16];
    size_t actual_len = 0;
    return srr_serial_request(
        port, SRR_SVC_SID_CHIP(chip), SRR_SID_OP_WRITE_BATCH,
        pairs, count * sizeof(srr_sid_reg_write_t),
        reply, sizeof(reply), &actual_len, 1000
    );
}

static int sid_reset(srr_serial_port_t *port, uint8_t chip) {
    uint8_t reply[16];
    size_t actual_len = 0;
    return srr_serial_request(
        port, SRR_SVC_SID_CHIP(chip), SRR_SID_OP_RESET,
        NULL, 0,
        reply, sizeof(reply), &actual_len, 1000
    );
}

static int sid_reset_all(srr_serial_port_t *port) {
    uint8_t reply[16];
    size_t actual_len = 0;
    for (uint8_t c = 0; c < SRR_SID_MAX_CHIPS; c++) {
        sid_reset(port, c);
    }
    return srr_serial_request(
        port, SRR_SVC_SID_BASE, SRR_SID_OP_RESET_ALL,
        NULL, 0,
        reply, sizeof(reply), &actual_len, 1000
    );
}

static int sid_read_regs(srr_serial_port_t *port, uint8_t chip, uint8_t *out_regs, size_t cap) {
    size_t actual_len = 0;
    int rc = srr_serial_request(
        port, SRR_SVC_SID_CHIP(chip), SRR_SID_OP_READ_REGS,
        NULL, 0,
        out_regs, cap, &actual_len, 1000
    );
    if (rc != 0 || actual_len < 25) return -1;
    return 0;
}

static void sid_print_status(uint8_t chip, const uint8_t *regs) {
    printf("\n=== MOS 6581 SID #%u (%s) Registers ($00..$18) ===\n",
           chip, (chip == 0) ? "Stereo Left" : (chip == 1) ? "Stereo Right" : "Surround");
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
    printf("========================================================\n");
}

/* 1. Stereo Dual-SID Demonstration (Massive chorus spread across SID 0 Left and SID 1 Right) */
static void run_stereo_demo(srr_serial_port_t *port) {
    printf("\n[STEREO DEMO] Running Dual-SID Stereo Chorus (SID 0 = Left, SID 1 = Right)...\n");
    sid_reset_all(port);

    /* Setup SID 0 (Left): Sawtooth, fast attack */
    srr_sid_reg_write_t left_setup[] = {
        { SRR_SID_MODE_VOL, 0x0F },
        { SRR_SID_V1_AD, 0x14 },
        { SRR_SID_V1_SR, 0xC8 },
        { SRR_SID_V1_CR, SRR_SID_CR_SAW | SRR_SID_CR_GATE }
    };
    sid_write_batch(port, 0, left_setup, sizeof(left_setup) / sizeof(left_setup[0]));

    /* Setup SID 1 (Right): Sawtooth, slightly different envelope for stereo richness */
    srr_sid_reg_write_t right_setup[] = {
        { SRR_SID_MODE_VOL, 0x0F },
        { SRR_SID_V1_AD, 0x24 },
        { SRR_SID_V1_SR, 0xB8 },
        { SRR_SID_V1_CR, SRR_SID_CR_SAW | SRR_SID_CR_GATE }
    };
    sid_write_batch(port, 1, right_setup, sizeof(right_setup) / sizeof(right_setup[0]));

    /* Add a deep sub-bassline on SID 0 Voice 2 */
    uint16_t bass_freq = srr_sid_freq_midi(36); /* C2 Bass */
    srr_sid_reg_write_t bass_setup[] = {
        { SRR_SID_V2_AD, 0x00 },
        { SRR_SID_V2_SR, 0xF0 },
        { SRR_SID_V2_FREQ_LO, (uint8_t)(bass_freq & 0xFF) },
        { SRR_SID_V2_FREQ_HI, (uint8_t)(bass_freq >> 8) },
        { SRR_SID_V2_CR, SRR_SID_CR_TRI | SRR_SID_CR_GATE }
    };
    sid_write_batch(port, 0, bass_setup, sizeof(bass_setup) / sizeof(bass_setup[0]));

    /* Melodic arpeggio across stereo channels with ~4 Hz detune on the right channel */
    uint8_t melody[] = { 60, 63, 67, 70, 72, 75, 72, 70 };
    size_t num_notes = sizeof(melody) / sizeof(melody[0]);

    for (int rep = 0; rep < 12; rep++) {
        for (size_t i = 0; i < num_notes; i++) {
            uint16_t f_left = srr_sid_freq_midi(melody[i]);
            uint16_t f_right = (uint16_t)(f_left + 18); /* ~4 Hz stereo chorus detune */

            /* Update Left Channel (SID 0) */
            sid_pulse_reg(port, 0, SRR_SID_V1_FREQ_LO, (uint8_t)(f_left & 0xFF));
            sid_pulse_reg(port, 0, SRR_SID_V1_FREQ_HI, (uint8_t)(f_left >> 8));

            /* Update Right Channel (SID 1) */
            sid_pulse_reg(port, 1, SRR_SID_V1_FREQ_LO, (uint8_t)(f_right & 0xFF));
            sid_pulse_reg(port, 1, SRR_SID_V1_FREQ_HI, (uint8_t)(f_right >> 8));

            sleep_ms(35);
        }
    }

    sid_reset_all(port);
    printf("[STEREO DEMO] Stereo chorus demo finished.\n");
}

/* 2. Polyphonic 6-Voice Chord Demonstration (using all 6 voices across SID 0 and SID 1) */
static void run_poly_demo(srr_serial_port_t *port) {
    printf("\n[POLY DEMO] Playing 6-Voice Polyphonic Synthesizer Chords (SIDs 0 & 1)...\n");
    sid_reset_all(port);

    /* Setup voices 1..3 on SID 0 and voices 1..3 on SID 1 */
    for (uint8_t c = 0; c < 2; c++) {
        srr_sid_reg_write_t init[] = {
            { SRR_SID_MODE_VOL, 0x0F },
            { SRR_SID_V1_AD, 0x22 }, { SRR_SID_V1_SR, 0xD4 },
            { SRR_SID_V2_AD, 0x22 }, { SRR_SID_V2_SR, 0xD4 },
            { SRR_SID_V3_AD, 0x22 }, { SRR_SID_V3_SR, 0xD4 }
        };
        sid_write_batch(port, c, init, sizeof(init) / sizeof(init[0]));
    }

    /* Chord 1: C Minor 9th (C3, G3, Eb4 on SID 0 | Bb4, D5, G5 on SID 1) */
    uint8_t chord_cm9[] = { 48, 55, 63, 70, 74, 79 };
    printf("  -> Chord 1: C Minor 9th (6 voices polyphony)\n");
    for (int v = 0; v < 3; v++) {
        uint16_t f0 = srr_sid_freq_midi(chord_cm9[v]);
        uint16_t f1 = srr_sid_freq_midi(chord_cm9[v + 3]);

        sid_write_reg(port, 0, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_LO), (uint8_t)(f0 & 0xFF));
        sid_write_reg(port, 0, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_HI), (uint8_t)(f0 >> 8));
        sid_write_reg(port, 0, (uint8_t)(v * 7 + SRR_SID_V1_CR), SRR_SID_CR_SAW | SRR_SID_CR_GATE);

        sid_write_reg(port, 1, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_LO), (uint8_t)(f1 & 0xFF));
        sid_write_reg(port, 1, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_HI), (uint8_t)(f1 >> 8));
        sid_write_reg(port, 1, (uint8_t)(v * 7 + SRR_SID_V1_CR), SRR_SID_CR_SAW | SRR_SID_CR_GATE);
    }
    sleep_ms(700);

    /* Chord 2: F Minor 11th (F3, C4, Ab4 on SID 0 | Eb5, G5, C6 on SID 1) */
    uint8_t chord_fm11[] = { 53, 60, 68, 75, 79, 84 };
    printf("  -> Chord 2: F Minor 11th (6 voices polyphony)\n");
    for (int v = 0; v < 3; v++) {
        uint16_t f0 = srr_sid_freq_midi(chord_fm11[v]);
        uint16_t f1 = srr_sid_freq_midi(chord_fm11[v + 3]);

        sid_write_reg(port, 0, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_LO), (uint8_t)(f0 & 0xFF));
        sid_write_reg(port, 0, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_HI), (uint8_t)(f0 >> 8));

        sid_write_reg(port, 1, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_LO), (uint8_t)(f1 & 0xFF));
        sid_write_reg(port, 1, (uint8_t)(v * 7 + SRR_SID_V1_FREQ_HI), (uint8_t)(f1 >> 8));
    }
    sleep_ms(800);

    /* Release all 6 voices */
    for (int v = 0; v < 3; v++) {
        sid_write_reg(port, 0, (uint8_t)(v * 7 + SRR_SID_V1_CR), SRR_SID_CR_SAW);
        sid_write_reg(port, 1, (uint8_t)(v * 7 + SRR_SID_V1_CR), SRR_SID_CR_SAW);
    }
    sleep_ms(250);
    sid_reset_all(port);
    printf("[POLY DEMO] Polyphonic chord demo finished.\n");
}

int main(int argc, char *argv[]) {
    const char *port_path = "--sim";
    const char *cmd = "demo";
    uint8_t target_chip = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--sim | /dev/ttyUSB0] [--chip <0..5>] [command] [args...]\n", argv[0]);
            printf("Commands:\n");
            printf("  demo                  Run arpeggio, stereo chorus & polyphony demo\n");
            printf("  stereo                Run Dual-SID Stereo Chorus lead\n");
            printf("  poly                  Run 6-Voice Polyphonic chord demonstration\n");
            printf("  reset                 Silence target chip\n");
            printf("  reset-all             Silence all 6 SID chips (18 voices)\n");
            printf("  read                  Display formatted register dump of chip\n");
            printf("  set <reg> <val>       Write single register on target chip\n");
            printf("  pulse <reg> <val>     Send asynchronous pulse to target chip\n");
            return 0;
        } else if (strcmp(argv[i], "--chip") == 0 && i + 1 < argc) {
            target_chip = (uint8_t)atoi(argv[++i]);
            if (target_chip >= SRR_SID_MAX_CHIPS) target_chip = 0;
        } else if (argv[i][0] == '/' || strcmp(argv[i], "--sim") == 0) {
            port_path = argv[i];
        } else {
            cmd = argv[i];
        }
    }

    srr_serial_config_t cfg;
    srr_serial_default_config(&cfg);
    cfg.baud_rate = 115200;

    srr_serial_port_t *port = NULL;
    pid_t sim_pid = -1;

    if (strcmp(port_path, "--sim") == 0) {
        printf("--- Launching Integrated Parallax Propeller P1 Multi-SID Simulator (6 Chips) ---\n");
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
        sleep_ms(50);
        port = srr_serial_from_fd(fds[0], true, &cfg);
    } else {
        printf("Connecting to SRR Parallax SID device at '%s' @ %u baud...\n", port_path, cfg.baud_rate);
        port = srr_serial_open(port_path, &cfg);
    }

    if (!port) {
        fprintf(stderr, "Failed to connect to SID device.\n");
        if (sim_pid > 0) {
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, NULL, 0);
        }
        return 1;
    }

    if (strcmp(cmd, "reset-all") == 0) {
        sid_reset_all(port);
        printf("[OK] All 6 SID chips (18 voices) silenced.\n");
    } else if (strcmp(cmd, "reset") == 0) {
        sid_reset(port, target_chip);
        printf("[OK] SID #%u reset completed.\n", target_chip);
    } else if (strcmp(cmd, "read") == 0) {
        uint8_t regs[32];
        if (sid_read_regs(port, target_chip, regs, sizeof(regs)) == 0) {
            sid_print_status(target_chip, regs);
        }
    } else if (strcmp(cmd, "stereo") == 0) {
        run_stereo_demo(port);
    } else if (strcmp(cmd, "poly") == 0) {
        run_poly_demo(port);
    } else {
        /* Default demo runs Stereo + Polyphony */
        run_stereo_demo(port);
        sleep_ms(200);
        run_poly_demo(port);
    }

    srr_serial_close(port);

    if (sim_pid > 0) {
        kill(sim_pid, SIGTERM);
        waitpid(sim_pid, NULL, 0);
    }
    return 0;
}
