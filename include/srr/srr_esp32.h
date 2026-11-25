#ifndef SRR_ESP32_H
#define SRR_ESP32_H

#include "srr_wire.h"
#include "srr_embedded.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SRR ESP32 / FreeRTOS Adapter
 * ============================================================================
 * Provides an RTOS-based execution model on ESP32 without leaking FreeRTOS types
 * into the public portable API (satisfying Kickoff Brief §10).
 *
 * Features:
 *   - Dedicated FreeRTOS background task reading UART byte stream.
 *   - Thread-safe TX mutex so multiple FreeRTOS tasks can send requests/pulses.
 *   - Blocking srr_esp32_request() synchronizing on a task notification / semaphore.
 *   - Full host emulation support under POSIX pthreads for CI testability.
 */

typedef struct srr_esp32_device srr_esp32_device_t;

typedef struct {
    int      uart_port;        /* UART_NUM_0, UART_NUM_1, etc. */
    int      tx_pin;           /* GPIO pin for TX (e.g. 17) */
    int      rx_pin;           /* GPIO pin for RX (e.g. 16) */
    uint32_t baud_rate;        /* e.g. 115200 */
    uint32_t task_stack_size;  /* Background worker stack size (e.g. 4096) */
    uint32_t task_priority;    /* Background worker task priority (e.g. 5) */
    size_t   rx_buffer_size;   /* e.g. 2048 */
    size_t   tx_buffer_size;   /* e.g. 2048 */
} srr_esp32_config_t;

/* Get default ESP32 configuration */
void srr_esp32_default_config(srr_esp32_config_t *cfg);

/*
 * Create and start ESP32 SRR device:
 * Configures UART, creates worker task, and initializes embedded endpoint.
 */
int srr_esp32_create(const srr_esp32_config_t *config, srr_esp32_device_t **out_dev);

/* Register a service request handler */
int srr_esp32_register_service(
    srr_esp32_device_t *dev,
    uint16_t service_id,
    srr_service_handler_t handler,
    void *user_data
);

/*
 * Thread-safe synchronous Request/Reply across FreeRTOS tasks.
 * Blocks calling task until matching reply arrives or timeout expires.
 */
int srr_esp32_request(
    srr_esp32_device_t *dev,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms
);

/*
 * Thread-safe one-way pulse transmission from any FreeRTOS task.
 */
int srr_esp32_pulse(
    srr_esp32_device_t *dev,
    uint16_t service_id,
    uint16_t operation_id,
    const void *payload,
    size_t payload_len
);

/* Stop worker task, release UART, and free device */
void srr_esp32_destroy(srr_esp32_device_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* SRR_ESP32_H */
