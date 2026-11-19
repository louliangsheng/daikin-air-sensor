/**
 * @file miot_spec.h
 * @brief MIoT BLE Spec v2 帧编解码（纯函数、零依赖、可在 host 单元测试）
 *
 * 帧总格式：[len16: ver=2 高 4 位 | 长度低 12 位][tid16 LE][opcode][payload...]
 * 所有帧在加密通道（k61 / AES-CCM）内传输，本模块只负责明文层的组装与解析。
 *
 * —— 以下格式全部经米家 App 反汇编（ud5/gyb/kt4 解析器）+ 空中抓包实证 ——
 *
 *  op=0/1 set_properties 请求/应答
 *      请求:  [count]([siid][piid16LE][type16LE][len][value])×count
 *      应答:  [count]([siid][piid16LE][result16LE])×count
 *  op=2/3 get_properties 请求/应答
 *      请求:  [count]([siid][piid16LE])×count
 *      应答:  [count]([siid][piid16LE][result16LE][type16LE][value])×count
 *  op=4   properties_changed 主动上报
 *      帧体:  [count]([siid][piid16LE][type16LE][len][value])×count   ★首字节是 count
 *  op=5/6 action 请求/应答
 *      请求:  [siid][aiid16LE][argc]([piid16LE][type16LE][len][value])×argc
 *      应答:  [respCode16LE]        ★只有 respCode，无 siid/aiid（带则被误读为失败）
 *  op=7   event 事件上报
 *      帧体:  [siid][eiid16LE][argc]([piid16LE][type16LE][len][value])×argc
 *             ★每个参数前必须带 16 位 piid，缺了 App 端 ud5 解析直接返回 NULL
 *  op=F0  版本交换（明文，[05 20][tid][F0] → 镜像应答）
 *
 * type16 = (type << 12) | len，内部 16 位字段一律小端。
 * 线上类型码：1=bool 2=uint8 3=uint16 4=uint32 5=int32 9=float 10=string
 */
#ifndef MIOT_SPEC_H
#define MIOT_SPEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 线上数据类型码（type16 高 4 位）----
 * ★必须与 miot-spec 的 DataType 枚举一致：
 *   BOOL=0 UINT8=1 INT8=2 UINT16=3 INT16=4 UINT32=5 INT32=6
 *   UINT64=7 INT64=8 FLOAT32=9 STRING=10（没有 FLOAT64，枚举到 10 就结束）
 * 此前这里写成 BOOL=1 / U8=2（整体错位），App 侧 gyb 解析器于是把 4.1 开关
 * 解成 UINT8、把 2.1/3.1 解成 INT8（logcat [PARSE] 实证），页面按云端 spec
 * 校验类型对不上就丢属性，卡在「设备初始化中」。UINT16=3 与 FLOAT=9 恰好
 * 与标准重合，所以温湿度计一直能通，掩盖了这个问题。 */
#define MIOT_TYPE_BOOL   0u
#define MIOT_TYPE_U8     1u
#define MIOT_TYPE_I8     2u
#define MIOT_TYPE_U16    3u
#define MIOT_TYPE_I16    4u
#define MIOT_TYPE_U32    5u
#define MIOT_TYPE_I32    6u
#define MIOT_TYPE_U64    7u
#define MIOT_TYPE_I64    8u
#define MIOT_TYPE_FLOAT  9u
/* ★STRING=10，中间没有 FLOAT64。协议枚举只有 0..10 共 11 个类型
 * （bool/uint8/int8/uint16/int16/uint32/int32/uint64/int64/float/string），
 * 早期这里多塞了一个 F64=10，把 STRING 顶到 11 —— 体脂秤 8.1/8.2 事件
 * 的 type16 因此是 0xB0xx，App 按未知类型整帧丢弃，页面永远停在「上秤」
 * （23:10 实测：发出去 03 b0，而抓包样本是 03 a0）。 */
#define MIOT_TYPE_STRING 10u

/* ---- opcode ---- */
#define MIOT_OP_SET         0x00
#define MIOT_OP_SET_REPLY   0x01
#define MIOT_OP_GET         0x02
#define MIOT_OP_GET_REPLY   0x03
#define MIOT_OP_PROP_CHANGED 0x04
#define MIOT_OP_ACTION      0x05
#define MIOT_OP_ACTION_REPLY 0x06
#define MIOT_OP_EVENT       0x07
#define MIOT_OP_VERSION_XCH 0xF0

#define MIOT_SPEC_VER       2u      /* 协议版本（len16 高 4 位） */
/* 单包最大参数/对象数。★别调小到 6：13.1 sync（用户档案同步）一帧带 5 个入参，
 * 第 5 个正是整段 uinfo JSON，截断后既看不到也存不下（原来是 4）。
 * ★也别低于 8：温湿度计 3 Pro 页面初始化时一次 GET 请求 8 个属性
 * （7.1001 7.1002 8.1029 4.1003 5.1026 8.1 7.1 6.1027），
 * MAX_ARGS=6 时 args[6]/args[7] 越界读、handle_get 的 vals[6]/vals[7] 越界写，
 * 把返回地址冲成 0 → Instruction access fault 直接重启（2026-11-13 21:49 实测）。
 * NimBLE host 任务栈 8KB；为控制栈占用，handle_get/handle_set 的临时数组已改 static。 */
#define MIOT_SPEC_MAX_ARGS  12
/* 字符串参数最大长度。★别调小：体脂秤 8.2 结果帧是 28 字段 CSV，
 * 实测 66 字符（含 10 位时间戳）。原来是 40，载荷被静默截断，
 * App 解析不出字段，页面就一直停在「上秤」（23:07 实测）。 */
#define MIOT_SPEC_MAX_STR   128     /* 字符串参数最大长度 */

/** 规范值：类型标签 + 联合体载荷（argc 展开用） */
typedef struct miot_spec_value {
    uint8_t  siid;      /* 仅 properties_changed / get·set reply 用 */
    uint16_t piid;
    uint8_t  type;      /* MIOT_TYPE_xxx */
    uint16_t result;    /* 仅 get/set reply 用：0=成功 其它=错误码 */
    union {
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        int32_t  i32;
        float    f;
        char     str[MIOT_SPEC_MAX_STR + 1];
    } val;
} miot_spec_value_t;

/** 解析出的明文包 */
typedef struct miot_spec_packet {
    uint8_t  op;
    uint16_t tid;
    uint8_t  siid;
    uint16_t piid;      /* get 请求的查询 piid / action 的 aiid */
    uint16_t resp_code; /* action reply */
    uint8_t  argc;
    miot_spec_value_t args[MIOT_SPEC_MAX_ARGS];
} miot_spec_packet_t;

/* ================= 组包（返回帧总长，负数=错误码） ================= */

/** set_properties 应答（op=1） */int miot_spec_build_set_reply(uint16_t tid, const miot_spec_value_t *res, size_t count,
                              uint8_t *out, size_t cap);

/** get_properties 应答（op=3） */
int miot_spec_build_get_reply(uint16_t tid, const miot_spec_value_t *vals, size_t count,
                              uint8_t *out, size_t cap);

/** properties_changed（op=4） */
int miot_spec_build_props_changed(uint16_t tid, const miot_spec_value_t *props, size_t count,
                                  uint8_t *out, size_t cap);

/** action 应答（op=6，仅 respCode） */
int miot_spec_build_action_reply(uint16_t tid, uint16_t resp_code,
                                 uint8_t *out, size_t cap);

/** action 应答（op=6）带 out 参数：`[06][respCode16][count][piid8][type16][value]...`
 *  13.1 sync 必须带 5 个 out（offline-msg-cnt / sn / mac / fw / battery） */
int miot_spec_build_action_reply_ex(uint16_t tid, uint16_t resp_code,
                                    const miot_spec_value_t *outs, size_t count,
                                    uint8_t *out, size_t cap);

/** 事件（op=7），args 的 piid/type/val 有效 */
int miot_spec_build_event(uint16_t tid, uint8_t siid, uint16_t eiid,
                          const miot_spec_value_t *args, size_t argc,
                          uint8_t *out, size_t cap);

/** 版本交换应答（op=F0 镜像） */
int miot_spec_build_version_reply(uint16_t tid, uint8_t *out, size_t cap);

/* ================= 解包（返回 0 成功，负数=错误码） ================= */

/** 解析一个明文 spec 帧（set/get 请求、action 请求、版本交换） */
int miot_spec_parse(const uint8_t *in, size_t len, miot_spec_packet_t *out);

/** 便捷：把 miot_spec_value_t 按声明类型编码为 [type16][value]，返回字节数 */
size_t miot_spec_encode_value(const miot_spec_value_t *v, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_SPEC_H */
