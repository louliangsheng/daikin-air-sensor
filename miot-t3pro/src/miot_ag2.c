/**
 * @file miot_ag2.c
 * @brief ag2 可靠传输层实现
 */
#include "miot_ble/miot_ag2.h"
#include <string.h>

void miot_ag2_init(miot_ag2_t *a)
{
    memset(a->rx, 0, sizeof(a->rx));
    a->rx_len = 0;
    a->rx_expect = 0;
    a->rx_got = 0;
    a->send = NULL;
    a->on_frame = NULL;
    a->user = NULL;
}

/* ACK 必须在数据通道上 notify（App 只在写通道收 ACK，错通道直接丢弃） */
static void ag_ack(miot_ag2_t *a, uint8_t status)
{
    uint8_t f[4] = {0, 0, 1, status};
    if (a->send) a->send(a->user, f, 4);
}

void miot_ag2_rx(miot_ag2_t *a, const uint8_t *d, size_t len)
{
    if (len >= 4 && d[0] == 0 && d[1] == 0) {
        uint8_t type = d[2];
        if (type == 0 && len >= 6) {                 /* START */
            a->rx_expect = d[4];
            a->rx_got = 0;
            a->rx_len = 0;
            ag_ack(a, 1);                            /* 就绪 */
            return;
        }
        if (type == 1 || type == 3 || type == 5) {   /* App 对我们数据的确认 */
            return;
        }
        if (type == 2 && len >= 4) {                 /* SINGLE：整帧一次到达 */
            a->rx_expect = 1;
            a->rx_got = 1;
            a->rx_len = len - 4;
            memcpy(a->rx, d + 4, a->rx_len);
            ag_ack(a, 0);
            /* 单帧即完整一帧：这里必须直接上交，不能落到下面的
             * DATA 分支判断再 return —— 那样 k61 帧永远到不了解密层，
             * 表现为 App 发了 get_properties，设备一声不响。 */
            a->rx_expect = 0;
            a->rx_got = 0;
            if (a->on_frame) a->on_frame(a->user, a->rx, a->rx_len);
        }
        return;
    }

    if (a->rx_expect > 0 && len >= 2) {              /* DATA 帧 [seq16][chunk] */
        uint16_t fseq = (uint16_t)(d[0] | (d[1] << 8));
        if (fseq < 1 || fseq > a->rx_expect ||
            a->rx_len + (len - 2) > sizeof(a->rx)) {
            return;
        }
        memcpy(a->rx + a->rx_len, d + 2, len - 2);
        a->rx_len += len - 2;
        a->rx_got++;
        if (a->rx_got >= a->rx_expect) {
            ag_ack(a, 0);                            /* 收齐才确认 */
        }
    } else {
        return;                                      /* stray */
    }

    if (a->rx_expect == 0 || a->rx_got < a->rx_expect) return;

    /* 重组完成 → 交给上层（k61 解密在 miot_device 里做） */
    a->rx_expect = 0;
    if (a->on_frame) a->on_frame(a->user, a->rx, a->rx_len);
}

int miot_ag2_send(miot_ag2_t *a, const uint8_t *k, size_t n)
{
    if (!a->send) return -1;
    uint16_t cnt = (uint16_t)((n + MIOT_AG2_TX_CHUNK - 1) / MIOT_AG2_TX_CHUNK);
    if (cnt < 1) cnt = 1;

    uint8_t start[6] = {0, 0, 0, 0, (uint8_t)(cnt & 0xFF), (uint8_t)(cnt >> 8)};
    a->send(a->user, start, 6);

    size_t off = 0;
    for (uint16_t i = 1; i <= cnt; i++) {
        size_t chunk = n - off;
        if (chunk > MIOT_AG2_TX_CHUNK) chunk = MIOT_AG2_TX_CHUNK;
        uint8_t f[2 + MIOT_AG2_TX_CHUNK];
        f[0] = (uint8_t)(i & 0xFF);
        f[1] = (uint8_t)(i >> 8);
        memcpy(f + 2, k + off, chunk);
        a->send(a->user, f, 2 + chunk);
        off += chunk;
    }
    return 0;
}
