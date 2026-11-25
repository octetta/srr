#define _POSIX_C_SOURCE 200809L
#include "srr_serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

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

static void run_pico_demo(srr_serial_port_t *port) {
    char reply_buf[256];
    size_t actual_len = 0;

    printf("\n=== 1. Querying Device Info (device.info) ===\n");
    int rc = srr_serial_request(
        port, SVC_DEVICE, OP_INFO,
        NULL, 0,
        reply_buf, sizeof(reply_buf) - 1,
        &actual_len, 2000
    );
    if (rc == 0) {
        reply_buf[actual_len] = '\0';
        printf("[OK] Device Response: %s\n", reply_buf);
    } else {
        printf("[FAIL] device.info failed with code %d\n", rc);
    }

    printf("\n=== 2. GPIO Write and Read (Onboard LED Pin 25) ===\n");
    uint8_t write_payload[2] = { 25, 1 }; /* Pin 25 -> HIGH */
    rc = srr_serial_request(
        port, SVC_GPIO, OP_WRITE,
        write_payload, sizeof(write_payload),
        reply_buf, sizeof(reply_buf) - 1,
        &actual_len, 2000
    );
    if (rc == 0) {
        reply_buf[actual_len] = '\0';
        printf("[OK] GPIO Write (Pin 25 = HIGH): %s\n", reply_buf);
    } else {
        printf("[FAIL] gpio.write failed with code %d\n", rc);
    }

    uint8_t read_payload[1] = { 25 };
    rc = srr_serial_request(
        port, SVC_GPIO, OP_READ,
        read_payload, sizeof(read_payload),
        reply_buf, sizeof(reply_buf) - 1,
        &actual_len, 2000
    );
    if (rc == 0 && actual_len >= 2) {
        printf("[OK] GPIO Read (Pin %u): State = %u\n", (uint8_t)reply_buf[0], (uint8_t)reply_buf[1]);
    } else {
        printf("[FAIL] gpio.read failed with code %d\n", rc);
    }

    printf("\n=== 3. ADC Read (Internal Temperature Sensor) ===\n");
    rc = srr_serial_request(
        port, SVC_ADC, OP_ADC_READ,
        NULL, 0,
        reply_buf, sizeof(reply_buf) - 1,
        &actual_len, 2000
    );
    if (rc == 0 && actual_len >= 2) {
        uint16_t raw_adc = (uint8_t)reply_buf[0] | ((uint8_t)reply_buf[1] << 8);
        printf("[OK] ADC Raw Reading: %u (~27.0 C)\n", raw_adc);
    } else {
        printf("[FAIL] adc.read failed with code %d\n", rc);
    }

    printf("\n=== 4. Counter Service ===\n");
    rc = srr_serial_request(
        port, SVC_COUNTER, OP_GET,
        NULL, 0,
        reply_buf, sizeof(reply_buf) - 1,
        &actual_len, 2000
    );
    if (rc == 0 && actual_len >= 4) {
        uint32_t cnt = (uint8_t)reply_buf[0] | ((uint8_t)reply_buf[1] << 8) |
                       ((uint8_t)reply_buf[2] << 16) | ((uint8_t)reply_buf[3] << 24);
        printf("[OK] Counter Value: %u\n", cnt);
    }

    rc = srr_serial_request(
        port, SVC_COUNTER, OP_RESET,
        NULL, 0,
        reply_buf, sizeof(reply_buf) - 1,
        &actual_len, 2000
    );
    if (rc == 0) {
        reply_buf[actual_len] = '\0';
        printf("[OK] Counter Reset: %s\n", reply_buf);
    }

    printf("\n=== 5. Pulse Test ===\n");
    rc = srr_serial_pulse(port, 10, 1, "HOST_HEARTBEAT", 14);
    if (rc == 0) {
        printf("[OK] Pulse sent to Pico successfully\n");
    }

    printf("\n=== 6. Timeout Handling ===\n");
    rc = srr_serial_request(
        port, 999, 999, /* Unknown service */
        NULL, 0,
        reply_buf, sizeof(reply_buf) - 1,
        &actual_len, 200
    );
    if (rc == -3) {
        printf("[OK] Target correctly returned SRR_WIRE_ERROR (-3)\n");
    } else {
        printf("Response code: %d\n", rc);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("Usage:\n");
        printf("  %s --sim               Run against integrated simulated Pico\n", argv[0]);
        printf("  %s /dev/ttyACM0        Run against real physical Raspberry Pi Pico\n", argv[0]);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--sim") != 0) {
        /* Physical serial port */
        const char *dev = argv[1];
        printf("Connecting to physical device at %s (115200 baud)...\n", dev);
        srr_serial_config_t cfg;
        srr_serial_default_config(&cfg);
        srr_serial_port_t *port = srr_serial_open(dev, &cfg);
        if (!port) {
            fprintf(stderr, "Error: Could not open serial port %s\n", dev);
            return 1;
        }
        run_pico_demo(port);
        srr_serial_close(port);
        return 0;
    }

    /* Simulation mode: spawn pico_device_sim connected via socketpair */
    printf("Starting Pico Simulation Demo...\n");
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: redirect stdin/stdout to fds[1] */
        close(fds[0]);
        dup2(fds[1], STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);

        execl("examples/pico/pico_device_sim", "pico_device_sim", (char *)NULL);
        perror("execl");
        exit(1);
    }

    /* Parent: wrap fds[0] */
    close(fds[1]);
    srr_serial_port_t *port = srr_serial_from_fd(fds[0], true, NULL);
    if (!port) {
        fprintf(stderr, "Failed to wrap serial fd\n");
        return 1;
    }

    run_pico_demo(port);

    srr_serial_close(port);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("\nPico Demonstration Finished Successfully!\n");
    return 0;
}
