#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "srr_esp32.h"
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"

struct srr_esp32_device {
    srr_esp32_config_t config;
    TaskHandle_t worker_task;
    SemaphoreHandle_t tx_mutex;
    SemaphoreHandle_t trans_mutex;
    SemaphoreHandle_t reply_sem;

    srr_endpoint_t ep;
    uint8_t *rx_buf;
    uint8_t *dec_buf;
    uint8_t *tx_buf;
    uint8_t *rep_buf;

    uint64_t waiting_tid;
    void *waiting_reply_buf;
    size_t waiting_reply_cap;
    size_t waiting_actual_len;
    int waiting_error;
    bool running;
};

static int esp_uart_tx(const uint8_t *data, size_t len, void *user_data) {
    srr_esp32_device_t *dev = (srr_esp32_device_t *)user_data;
    return uart_write_bytes(dev->config.uart_port, (const char *)data, len) == (int)len ? 0 : -1;
}

static void esp_uart_worker_task(void *pvParameters) {
    srr_esp32_device_t *dev = (srr_esp32_device_t *)pvParameters;
    uint8_t ch;
    while (dev->running) {
        int len = uart_read_bytes(dev->config.uart_port, &ch, 1, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            xSemaphoreTake(dev->tx_mutex, portMAX_DELAY);
            xSemaphoreTake(dev->trans_mutex, portMAX_DELAY);
            srr_parser_status_t st = srr_endpoint_feed_byte(&dev->ep, ch);
            if (st == SRR_PARSER_FRAME_READY && dev->waiting_tid > 0) {
                size_t act_len = 0;
                int err = 0;
                int rc = srr_collect_reply(&dev->ep, dev->waiting_tid, &act_len, &err);
                if (rc == 0 || rc == -1) {
                    dev->waiting_actual_len = act_len;
                    dev->waiting_error = (rc == 0) ? 0 : -3;
                    xSemaphoreGive(dev->reply_sem);
                }
            }
            xSemaphoreGive(dev->trans_mutex);
            xSemaphoreGive(dev->tx_mutex);
        }
    }
    vTaskDelete(NULL);
}

void srr_esp32_default_config(srr_esp32_config_t *cfg) {
    if (!cfg) return;
    cfg->uart_port = 1;
    cfg->tx_pin = 17;
    cfg->rx_pin = 16;
    cfg->baud_rate = 115200;
    cfg->task_stack_size = 4096;
    cfg->task_priority = 5;
    cfg->rx_buffer_size = 2048;
    cfg->tx_buffer_size = 2048;
}

int srr_esp32_create(const srr_esp32_config_t *config, srr_esp32_device_t **out_dev) {
    if (!out_dev) return -1;
    srr_esp32_device_t *dev = (srr_esp32_device_t *)calloc(1, sizeof(srr_esp32_device_t));
    if (!dev) return -1;

    if (config) dev->config = *config;
    else srr_esp32_default_config(&dev->config);

    dev->rx_buf = (uint8_t *)malloc(dev->config.rx_buffer_size);
    dev->dec_buf = (uint8_t *)malloc(dev->config.rx_buffer_size);
    dev->tx_buf = (uint8_t *)malloc(dev->config.tx_buffer_size);
    dev->rep_buf = (uint8_t *)malloc(dev->config.tx_buffer_size);

    if (!dev->rx_buf || !dev->dec_buf || !dev->tx_buf || !dev->rep_buf) {
        free(dev->rx_buf);
        free(dev->dec_buf);
        free(dev->tx_buf);
        free(dev->rep_buf);
        free(dev);
        return -1;
    }

    dev->tx_mutex = xSemaphoreCreateMutex();
    dev->trans_mutex = xSemaphoreCreateMutex();
    dev->reply_sem = xSemaphoreCreateBinary();

    if (!dev->tx_mutex || !dev->trans_mutex || !dev->reply_sem) {
        if (dev->tx_mutex) vSemaphoreDelete(dev->tx_mutex);
        if (dev->trans_mutex) vSemaphoreDelete(dev->trans_mutex);
        if (dev->reply_sem) vSemaphoreDelete(dev->reply_sem);
        free(dev->rx_buf);
        free(dev->dec_buf);
        free(dev->tx_buf);
        free(dev->rep_buf);
        free(dev);
        return -1;
    }

    srr_endpoint_init(
        &dev->ep, (uint32_t)(esp_timer_get_time() / 1000),
        esp_uart_tx, dev,
        dev->rx_buf, dev->config.rx_buffer_size,
        dev->dec_buf, dev->config.rx_buffer_size,
        dev->tx_buf, dev->config.tx_buffer_size,
        dev->rep_buf, dev->config.tx_buffer_size
    );

    dev->running = true;
    BaseType_t task_rc = xTaskCreate(esp_uart_worker_task, "srr_uart", dev->config.task_stack_size, dev, dev->config.task_priority, &dev->worker_task);
    if (task_rc != pdPASS) {
        vSemaphoreDelete(dev->tx_mutex);
        vSemaphoreDelete(dev->trans_mutex);
        vSemaphoreDelete(dev->reply_sem);
        free(dev->rx_buf);
        free(dev->dec_buf);
        free(dev->tx_buf);
        free(dev->rep_buf);
        free(dev);
        return -1;
    }

    *out_dev = dev;
    return 0;
}

int srr_esp32_register_service(srr_esp32_device_t *dev, uint16_t service_id, srr_service_handler_t handler, void *user_data) {
    if (!dev) return -1;
    xSemaphoreTake(dev->trans_mutex, portMAX_DELAY);
    int rc = srr_endpoint_register_service(&dev->ep, service_id, handler, user_data);
    xSemaphoreGive(dev->trans_mutex);
    return rc;
}

int srr_esp32_request(
    srr_esp32_device_t *dev,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms)
{
    if (!dev) return -1;

    xSemaphoreTake(dev->tx_mutex, portMAX_DELAY);
    xSemaphoreTake(dev->trans_mutex, portMAX_DELAY);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint64_t tid = srr_begin_request(
        &dev->ep, service_id, operation_id,
        req_payload, req_len,
        (uint8_t *)reply_buf, reply_cap,
        timeout_ms, now_ms
    );
    if (tid == 0) {
        xSemaphoreGive(dev->trans_mutex);
        xSemaphoreGive(dev->tx_mutex);
        return -1;
    }

    dev->waiting_tid = tid;
    dev->waiting_reply_buf = reply_buf;
    dev->waiting_reply_cap = reply_cap;
    dev->waiting_actual_len = 0;
    dev->waiting_error = 0;

    xSemaphoreGive(dev->trans_mutex);
    /* Release TX mutex as soon as wire frame transmission is complete */
    xSemaphoreGive(dev->tx_mutex);

    BaseType_t sem_rc = xSemaphoreTake(dev->reply_sem, pdMS_TO_TICKS(timeout_ms));

    xSemaphoreTake(dev->trans_mutex, portMAX_DELAY);
    dev->waiting_tid = 0;
    int result = -1;
    if (sem_rc == pdTRUE) {
        if (actual_reply_len) *actual_reply_len = dev->waiting_actual_len;
        result = dev->waiting_error;
    } else {
        srr_cancel_transaction(&dev->ep, tid);
        result = -1; /* Timeout */
    }
    xSemaphoreGive(dev->trans_mutex);

    return result;
}

int srr_esp32_pulse(srr_esp32_device_t *dev, uint16_t service_id, uint16_t operation_id, const void *payload, size_t payload_len) {
    if (!dev) return -1;
    xSemaphoreTake(dev->tx_mutex, portMAX_DELAY);
    int rc = srr_endpoint_send_pulse(&dev->ep, service_id, operation_id, payload, payload_len);
    xSemaphoreGive(dev->tx_mutex);
    return rc;
}

void srr_esp32_destroy(srr_esp32_device_t *dev) {
    if (!dev) return;
    dev->running = false;
    if (dev->worker_task) {
        vTaskDelete(dev->worker_task);
        dev->worker_task = NULL;
    }
    vSemaphoreDelete(dev->tx_mutex);
    vSemaphoreDelete(dev->trans_mutex);
    vSemaphoreDelete(dev->reply_sem);
    free(dev->rx_buf);
    free(dev->dec_buf);
    free(dev->tx_buf);
    free(dev->rep_buf);
    free(dev);
}

#else
/*
 * ============================================================================
 * Host / Linux Emulation for ESP32 FreeRTOS (CI & Unit Testing)
 * ============================================================================
 */
#include <pthread.h>
#include <unistd.h>
#include <time.h>

struct srr_esp32_device {
    srr_esp32_config_t config;
    pthread_t worker_th;
    pthread_mutex_t tx_mutex;
    pthread_mutex_t trans_mutex;
    pthread_cond_t reply_cond;

    int pipe_fds[2]; /* Loopback pipe for test */

    srr_endpoint_t ep;
    uint8_t *rx_buf;
    uint8_t *dec_buf;
    uint8_t *tx_buf;
    uint8_t *rep_buf;

    uint64_t waiting_tid;
    void *waiting_reply_buf;
    size_t waiting_reply_cap;
    size_t waiting_actual_len;
    int waiting_error;
    bool reply_arrived;
    bool running;
};

static int host_uart_tx(const uint8_t *data, size_t len, void *user_data) {
    srr_esp32_device_t *dev = (srr_esp32_device_t *)user_data;
    if (dev->pipe_fds[1] >= 0) {
        ssize_t n = write(dev->pipe_fds[1], data, len);
        return n == (ssize_t)len ? 0 : -1;
    }
    return 0;
}

static void* host_uart_worker(void *arg) {
    srr_esp32_device_t *dev = (srr_esp32_device_t *)arg;
    uint8_t ch;
    while (1) {
        pthread_mutex_lock(&dev->trans_mutex);
        bool run = dev->running;
        pthread_mutex_unlock(&dev->trans_mutex);
        if (!run) break;

        ssize_t n = read(dev->pipe_fds[0], &ch, 1);
        if (n <= 0) {
            usleep(1000);
            continue;
        }

        pthread_mutex_lock(&dev->tx_mutex);
        pthread_mutex_lock(&dev->trans_mutex);
        srr_parser_status_t st = srr_endpoint_feed_byte(&dev->ep, ch);
        if (st == SRR_PARSER_FRAME_READY && dev->waiting_tid > 0) {
            size_t act_len = 0;
            int err = 0;
            int rc = srr_collect_reply(&dev->ep, dev->waiting_tid, &act_len, &err);
            if (rc == 0 || rc == -1) {
                dev->waiting_actual_len = act_len;
                dev->waiting_error = (rc == 0) ? 0 : -3;
                dev->reply_arrived = true;
                pthread_cond_signal(&dev->reply_cond);
            }
        }
        pthread_mutex_unlock(&dev->trans_mutex);
        pthread_mutex_unlock(&dev->tx_mutex);
    }
    return NULL;
}

void srr_esp32_default_config(srr_esp32_config_t *cfg) {
    if (!cfg) return;
    cfg->uart_port = 1;
    cfg->tx_pin = 17;
    cfg->rx_pin = 16;
    cfg->baud_rate = 115200;
    cfg->task_stack_size = 4096;
    cfg->task_priority = 5;
    cfg->rx_buffer_size = 2048;
    cfg->tx_buffer_size = 2048;
}

int srr_esp32_create(const srr_esp32_config_t *config, srr_esp32_device_t **out_dev) {
    if (!out_dev) return -1;
    srr_esp32_device_t *dev = (srr_esp32_device_t *)calloc(1, sizeof(srr_esp32_device_t));
    if (!dev) return -1;

    if (config) dev->config = *config;
    else srr_esp32_default_config(&dev->config);

    dev->rx_buf = (uint8_t *)malloc(dev->config.rx_buffer_size);
    dev->dec_buf = (uint8_t *)malloc(dev->config.rx_buffer_size);
    dev->tx_buf = (uint8_t *)malloc(dev->config.tx_buffer_size);
    dev->rep_buf = (uint8_t *)malloc(dev->config.tx_buffer_size);

    if (!dev->rx_buf || !dev->dec_buf || !dev->tx_buf || !dev->rep_buf) {
        free(dev->rx_buf);
        free(dev->dec_buf);
        free(dev->tx_buf);
        free(dev->rep_buf);
        free(dev);
        return -1;
    }

    if (pipe(dev->pipe_fds) != 0) {
        free(dev->rx_buf);
        free(dev->dec_buf);
        free(dev->tx_buf);
        free(dev->rep_buf);
        free(dev);
        return -1;
    }

    pthread_mutex_init(&dev->tx_mutex, NULL);
    pthread_mutex_init(&dev->trans_mutex, NULL);
    pthread_cond_init(&dev->reply_cond, NULL);

    srr_endpoint_init(
        &dev->ep, (uint32_t)time(NULL),
        host_uart_tx, dev,
        dev->rx_buf, dev->config.rx_buffer_size,
        dev->dec_buf, dev->config.rx_buffer_size,
        dev->tx_buf, dev->config.tx_buffer_size,
        dev->rep_buf, dev->config.tx_buffer_size
    );

    dev->running = true;
    if (pthread_create(&dev->worker_th, NULL, host_uart_worker, dev) != 0) {
        close(dev->pipe_fds[0]);
        close(dev->pipe_fds[1]);
        pthread_mutex_destroy(&dev->tx_mutex);
        pthread_mutex_destroy(&dev->trans_mutex);
        pthread_cond_destroy(&dev->reply_cond);
        free(dev->rx_buf);
        free(dev->dec_buf);
        free(dev->tx_buf);
        free(dev->rep_buf);
        free(dev);
        return -1;
    }

    *out_dev = dev;
    return 0;
}

int srr_esp32_register_service(srr_esp32_device_t *dev, uint16_t service_id, srr_service_handler_t handler, void *user_data) {
    if (!dev) return -1;
    pthread_mutex_lock(&dev->trans_mutex);
    int rc = srr_endpoint_register_service(&dev->ep, service_id, handler, user_data);
    pthread_mutex_unlock(&dev->trans_mutex);
    return rc;
}

int srr_esp32_request(
    srr_esp32_device_t *dev,
    uint16_t service_id,
    uint16_t operation_id,
    const void *req_payload,
    size_t req_len,
    void *reply_buf,
    size_t reply_cap,
    size_t *actual_reply_len,
    uint32_t timeout_ms)
{
    if (!dev) return -1;

    pthread_mutex_lock(&dev->tx_mutex);
    pthread_mutex_lock(&dev->trans_mutex);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t now_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    uint64_t tid = srr_begin_request(
        &dev->ep, service_id, operation_id,
        req_payload, req_len,
        (uint8_t *)reply_buf, reply_cap,
        timeout_ms, now_ms
    );
    if (tid == 0) {
        pthread_mutex_unlock(&dev->trans_mutex);
        pthread_mutex_unlock(&dev->tx_mutex);
        return -1;
    }

    dev->waiting_tid = tid;
    dev->waiting_reply_buf = reply_buf;
    dev->waiting_reply_cap = reply_cap;
    dev->waiting_actual_len = 0;
    dev->waiting_error = 0;
    dev->reply_arrived = false;

    /* Release TX mutex as soon as wire frame transmission is complete */
    pthread_mutex_unlock(&dev->tx_mutex);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }

    while (!dev->reply_arrived) {
        int cond_rc = pthread_cond_timedwait(&dev->reply_cond, &dev->trans_mutex, &deadline);
        if (cond_rc != 0) break; /* Timeout */
    }

    int result = -1;
    if (dev->reply_arrived) {
        if (actual_reply_len) *actual_reply_len = dev->waiting_actual_len;
        result = dev->waiting_error;
    } else {
        srr_cancel_transaction(&dev->ep, tid);
        result = -1;
    }

    dev->waiting_tid = 0;
    pthread_mutex_unlock(&dev->trans_mutex);

    return result;
}

int srr_esp32_pulse(srr_esp32_device_t *dev, uint16_t service_id, uint16_t operation_id, const void *payload, size_t payload_len) {
    if (!dev) return -1;
    pthread_mutex_lock(&dev->tx_mutex);
    int rc = srr_endpoint_send_pulse(&dev->ep, service_id, operation_id, payload, payload_len);
    pthread_mutex_unlock(&dev->tx_mutex);
    return rc;
}

void srr_esp32_destroy(srr_esp32_device_t *dev) {
    if (!dev) return;
    pthread_mutex_lock(&dev->trans_mutex);
    dev->running = false;
    pthread_mutex_unlock(&dev->trans_mutex);
    if (dev->pipe_fds[1] >= 0) {
        close(dev->pipe_fds[1]);
        dev->pipe_fds[1] = -1;
    }
    pthread_join(dev->worker_th, NULL);
    if (dev->pipe_fds[0] >= 0) {
        close(dev->pipe_fds[0]);
        dev->pipe_fds[0] = -1;
    }
    pthread_mutex_destroy(&dev->tx_mutex);
    pthread_mutex_destroy(&dev->trans_mutex);
    pthread_cond_destroy(&dev->reply_cond);
    free(dev->rx_buf);
    free(dev->dec_buf);
    free(dev->tx_buf);
    free(dev->rep_buf);
    free(dev);
}

#endif
