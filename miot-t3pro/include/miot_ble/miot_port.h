/**
 * @file miot_port.h
 * @brief 移植层接口 —— 平台适配所需的全部回调
 *
 * SDK 核心不依赖任何 OS / BLE 协议栈 / 加密库。
 * 移植到新平台只需：
 *   1. 实现 miot_port_t 中的发送/时间/随机数回调
 *   2. 实现 miot_crypto_ops_t 中的 HMAC-SHA256 / AES-CCM（参考 port/esp32）
 *   3. 在 GATT 写回调里调用 miot_device_on_rx()，notify 句柄由实现方管理
 */
#ifndef MIOT_PORT_H
#define MIOT_PORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 日志级别 */
#define MIOT_LOG_ERROR 0
#define MIOT_LOG_WARN  1
#define MIOT_LOG_INFO  2

/**
 * 平台回调集合。所有指针可为 NULL（对应功能自动降级）。
 */
typedef struct miot_port {
    /**
     * 发送一帧 GATT notify（spec 通道 / 认证通道由上层选择句柄）。
     * @return 0 成功，非 0 失败
     */
    int (*send_notify)(void *user, const uint8_t *data, size_t len);

    /** 当前 Unix 时间戳（秒）。NULL 时事件时间戳填 0 */
    uint32_t (*time_now)(void *user);

    /** 真随机数（标准认证 ECDH 密钥生成用）。NULL 时退化为伪随机 */
    int (*random)(void *user, uint8_t *out, size_t len);

    /** 日志输出（printf 风格）。NULL 时静默 */
    void (*log)(void *user, int level, const char *tag, const char *fmt, ...);

    /** 透传给各回调的用户上下文 */
    void *user;
} miot_port_t;

#ifdef __cplusplus
}
#endif
#endif /* MIOT_PORT_H */
