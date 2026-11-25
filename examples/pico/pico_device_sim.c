#define _POSIX_C_SOURCE 200809L
#include "srr_embedded.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#define SVC_DEVICE  1
#define OP_INFO     1

#define SVC_GPIO    2
#define OP_READ     1
#define OP_WRITE    2

#define SVC_ADC     3
#define OP_ADC_READ 1

#define SVC_COUNTER 4
#define OP_GET      1
#define OP_RESET    2

static uint8_t  g_gpio_pins[32] = {0};
static uint32_t g_counter = 0;
static volatile sig_atomic_t g_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_running = 0;
}

static int device_handler(
    uint16_t svc, uint16_t op,
    const uint8_t *req, size_t req_len,
    uint8_t *reply, size_t cap, size_t *out_len,
    void *user_data)
{
    (void)svc; (void)req; (void)req_len; (void)user_data;
    if (op == OP_INFO) {
        const char *info = "Raspberry Pi Pico (RP2040) | FW: 1.0.0 | Flash: 2MB | SRAM: 264KB";
        size_t len = strlen(info);
        if (cap < len) return -1;
        memcpy(reply, info, len);
        *out_len = len;
        return 0;
    }
    return -1;
}

static int gpio_handler(
    uint16_t svc, uint16_t op,
    const uint8_t *req, size_t req_len,
    uint8_t *reply, size_t cap, size_t *out_len,
    void *user_data)
{
    (void)svc; (void)user_data;
    if (op == OP_READ) {
        if (req_len < 1) return -1;
        uint8_t pin = req[0];
        if (pin >= 32 || cap < 2) return -1;
        reply[0] = pin;
        reply[1] = g_gpio_pins[pin];
        *out_len = 2;
        return 0;
    } else if (op == OP_WRITE) {
        if (req_len < 2) return -1;
        uint8_t pin = req[0];
        uint8_t val = req[1];
        if (pin >= 32) return -1;
        g_gpio_pins[pin] = val;
        const char *ok = "OK";
        memcpy(reply, ok, 2);
        *out_len = 2;
        return 0;
    }
    return -1;
}

static int adc_handler(
    uint16_t svc, uint16_t op,
    const uint8_t *req, size_t req_len,
    uint8_t *reply, size_t cap, size_t *out_len,
    void *user_data)
{
    (void)svc; (void)req; (void)req_len; (void)user_data;
    if (op == OP_ADC_READ) {
        /* Return simulated RP2040 temperature sensor (~27.0 C = raw 720) */
        uint16_t raw_val = 720;
        if (cap < sizeof(uint16_t)) return -1;
        reply[0] = (uint8_t)(raw_val & 0xFF);
        reply[1] = (uint8_t)((raw_val >> 8) & 0xFF);
        *out_len = 2;
        return 0;
    }
    return -1;
}

static int counter_handler(
    uint16_t svc, uint16_t op,
    const uint8_t *req, size_t req_len,
    uint8_t *reply, size_t cap, size_t *out_len,
    void *user_data)
{
    (void)svc; (void)req; (void)req_len; (void)user_data;
    if (op == OP_GET) {
        g_counter++;
        if (cap < sizeof(uint32_t)) return -1;
        reply[0] = (uint8_t)(g_counter & 0xFF);
        reply[1] = (uint8_t)((g_counter >> 8) & 0xFF);
        reply[2] = (uint8_t)((g_counter >> 16) & 0xFF);
        reply[3] = (uint8_t)((g_counter >> 24) & 0xFF);
        *out_len = 4;
        return 0;
    } else if (op == OP_RESET) {
        g_counter = 0;
        const char *ok = "RESET_OK";
        size_t len = strlen(ok);
        if (cap < len) return -1;
        memcpy(reply, ok, len);
        *out_len = len;
        return 0;
    }
    return -1;
}

static int stdout_tx(const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(STDOUT_FILENO, data + written, len - written);
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    /* Zero heap allocation: all buffers on stack */
    uint8_t rx_buf[1024];
    uint8_t dec_buf[1024];
    uint8_t tx_buf[1024];
    uint8_t rep_buf[1024];

    srr_endpoint_t ep;
    srr_endpoint_init(
        &ep, 0x5049434F, /* "PICO" */
        stdout_tx, NULL,
        rx_buf, sizeof(rx_buf),
        dec_buf, sizeof(dec_buf),
        tx_buf, sizeof(tx_buf),
        rep_buf, sizeof(rep_buf)
    );

    srr_endpoint_register_service(&ep, SVC_DEVICE, device_handler, NULL);
    srr_endpoint_register_service(&ep, SVC_GPIO, gpio_handler, NULL);
    srr_endpoint_register_service(&ep, SVC_ADC, adc_handler, NULL);
    srr_endpoint_register_service(&ep, SVC_COUNTER, counter_handler, NULL);

    /* Main loop: read stdin byte-by-byte and dispatch */
    uint8_t ch;
    while (g_running) {
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        srr_endpoint_feed_byte(&ep, ch);
    }

    return 0;
}
