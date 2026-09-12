#include "captive_dns.h"

#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lwip/sockets.h"

static const char *TAG = "captive_dns";

#define DNS_PORT        53
#define DNS_MAX_PKT     512
#define DNS_TTL_S       60
#define DNS_TYPE_A      1
#define DNS_CLASS_IN    1
#define TASK_STACK      3072
#define TASK_PRIO       4

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static struct {
    TaskHandle_t volatile task;
    volatile bool running;
    esp_ip4_addr_t answer;
} s;

/** Returns length of the encoded QNAME (including the terminating zero) or -1 if malformed. */
static int qname_len(const uint8_t *p, int avail)
{
    int i = 0;
    while (i < avail) {
        uint8_t label = p[i];
        if (label == 0) {
            return i + 1;
        }
        if ((label & 0xC0) != 0) { /* Compression is not expected in a query. */
            return -1;
        }
        i += 1 + label;
    }
    return -1;
}

static int build_response(uint8_t *buf, int len)
{
    if (len < (int)sizeof(dns_header_t)) {
        return -1;
    }
    dns_header_t *hdr = (dns_header_t *)buf;
    uint16_t flags = ntohs(hdr->flags);

    /* Only standard queries (QR=0, OPCODE=0) with exactly one question. */
    if ((flags & 0x8000) != 0 || (flags & 0x7800) != 0 || ntohs(hdr->qdcount) != 1) {
        return -1;
    }

    const int qoff = sizeof(dns_header_t);
    int nlen = qname_len(buf + qoff, len - qoff);
    if (nlen < 0 || qoff + nlen + 4 > len) {
        return -1;
    }
    uint16_t qtype = (uint16_t)((buf[qoff + nlen] << 8) | buf[qoff + nlen + 1]);
    int resp_len = qoff + nlen + 4;

    /* Response header: QR=1, AA=1, RD copied, RCODE=0. */
    hdr->flags = htons(0x8400 | (flags & 0x0100));
    hdr->nscount = 0;
    hdr->arcount = 0;

    if (qtype != DNS_TYPE_A || resp_len + 16 > DNS_MAX_PKT) {
        hdr->ancount = 0;
        return resp_len;
    }

    uint8_t *a = buf + resp_len;
    a[0] = 0xC0; /* Pointer to the QNAME at offset 12. */
    a[1] = 0x0C;
    a[2] = 0x00; a[3] = DNS_TYPE_A;
    a[4] = 0x00; a[5] = DNS_CLASS_IN;
    a[6] = (DNS_TTL_S >> 24) & 0xFF;
    a[7] = (DNS_TTL_S >> 16) & 0xFF;
    a[8] = (DNS_TTL_S >> 8) & 0xFF;
    a[9] = DNS_TTL_S & 0xFF;
    a[10] = 0x00; a[11] = 0x04;
    memcpy(&a[12], &s.answer.addr, 4); /* Already in network byte order. */
    hdr->ancount = htons(1);
    return resp_len + 16;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_MAX_PKT];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        goto done;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: errno %d", errno);
        goto done;
    }

    /* Short receive timeout so the stop flag is honoured promptly. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "listening on UDP/%d", DNS_PORT);
    while (s.running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        int resp = build_response(buf, len);
        if (resp > 0) {
            sendto(sock, buf, resp, 0, (struct sockaddr *)&from, from_len);
        }
    }

done:
    if (sock >= 0) {
        close(sock);
    }
    ESP_LOGI(TAG, "stopped");
    s.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(esp_ip4_addr_t answer_ip)
{
    if (s.task != NULL) {
        s.answer = answer_ip;
        return ESP_OK;
    }
    s.answer = answer_ip;
    s.running = true;
    BaseType_t ok = xTaskCreate(dns_task, "captive_dns", TASK_STACK, NULL, TASK_PRIO, &s.task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task alloc failed");
    return ESP_OK;
}

void captive_dns_stop(void)
{
    s.running = false;
    /* The task exits on its own after the receive timeout; wait for it so a
     * subsequent start() cannot race with the dying instance. */
    for (int i = 0; i < 20 && s.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
