#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include "srr/srr.h"

#define SYNTH_SERVICE_NAME "srr_synth"
#define MAX_VOICES 8

typedef struct {
    bool active;
    int  note;
    int  velocity;
} voice_t;

typedef struct {
    float   cutoff_hz;
    float   resonance;
    float   master_volume;
    uint64_t clock_ticks;
    voice_t voices[MAX_VOICES];
} synth_state_t;

static volatile sig_atomic_t g_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv) {
    const char *service_name = (argc > 1) ? argv[1] : SYNTH_SERVICE_NAME;

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    printf("=================================================================\n");
    printf(" SRR Synthesizer Engine Control Server\n");
    printf(" Listening on service: '%s'\n", service_name);
    printf("=================================================================\n");

    srr_channel_options_t opts = {
        .queue_capacity = 64,
        .max_request_size = 1024,
        .max_reply_size = 1024,
        .service_name = service_name
    };

    srr_channel_t *chan = NULL;
    srr_status_t st = srr_channel_create(&opts, &chan);
    if (st != SRR_OK) {
        fprintf(stderr, "Failed to create channel on '%s': %s\n", service_name, srr_status_str(st));
        return 1;
    }

    synth_state_t synth;
    memset(&synth, 0, sizeof(synth));
    synth.cutoff_hz = 1000.0f;
    synth.resonance = 0.5f;
    synth.master_volume = 0.8f;

    printf("[Synth Server] Ready for client commands and clock pulses.\n");

    while (g_running) {
        srr_request_t *req = NULL;
        st = srr_receive(chan, &req);
        if (st != SRR_OK) {
            break; /* Channel closed or error */
        }

        /* 1. Check for one-way Pulses (e.g. MIDI Clock / Heartbeat) */
        if (srr_request_is_pulse(req)) {
            uint32_t code = srr_pulse_code(req);
            uint64_t val = srr_pulse_value(req);
            if (code == 1) {
                synth.clock_ticks += (val > 0 ? val : 1);
                printf("[PULSE] MIDI Clock Tick received (total: %lu)\n", (unsigned long)synth.clock_ticks);
            } else {
                printf("[PULSE] Notification code=%u value=%lu\n", code, (unsigned long)val);
            }
            srr_request_dispose(req);
            continue;
        }

        /* 2. Process Synchronous Request */
        const char *cmd = (const char *)srr_request_data(req);
        size_t cmd_len = srr_request_size(req);

        char req_buf[256];
        size_t copy_len = cmd_len < sizeof(req_buf) - 1 ? cmd_len : sizeof(req_buf) - 1;
        memcpy(req_buf, cmd, copy_len);
        req_buf[copy_len] = '\0';

        char reply[512] = {0};

        if (strncmp(req_buf, "SET ", 4) == 0) {
            char param[32];
            float val = 0.0f;
            if (sscanf(req_buf + 4, "%31s %f", param, &val) == 2) {
                if (strcmp(param, "cutoff") == 0) {
                    synth.cutoff_hz = val;
                    snprintf(reply, sizeof(reply), "OK cutoff=%.1f Hz", synth.cutoff_hz);
                } else if (strcmp(param, "resonance") == 0) {
                    synth.resonance = val;
                    snprintf(reply, sizeof(reply), "OK resonance=%.2f", synth.resonance);
                } else if (strcmp(param, "volume") == 0) {
                    synth.master_volume = val;
                    snprintf(reply, sizeof(reply), "OK volume=%.2f", synth.master_volume);
                } else {
                    snprintf(reply, sizeof(reply), "ERR unknown_param '%s'", param);
                }
            } else {
                snprintf(reply, sizeof(reply), "ERR invalid_set_syntax");
            }
        } else if (strncmp(req_buf, "GET ", 4) == 0) {
            char param[32];
            if (sscanf(req_buf + 4, "%31s", param) == 1) {
                if (strcmp(param, "cutoff") == 0) {
                    snprintf(reply, sizeof(reply), "VAL cutoff=%.1f", synth.cutoff_hz);
                } else if (strcmp(param, "resonance") == 0) {
                    snprintf(reply, sizeof(reply), "VAL resonance=%.2f", synth.resonance);
                } else if (strcmp(param, "volume") == 0) {
                    snprintf(reply, sizeof(reply), "VAL volume=%.2f", synth.master_volume);
                } else {
                    snprintf(reply, sizeof(reply), "ERR unknown_param '%s'", param);
                }
            } else {
                snprintf(reply, sizeof(reply), "ERR invalid_get_syntax");
            }
        } else if (strncmp(req_buf, "NOTE_ON ", 8) == 0) {
            int note = 0, vel = 0;
            if (sscanf(req_buf + 8, "%d %d", &note, &vel) == 2) {
                int assigned_voice = -1;
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (!synth.voices[i].active) {
                        synth.voices[i].active = true;
                        synth.voices[i].note = note;
                        synth.voices[i].velocity = vel;
                        assigned_voice = i;
                        break;
                    }
                }
                if (assigned_voice >= 0) {
                    snprintf(reply, sizeof(reply), "VOICE_ON voice=%d note=%d vel=%d", assigned_voice, note, vel);
                } else {
                    snprintf(reply, sizeof(reply), "ERR polyphony_exhausted");
                }
            } else {
                snprintf(reply, sizeof(reply), "ERR invalid_note_on_syntax");
            }
        } else if (strncmp(req_buf, "NOTE_OFF ", 9) == 0) {
            int note = 0;
            if (sscanf(req_buf + 9, "%d", &note) == 1) {
                int released_voice = -1;
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (synth.voices[i].active && synth.voices[i].note == note) {
                        synth.voices[i].active = false;
                        released_voice = i;
                        break;
                    }
                }
                if (released_voice >= 0) {
                    snprintf(reply, sizeof(reply), "VOICE_OFF voice=%d note=%d", released_voice, note);
                } else {
                    snprintf(reply, sizeof(reply), "WARN note_not_found note=%d", note);
                }
            } else {
                snprintf(reply, sizeof(reply), "ERR invalid_note_off_syntax");
            }
        } else if (strcmp(req_buf, "STATUS") == 0) {
            int active_count = 0;
            for (int i = 0; i < MAX_VOICES; i++) {
                if (synth.voices[i].active) active_count++;
            }
            snprintf(reply, sizeof(reply),
                     "STATUS active_voices=%d/%d cutoff=%.1f res=%.2f vol=%.2f ticks=%lu",
                     active_count, MAX_VOICES, synth.cutoff_hz, synth.resonance,
                     synth.master_volume, (unsigned long)synth.clock_ticks);
        } else if (strcmp(req_buf, "SHUTDOWN") == 0) {
            snprintf(reply, sizeof(reply), "BYE");
            g_running = 0;
        } else {
            snprintf(reply, sizeof(reply), "ERR unknown_command");
        }

        printf("[REQ] '%s' -> [REPLY] '%s'\n", req_buf, reply);
        srr_reply(req, reply, strlen(reply) + 1);
    }

    printf("[Synth Server] Shutting down channel...\n");
    srr_channel_destroy(chan);
    printf("[Synth Server] Exited cleanly.\n");
    return 0;
}
