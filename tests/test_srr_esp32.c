#define _POSIX_C_SOURCE 200809L
#include "srr_esp32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SVC_ESP_DEMO 1
#define OP_ECHO      1

static int esp_demo_service(
    uint16_t service_id,
    uint16_t operation_id,
    const uint8_t *req_payload,
    size_t req_len,
    uint8_t *reply_buf,
    size_t reply_cap,
    size_t *out_reply_len,
    void *user_data)
{
    (void)service_id; (void)user_data;
    if (operation_id == OP_ECHO) {
        if (reply_cap < req_len) return -1;
        memcpy(reply_buf, req_payload, req_len);
        *out_reply_len = req_len;
        return 0;
    }
    return -1;
}

static void test_esp32_lifecycle_and_pulse(void) {
    printf("Testing ESP32 FreeRTOS adapter lifecycle and pulse...\n");

    srr_esp32_config_t cfg;
    srr_esp32_default_config(&cfg);

    srr_esp32_device_t *dev = NULL;
    int rc = srr_esp32_create(&cfg, &dev);
    assert(rc == 0);
    assert(dev != NULL);

    rc = srr_esp32_register_service(dev, SVC_ESP_DEMO, esp_demo_service, NULL);
    assert(rc == 0);

    /* Test pulse */
    rc = srr_esp32_pulse(dev, SVC_ESP_DEMO, 5, "ESP_PULSE", 9);
    assert(rc == 0);

    srr_esp32_destroy(dev);
    printf("  ESP32 lifecycle and pulse verified.\n");
}

int main(void) {
    printf("=========================================\n");
    printf("SRR ESP32 Adapter Unit Test Suite\n");
    printf("=========================================\n");

    test_esp32_lifecycle_and_pulse();

    printf("\nAll ESP32 Adapter tests PASSED!\n");
    return 0;
}
