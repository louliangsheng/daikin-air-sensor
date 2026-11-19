/**
 * @file miot_device.h
 * @brief 设备层组合 API —— 加密通道 + ag2 传输 + spec 分派 + 属性表
 *
 * 典型用法（绑定/认证完成后）：
 * @code
 *   miot_device_t dev;
 *   miot_device_init(&dev, &port);
 *   miot_device_attach(&dev, props_tbl, n, &my_get, &my_set, user);
 *   miot_device_set_session_keys(&dev, keys);
 *   // GATT 写回调：
 *   miot_device_on_rx(&dev, data, len);
 *   // 测量完成时：
 *   miot_device_send_event(&dev, 8, 2, args, 9);
 * @endcode
 */
#ifndef MIOT_DEVICE_H
#define MIOT_DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include "miot_port.h"
#include "miot_spec.h"
#include "miot_props.h"
#include "miot_ag2.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 会话密钥块：40 字节，取自 LOGIN 派生 E(64B) 的前 40B：
 *  Ekey[0:16]=设备加密 key，Dkey[16:32]=设备解密 key，
 *  ivTx[32:36]=设备→App iv，ivRx[36:40]=App→设备 iv */
#define MIOT_KEYS_LEN 40

/** 自定义 get/set 兜底回调（属性表未命中的 siid/piid） */
typedef bool (*miot_get_fn)(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out);
typedef bool (*miot_set_fn)(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in);

typedef struct miot_device {
    /* 会话状态 */
    uint8_t  keys[MIOT_KEYS_LEN];
    uint16_t tx_seq;
    uint16_t tid;
    int      session_ok;

    /* ag2 传输层 */
    miot_ag2_t ag2;

    /* 属性表 + 兜底回调 */
    miot_prop_t  *props;
    size_t        prop_cnt;
    miot_get_fn   on_get;
    miot_set_fn   on_set;
    void         *user;

    /* 平台接口 */
    miot_port_t  *port;
} miot_device_t;

void miot_device_init(miot_device_t *d, miot_port_t *port);

/** 绑定属性表与兜底回调（BLE 启动前调用） */
void miot_device_attach(miot_device_t *d, miot_prop_t *tbl, size_t cnt,
                        miot_get_fn get, miot_set_fn set, void *user);

/** 认证完成后注入会话密钥（40B，内部复制） */
void miot_device_set_session_keys(miot_device_t *d, const uint8_t keys[MIOT_KEYS_LEN]);

/** 清除会话（断连时调用） */
void miot_device_session_reset(miot_device_t *d);

/** 通道写入口：GATT 写回调直接透传（内部走 ag2 → 解密 → 分派） */
void miot_device_on_rx(miot_device_t *d, const uint8_t *data, size_t len);

/** 明文 spec 请求分派入口（解密后 / 自测模式共用，也用于 host 单元测试） */
void miot_device_dispatch_plain(miot_device_t *d, const uint8_t *pt, size_t len);

/* ---- 主动上报 / 应答（加密 + ag2 分帧 + notify） ---- */

int miot_device_send_event(miot_device_t *d, uint8_t siid, uint16_t eiid,
                           const miot_spec_value_t *args, size_t argc);
int miot_device_send_props_changed(miot_device_t *d,
                                   const miot_spec_value_t *props, size_t count);
int miot_device_send_action_reply(miot_device_t *d, uint16_t tid, uint16_t resp_code);
int miot_device_send_version_reply(miot_device_t *d, uint16_t tid);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_DEVICE_H */
