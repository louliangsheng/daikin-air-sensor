/**
 * @file miot_ag2.h
 * @brief ag2 可靠传输层（spec 通道 0x001A/0x001B 外层分帧协议）
 *
 * 镜像米家 App 端 ag2/zn1/o0ooOOo/pu3/iae 状态机（反汇编实证）：
 *   控制帧: [00 00][type][...]
 *   数据帧: [seq16 LE][payload]（载荷 = 完整 k61 加密帧）
 *   type 0 START:  [00 00][00][pkgType][count LE16] —— 声明随后 count 个数据帧
 *   type 1 ACK:    [00 00][01][status][seq16...]*
 *   type 2 SINGLE: [00 00][02][pkgType][payload]
 *   type 3 SACK:   [00 00][03][code]
 *   type 5 SYNC:   [00 00][05][seq16...]*（丢失帧重发请求）
 *
 * status 语义（认证通道实测）：App START → ACK status=1（就绪）；
 * App 数据收齐 → ACK status=0（确认）。App 只认对应状态才推进状态机。
 * 发送侧每帧最多 18B 载荷（与 App 认证通道分帧一致）。
 */
#ifndef MIOT_AG2_H
#define MIOT_AG2_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIOT_AG2_RX_CAP     512     /* 重组缓冲：160 不够 —— App 的 sync action
                                       (userinfolist 含 string) > 149B 会溢出 */
#define MIOT_AG2_TX_CHUNK   18

typedef struct miot_ag2 {
    /* 内部重组状态 */
    uint8_t  rx[MIOT_AG2_RX_CAP];
    size_t   rx_len;
    uint8_t  rx_expect;
    uint8_t  rx_got;

    /* 配置 */
    void (*send)(void *user, const uint8_t *frame, size_t len);  /* 底层 notify */
    void (*on_frame)(void *user, const uint8_t *k61, size_t n);  /* 重组完成的 k61 帧 */
    void *user;
} miot_ag2_t;

void miot_ag2_init(miot_ag2_t *a);

/** 收到通道写数据（原始帧，未重组） */
void miot_ag2_rx(miot_ag2_t *a, const uint8_t *d, size_t len);

/** 设备→App 发一个 k61 帧：START + DATA 分帧 */
int miot_ag2_send(miot_ag2_t *a, const uint8_t *k, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_AG2_H */
