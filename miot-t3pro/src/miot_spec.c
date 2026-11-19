/**
 * @file miot_spec.c
 * @brief MIoT BLE Spec v2 帧编解码实现（从 S400 模拟秤固件抽象而来）
 */
#include "miot_ble/miot_spec.h"
#include <string.h>

/* ---------------- 内部小工具 ---------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/** 写入帧头占位：[00 00][tid16][op] */
static size_t hdr_begin(uint8_t *out, uint16_t tid, uint8_t op)
{
    out[0] = 0; out[1] = 0;                 /* len16 占位 */
    put16(out + 2, tid);
    out[4] = op;
    return 5;
}

/** 回填 len16：ver=2 高 4 位 | 总长低 12 位 */
static void hdr_finish(uint8_t *out, size_t n)
{
    uint16_t v = (uint16_t)((MIOT_SPEC_VER << 12) | (n & 0x0FFF));
    put16(out, v);
}

size_t miot_spec_encode_value(const miot_spec_value_t *v, uint8_t *out, size_t cap)
{
    uint8_t buf[8];
    uint8_t vlen = 0;

    switch (v->type) {
    case MIOT_TYPE_BOOL:
        buf[0] = v->val.u8 ? 1 : 0; vlen = 1; break;
    case MIOT_TYPE_U8:
        buf[0] = v->val.u8; vlen = 1; break;
    case MIOT_TYPE_U16:
        put16(buf, v->val.u16); vlen = 2; break;
    case MIOT_TYPE_U32:
    case MIOT_TYPE_I32:
        memcpy(buf, &v->val.u32, 4); vlen = 4; break;
    case MIOT_TYPE_FLOAT:
        memcpy(buf, &v->val.f, 4); vlen = 4; break;
    case MIOT_TYPE_STRING: {
        size_t sl = strlen(v->val.str);
        if (sl > MIOT_SPEC_MAX_STR) sl = MIOT_SPEC_MAX_STR;
        if (2 + sl > cap) return 0;
        put16(out, (uint16_t)((v->type << 12) | sl));
        memcpy(out + 2, v->val.str, sl);
        return 2 + (size_t)sl;
    }
    default:
        return 0;
    }
    if (2 + (size_t)vlen > cap) return 0;
    put16(out, (uint16_t)((v->type << 12) | vlen));
    memcpy(out + 2, buf, vlen);
    return 2 + (size_t)vlen;
}

/* ---------------- 组包 ---------------- */

int miot_spec_build_set_reply(uint16_t tid, const miot_spec_value_t *res, size_t count,
                              uint8_t *out, size_t cap)
{
    if (count > MIOT_SPEC_MAX_ARGS || cap < 6 + count * 5) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_SET_REPLY);
    out[n++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        out[n++] = res[i].siid;
        put16(out + n, res[i].piid); n += 2;
        put16(out + n, res[i].result); n += 2;
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_get_reply(uint16_t tid, const miot_spec_value_t *vals, size_t count,
                              uint8_t *out, size_t cap)
{
    if (count > MIOT_SPEC_MAX_ARGS || cap < 6 + count * 15) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_GET_REPLY);
    out[n++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        uint8_t enc[MIOT_SPEC_MAX_STR + 2];
        size_t en;
        if (vals[i].result != 0) {
            en = 0;
        } else {
            en = miot_spec_encode_value(&vals[i], enc, sizeof(enc));
            if (en == 0) return -2;
        }
        if (n + 5 + en > cap) return -3;
        out[n++] = vals[i].siid;
        put16(out + n, vals[i].piid); n += 2;
        put16(out + n, vals[i].result); n += 2;
        if (en) { memcpy(out + n, enc, en); n += en; }
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_props_changed(uint16_t tid, const miot_spec_value_t *props, size_t count,
                                  uint8_t *out, size_t cap)
{
    if (count > MIOT_SPEC_MAX_ARGS || cap < 6 + count * 15) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_PROP_CHANGED);
    out[n++] = (uint8_t)count;               /* ★ gyb 解析器要求前置 count */
    for (size_t i = 0; i < count; i++) {
        uint8_t enc[MIOT_SPEC_MAX_STR + 2];
        size_t en = miot_spec_encode_value(&props[i], enc, sizeof(enc));
        if (en == 0) return -2;
        if (n + 3 + en > cap) return -3;
        out[n++] = props[i].siid;
        put16(out + n, props[i].piid); n += 2;
        memcpy(out + n, enc, en); n += en;
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_action_reply(uint16_t tid, uint16_t resp_code,
                                 uint8_t *out, size_t cap)
{
    return miot_spec_build_action_reply_ex(tid, resp_code, NULL, 0, out, cap);
}

/* action 应答（带 out 参数）：
 *   `[op=06][respCode16][count][piid8][type16][value]...`
 * ★piid 在 out 参数里是 **8 位**（docs/protocol.md §4.2 第 3 条，kt4 格式）。
 * 13.1 sync 必须有这 5 个 out（6/7/8/9/10），否则 App 同步流程不推进 ——
 * 页面停在开屏（v67 实测：只回 respCode=0 时 App 会重发一次后彻底沉默）。 */
int miot_spec_build_action_reply_ex(uint16_t tid, uint16_t resp_code,
                                    const miot_spec_value_t *outs, size_t count,
                                    uint8_t *out, size_t cap)
{
    if (cap < 9) return -1;
    if (count > MIOT_SPEC_MAX_ARGS) return -2;
    size_t n = hdr_begin(out, tid, MIOT_OP_ACTION_REPLY);
    put16(out + n, resp_code); n += 2;       /* ★ 不带 siid/aiid */
    out[n++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        uint8_t enc[MIOT_SPEC_MAX_STR + 2];
        size_t en = miot_spec_encode_value(&outs[i], enc, sizeof(enc));
        if (en == 0) return -3;
        if (n + 2 + en > cap) return -3;
        out[n++] = (uint8_t)(outs[i].piid & 0xFF);
        memcpy(out + n, enc, en); n += en;
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_event(uint16_t tid, uint8_t siid, uint16_t eiid,
                          const miot_spec_value_t *args, size_t argc,
                          uint8_t *out, size_t cap)
{
    /* ★只校验参数个数与最小头部空间。原来按「每个参数都是最长字符串」做
     * 最坏情况预检（cap < 8 + argc*(MAX_STR+4)），argc=3 时要求 140B，
     * 而调用方缓冲是 128B，于是 11.1022 事件永远返回 -1 —— 收尾事件缺失，
     * App 报告页永远等不到完成信号（23:07 实测 step 5/7 rc=-1）。
     * 真正的容量检查在下面逐参数做（n + 2 + en > cap）。 */
    if (argc > MIOT_SPEC_MAX_ARGS || cap < 8) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_EVENT);
    out[n++] = siid;
    put16(out + n, eiid); n += 2;
    out[n++] = (uint8_t)argc;
    for (size_t i = 0; i < argc; i++) {
        uint8_t enc[MIOT_SPEC_MAX_STR + 2];
        size_t en = miot_spec_encode_value(&args[i], enc, sizeof(enc));
        if (en == 0) return -2;
        if (n + 2 + en > cap) return -3;
        put16(out + n, args[i].piid); n += 2;   /* ★ 每参数前置 piid16（ud5 要求） */
        memcpy(out + n, enc, en); n += en;
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_version_reply(uint16_t tid, uint8_t *out, size_t cap)
{
    if (cap < 5) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_VERSION_XCH);
    hdr_finish(out, n);
    return (int)n;
}

/* ---------------- 解包 ---------------- */

/* App→设备 SET 请求对象（固件实测互通格式，勿改）：
 * [siid 1B][piid 16LE][type 16LE（纯类型号，非 (type<<12)|len）][len 1B][value]
 * 注意：设备→App 方向（get_reply 等）的 value 编码才是 [type16=(type<<12)|len]，
 * 两个方向格式不同 —— 见 docs/protocol.md。 */
/** 解析一个「无 siid」的入参对象：`[piid16][type16]` + value，type16=(type<<12)|len。
 *  action 的入参就是这种形态（v67 串口实证：13.1 sync 的 5 个入参）。
 *  返回消耗字节数，负数=错误。 */
static int parse_input(const uint8_t *p, size_t remain, uint8_t siid, miot_spec_value_t *v)
{
    if (remain < 4) return -1;
    v->siid = siid;
    v->piid = get16(p);
    uint16_t t16 = get16(p + 2);
    uint8_t vtype = (uint8_t)(t16 >> 12);
    size_t vlen = t16 & 0x0FFF;
    if (remain < 4 + vlen) return -1;
    const uint8_t *val = p + 4;
    v->type = vtype;
    switch (vtype) {
    case MIOT_TYPE_BOOL:
    case MIOT_TYPE_U8:   v->val.u8  = val[0]; break;
    case MIOT_TYPE_I8:   v->val.u8  = val[0]; break;
    case MIOT_TYPE_U16:
    case MIOT_TYPE_I16:  v->val.u16 = get16(val); break;
    case MIOT_TYPE_U32:
    case MIOT_TYPE_I32:
    case MIOT_TYPE_U64:
    case MIOT_TYPE_I64:  memcpy(&v->val.u32, val, vlen >= 4 ? 4 : vlen); break;
    case MIOT_TYPE_FLOAT: memcpy(&v->val.f, val, 4); break;
    case MIOT_TYPE_STRING: {
        size_t n = vlen > MIOT_SPEC_MAX_STR ? MIOT_SPEC_MAX_STR : vlen;
        memcpy(v->val.str, val, n);
        v->val.str[n] = 0;
        break;
    }
    default:
        memset(&v->val, 0, sizeof(v->val.str));
        break;
    }
    return (int)(4 + vlen);
}

static int parse_object(const uint8_t *p, size_t remain, miot_spec_value_t *v)
{
    if (remain < 5) return -1;
    v->siid = p[0];
    v->piid = get16(p + 1);
    /* ★App→设备的 SET 条目同样是 `(type<<12)|len`（docs/protocol.md §4.2 第 4 条）。
     * 旧代码固定按「type16 + len8」7 字节头解析，遇到大 SET 必然错位 → rc=-4 →
     * 整帧丢弃 → App 无限重发（23:47 实测）。这里做双格式判别。 */
    uint16_t rawt = get16(p + 3);
    uint8_t vtype;
    size_t vlen;
    const uint8_t *val;
    size_t adv;
    if ((rawt >> 12) != 0) {
        vtype = (uint8_t)(rawt >> 12);
        vlen  = rawt & 0x0FFF;
        val   = p + 5;
        adv   = 5 + vlen;
    } else if (remain >= 6) {                  /* 旧格式兼容：type16 高 4 位为 0 */
        vtype = (uint8_t)rawt;
        vlen  = p[5];
        val   = p + 6;
        adv   = 6 + vlen;
    } else {
        return -1;
    }
    if (remain < adv) return -1;
    v->type = vtype;

    switch (vtype) {
    case MIOT_TYPE_BOOL:
    case MIOT_TYPE_U8:   v->val.u8  = val[0]; break;
    case MIOT_TYPE_U16:
    case MIOT_TYPE_I16:  v->val.u16 = get16(val); break;
    case MIOT_TYPE_U32:
    case MIOT_TYPE_I32:  memcpy(&v->val.u32, val, 4); break;
    case MIOT_TYPE_U64:
    case MIOT_TYPE_I64:  memcpy(&v->val.u32, val, 4); break;   /* 只取低 32 位 */
    case MIOT_TYPE_FLOAT: memcpy(&v->val.f, val, 4); break;
    case MIOT_TYPE_STRING: {
        /* ★超长字符串不再让整包失败：App 会把整段配置/CSV 塞进 SET，
         * 一旦返回 -2，miot_device_dispatch_plain 就整帧丢弃，App 无限重发，
         * 页面永远等不到 set_reply（23:47 实测 spec parse fail rc=-4）。
         * 这里截断保留，至少能把 set_reply 回过去，让流程继续。 */
        size_t n = vlen > MIOT_SPEC_MAX_STR ? MIOT_SPEC_MAX_STR : vlen;
        memcpy(v->val.str, val, n);
        v->val.str[n] = 0;
        break;
    }
    default:
        /* 未知类型也不要整包失败，置 0 继续解析后面的对象 */
        memset(&v->val, 0, sizeof(v->val.str));
        break;
    }
    return (int)adv;
}

int miot_spec_parse(const uint8_t *in, size_t len, miot_spec_packet_t *out)
{
    if (len < 5) return -1;
    uint16_t hdr = get16(in);
    if ((hdr >> 12) != MIOT_SPEC_VER) return -2;
    memset(out, 0, sizeof(*out));
    out->tid = get16(in + 2);
    out->op  = in[4];

    const uint8_t *p = in + 5;
    size_t remain = len - 5;

    switch (out->op) {
    case MIOT_OP_VERSION_XCH:
        return 0;
    case MIOT_OP_GET:
        if (remain < 1) return -3;
        out->argc = p[0];
        p++; remain--;
        for (int i = 0; i < out->argc && i < MIOT_SPEC_MAX_ARGS; i++) {
            if (remain < 3) return -3;
            out->args[i].siid = p[0];
            out->args[i].piid = get16(p + 1);
            p += 3; remain -= 3;
        }
        /* ★必须钳制：只截断循环而不改 argc，上层按 pkt->argc 遍历就会读到
         * args[] 之外的栈垃圾，并在应答数组上越界写（3 Pro 一次 GET 8 个属性，
         * MAX_ARGS=6 时直接栈溢出重启）。宁可少回几个，也不能越界。 */
        if (out->argc > MIOT_SPEC_MAX_ARGS) out->argc = MIOT_SPEC_MAX_ARGS;
        return 0;
    case MIOT_OP_SET:
        if (remain < 1) return -3;
        out->argc = p[0];
        p++; remain--;
        for (int i = 0; i < out->argc && i < MIOT_SPEC_MAX_ARGS; i++) {
            int c = parse_object(p, remain, &out->args[i]);
            if (c < 0) return -4;
            p += c; remain -= (size_t)c;
        }
        if (out->argc > MIOT_SPEC_MAX_ARGS) out->argc = MIOT_SPEC_MAX_ARGS;
        return 0;
    case MIOT_OP_ACTION:
        /* ★v67 串口实证（267B 的 13.1 sync 帧逐字节对齐）：
         *   `[op=05][siid8][aiid8][argc8]` 然后 argc 个 `[piid16][type16][value]`
         * 旧代码把 aiid 当 u16、把入参当带 siid 的 SET 对象，于是 13.1 被读成
         * `aiid=0x0501=1281` 且对象全线错位，应答回去 App 也不认。 */
        if (remain < 3) return -3;
        out->siid = p[0];
        out->piid = p[1];                       /* aiid（8 位） */
        {
            uint8_t acnt = p[2];
            if (acnt > MIOT_SPEC_MAX_ARGS) acnt = MIOT_SPEC_MAX_ARGS;
            out->argc = acnt;
        }
        p += 3; remain -= 3;
        for (int i = 0; i < out->argc; i++) {
            int c = parse_input(p, remain, out->siid, &out->args[i]);
            if (c < 0) return -4;
            p += c; remain -= (size_t)c;
        }
        return 0;
    case MIOT_OP_ACTION_REPLY:
        if (remain < 2) return -3;
        out->resp_code = get16(p);
        return 0;
    default:
        return -5;   /* 事件/属性变更应答为设备→App 方向，一般无需解析 */
    }
}
