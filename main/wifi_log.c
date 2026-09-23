/*
 * wifi_log.c - see wifi_log.h. Ported from FPGA-Companion/src/esp32/wifi_log.c,
 * trimmed to just the UDP forwarder -- the loader already has its own WiFi
 * bring-up (wifi_init.c) and status LED (main.c's solid purple), so this
 * module doesn't duplicate either.
 */

#include <string.h>
#include <stdio.h>

#include <reent.h>  /* struct _reent for __wrap__write_r */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"

#include "lwip/sockets.h"

#include "wifi_log.h"

#define WIFI_LOG_UDP_PORT       7777
#define WIFI_LOG_UDP_CHUNK      1400  /* stay under Ethernet MTU */
#define WIFI_LOG_RING_BUF_BYTES 8192  /* pre-connect backlog */

static const char *TAG = "wifi_log";

static int                s_udp_sock    = -1;
static struct sockaddr_in s_dest_addr;
static RingbufHandle_t    s_ringbuf     = NULL;
static TaskHandle_t       s_sender_task = NULL;

/* Drains the ring buffer and sends UDP packets in its own task context so
 * __wrap__write_r never blocks on network I/O. Wakes on notification from
 * the hook, or polls every 50 ms to catch any missed notifications. */
static void udp_sender_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        if (s_udp_sock < 0 || !s_ringbuf) continue;

        size_t   item_size;
        uint8_t *item;
        while ((item = (uint8_t *)xRingbufferReceiveUpTo(
                        s_ringbuf, &item_size, 0, WIFI_LOG_UDP_CHUNK)) != NULL) {
            sendto(s_udp_sock, item, item_size, 0,
                   (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
            vRingbufferReturnItem(s_ringbuf, item);
        }
    }
}

/* Intercepts every printf()/ESP_LOGx() byte written to stdout/stderr and
 * queues it into the ring buffer non-blocking. CMakeLists.txt adds
 * -Wl,--wrap=_write_r for this. */
extern ssize_t __real__write_r(struct _reent *r, int fd, const void *buf, size_t nbytes);

ssize_t __wrap__write_r(struct _reent *r, int fd, const void *buf, size_t nbytes)
{
    ssize_t ret = __real__write_r(r, fd, buf, nbytes);

    if ((fd == 1 || fd == 2) && nbytes > 0 && s_ringbuf
            && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        if (xRingbufferSend(s_ringbuf, buf, nbytes, 0) == pdTRUE && s_sender_task) {
            xTaskNotifyGive(s_sender_task);
        }
    }
    return ret;
}

void wifi_log_early_init(void)
{
    s_ringbuf = xRingbufferCreate(WIFI_LOG_RING_BUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    xTaskCreate(udp_sender_task, "udp_log", 4096, NULL, 5, &s_sender_task);
}

void wifi_log_start(void)
{
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return;
    }

    int broadcast = 1;
    setsockopt(s_udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family      = AF_INET;
    s_dest_addr.sin_port        = htons(WIFI_LOG_UDP_PORT);
    s_dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    /* Nudge the sender task to flush the pre-connect backlog immediately. */
    if (s_sender_task) xTaskNotifyGive(s_sender_task);

    ESP_LOGI(TAG, "WiFi UDP logging active - listen with: ncat -u -l %d  (or nc -u -l -p %d)",
             WIFI_LOG_UDP_PORT, WIFI_LOG_UDP_PORT);
}
