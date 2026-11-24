/**
 * @file miot_props.h
 * @brief 属性表驱动执行器 —— 把 spec 下行从硬编码 if-else 变成表驱动
 *
 * 注册一张属性表后，get_properties / set_properties 由框架自动处理：
 *   - get → 按声明类型编码应答（未命中项回 result=1）
 *   - set → 类型转换、范围夹取、读写权限校验，写本地状态后回调 on_set
 * 未命中的 (siid,piid) 可由 miot_device_t 的自定义回调兜底。
 */
#ifndef MIOT_PROPS_H
#define MIOT_PROPS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "miot_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIOT_PROP_R   1u    /* 只读 */
#define MIOT_PROP_RW  3u    /* 可写 */

struct miot_prop;
typedef void (*miot_on_set_fn)(const struct miot_prop *p);

/**
 * 属性表项。本地存储统一 int32（size=4）或单字节（size=1）。
 */
typedef struct miot_prop {
    uint8_t        siid;
    uint16_t       piid;
    uint8_t        type;      /* 对外声明的 MIoT 类型（MIOT_TYPE_xxx） */
    uint8_t        rw;        /* MIOT_PROP_R / MIOT_PROP_RW */
    uint8_t        size;      /* 本地存储宽度：1 或 4 */
    void          *ptr;       /* 指向状态变量 */
    int32_t        min, max;  /* 可写属性夹取区间（max<=min 表示不夹） */
    miot_on_set_fn on_set;    /* 写入成功后的副作用（LED/持久化/主动上报），可 NULL */
    const char    *name;      /* 日志用 */
} miot_prop_t;

/** 读写本地属性状态（属性表命中时由 miot_device 自动调用，也可手动调用） */
size_t miot_props_encode(const miot_prop_t *p, uint8_t *out, size_t cap);
bool   miot_props_apply(miot_prop_t *p, uint8_t vtype, const uint8_t *v, uint8_t vlen);
miot_prop_t *miot_props_find(miot_prop_t *tbl, size_t cnt, uint8_t siid, uint16_t piid);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_PROPS_H */
