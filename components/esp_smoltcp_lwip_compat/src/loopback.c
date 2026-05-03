#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "loopback.h"

static const char *TAG = "lo";

#define LO_PORT_SLOTS 4
#define LO_QUEUE_DEPTH 8

typedef struct {
    uint16_t   port;     /* 0 = slot free */
    fd_entry_t *fd;
} lo_slot_t;

static lo_slot_t          s_slots[LO_PORT_SLOTS];
static SemaphoreHandle_t  s_lock;

static void lock_init_once(void)
{
    static bool inited;
    if (!inited) {
        s_lock = xSemaphoreCreateMutex();
        inited = true;
    }
}

esp_err_t lo_bind(uint16_t port, fd_entry_t *e)
{
    if (!e || !port) return ESP_ERR_INVALID_ARG;
    lock_init_once();

    if (!e->lo_rx) {
        e->lo_rx = xQueueCreate(LO_QUEUE_DEPTH, sizeof(lo_msg_t *));
        if (!e->lo_rx) return ESP_ERR_NO_MEM;
    }
    e->loopback = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int free_slot = -1;
    for (int i = 0; i < LO_PORT_SLOTS; i++) {
        if (s_slots[i].port == port) { s_slots[i].fd = e; xSemaphoreGive(s_lock); return ESP_OK; }
        if (free_slot < 0 && s_slots[i].port == 0) free_slot = i;
    }
    if (free_slot < 0) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    s_slots[free_slot] = (lo_slot_t){ port, e };
    xSemaphoreGive(s_lock);
    ESP_LOGD(TAG, "bind port=%u fd=%p", port, e);
    return ESP_OK;
}

void lo_unbind(uint16_t port, fd_entry_t *e)
{
    lock_init_once();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LO_PORT_SLOTS; i++) {
        if (s_slots[i].port == port && s_slots[i].fd == e) {
            s_slots[i] = (lo_slot_t){ 0, NULL };
        }
    }
    xSemaphoreGive(s_lock);

    /* Drain any pending messages so we don't leak. */
    if (e && e->lo_rx) {
        lo_msg_t *m;
        while (xQueueReceive(e->lo_rx, &m, 0) == pdTRUE) free(m);
    }
}

int lo_deliver(uint16_t dst_port, uint32_t src_be, uint16_t src_port,
               const uint8_t *buf, size_t len)
{
    if (!buf || len > UINT16_MAX) return -1;
    lock_init_once();

    fd_entry_t *target = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LO_PORT_SLOTS; i++) {
        if (s_slots[i].port == dst_port) { target = s_slots[i].fd; break; }
    }
    xSemaphoreGive(s_lock);

    if (!target || !target->lo_rx) return -1;

    lo_msg_t *m = malloc(sizeof(*m) + len);
    if (!m) return -1;
    m->src_be   = src_be;
    m->src_port = src_port;
    m->len      = (uint16_t)len;
    memcpy(m->data, buf, len);

    if (xQueueSend(target->lo_rx, &m, 0) != pdTRUE) {
        free(m);
        return -1;
    }
    return (int)len;
}
