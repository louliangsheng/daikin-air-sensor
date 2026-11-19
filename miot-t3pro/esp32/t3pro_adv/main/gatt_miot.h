/**
 * @file gatt_miot.h
 * @brief 米家 Spec v2 的 GATT 承载层（0xFE95 服务）
 *
 * 认证与 spec 两个状态机都由 SDK 提供，这一层只做三件事：
 *   1. 注册 4 个特征值（0x0010 / 0x0019 / 0x001A / 0x001B）
 *   2. 把 GATT 写事件分发给对应状态机
 *   3. 提供 notify 出口（按通道选特征值句柄）
 *
 * 通道分工（docs/protocol.md §1）：
 *   0x0010  App→设备：0xA4 建链 / 0xA2 注册 / 0x24 登录；设备→App：结果码 0x11/0x21
 *   0x0019  认证数据，ag2 分帧（公钥、devRandom、devConfirm、appConfirm）
 *   0x001A  spec 数据第二通道（App 只订阅 0x0010/0x0019，往这里推页面收不到；
 *           实测保留仅供观察，真正上报走 0x0010）
 *   0x001B  App→设备第二写通道（米家部分版本用它补发）
 */
#ifndef GATT_MIOT_H
#define GATT_MIOT_H

#include <stdint.h>
#include <stddef.h>
#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册服务配置。必须在 nimble_port_init() 之后、host 同步前调用。 */
void gatt_miot_register_svcs(void);

/** 绑定状态机实例。之后所有写事件都投给它们。 */
void gatt_miot_bind(miot_auth_t *auth, miot_device_t *dev);

void gatt_miot_on_connect(uint16_t conn_handle, uint16_t mtu);
void gatt_miot_on_disconnect(void);
void gatt_miot_on_mtu(uint16_t mtu);

/** 打印四个特征的实际 attribute handle（排查 App 订阅到哪个通道用） */
void gatt_miot_dump_handles(void);

/** 当前是否有手机连着（决定要不要继续广播） */
int gatt_miot_is_connected(void);

/** spec 通道 notify 出口，直接塞给 miot_port_t.send_notify */
int gatt_miot_notify_spec(void *user, const uint8_t *d, size_t len);

/** 认证通道 notify 出口，塞给 miot_auth_port_t.notify_0010 / notify_0019 */
int gatt_miot_notify_auth_0010(void *user, const uint8_t *d, size_t len);
int gatt_miot_notify_auth_0019(void *user, const uint8_t *d, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* GATT_MIOT_H */
