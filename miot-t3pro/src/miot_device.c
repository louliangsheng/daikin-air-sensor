/**
 * @file miot_device.c
 * @brief 设备层组合 API 实现
 */
#include "miot_ble/miot_device.h"
#include "miot_ble/miot_crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------------- 日志 ---------------- */

static void dev_log(miot_device_t *d, int lv, const char *fmt, ...)
{
    if (!d->port || !d->port->log) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    d->port->log(d->port->user, lv, "miot", "%s", buf);
}

/* ---------------- 底层发送 ---------------- */

static void ag2_send_cb(void *user, const uint8_t *frame, size_t len)
{
    miot_device_t *d = (miot_device_t *)user;
    if (d->port && d->port->send_notify) d->port->send_notify(d->port->user, frame, len);
}

static void ag2_frame_cb(void *user, const uint8_t *k61, size_t n)
{
    miot_device_t *d = (miot_device_t *)user;
    if (!d->session_ok || n < 6) return;

    uint16_t seq = (uint16_t)(k61[0] | (k61[1] << 8));
    size_t ct_len = n - 2 - 4;                  /* 去掉 seq16 与 tag4 */
    /* ★缓冲从 256 提到 512（与 MIOT_AG2_RX_CAP 对齐）：2026-11-12 夜里手机在
     * spec version exchange 之后紧接着发了一个 ~500B 的多帧请求（ag2 共 32 帧），
     * 旧代码 `if (ct_len > sizeof(pt)) return;` **静默丢弃**，App 收不到应答就
     * 干等 19s 再 0x213 断开。丢包必须有日志，否则现场只能看到"卡住"。 */
    static uint8_t pt[512];
    if (ct_len > sizeof(pt)) {
        dev_log(d, MIOT_LOG_WARN, "spec req DROP seq=%u ct=%u > cap %u",
                seq, (unsigned)ct_len, (unsigned)sizeof(pt));
        return;
    }
    if (miot_spec_frame_decrypt(d->keys + 16, d->keys + 36, seq, k61 + 2, ct_len, pt)) {
        dev_log(d, MIOT_LOG_WARN, "spec decrypt fail seq=%u", seq);
        return;
    }
    dev_log(d, MIOT_LOG_INFO, "spec req seq=%u pt=%uB", seq, (unsigned)ct_len);
    /* ★请求明文全量转储（前 4 包）：267B 大请求被误判成 ACTION siid=13 aiid=1281，
     * 说明字段边界对不上；只有看到完整字节才能确定真实布局。 */
    static int n_dump;
    if (n_dump < 4) {
        n_dump++;
        /* ★static：本函数跑在 nimble_host 栈上，这 124B 的转储缓冲没必要压栈 */
        static char hx[3 * 40 + 4];
        for (size_t off = 0; off < ct_len; off += 40) {
            size_t dn = ct_len - off;
            if (dn > 40) dn = 40;
            size_t o = 0;
            for (size_t i = 0; i < dn; i++)
                o += (size_t)snprintf(hx + o, sizeof(hx) - o, "%02X ", pt[off + i]);
            hx[o] = 0;
            dev_log(d, MIOT_LOG_INFO, "  pt[%03u] %s", (unsigned)off, hx);
        }
    }
    miot_device_dispatch_plain(d, pt, ct_len);
}

/* ---------------- 加密发送（明文 → k61 → ag2） ---------------- */

static int spec_reply(miot_device_t *d, const uint8_t *pt, size_t pt_len)
{
    if (!d->session_ok) return -1;
    if (pt_len > 300) return -2;

    uint8_t out[2 + 300 + 4];
    if (miot_spec_frame_encrypt(d->keys, d->keys + 32, d->tx_seq, pt, pt_len, out + 2)) {
        /* 最常见原因：忘记注册加密后端（miot_crypto_set_ops） */
        static int warned;
        if (!warned++) {
            dev_log(d, MIOT_LOG_ERROR,
                    "spec encrypt fail — crypto backend registered? see miot_crypto_set_ops / port/esp32");
        }
        return -3;
    }

    /* ★明文 trace：页面不刷新时，先确认发出去的 spec 包本身对不对
     * （siid/piid/类型/值）。加密后的字节看不出来，出问题只能靠这段对照 spec。 */
    static uint32_t n_reply;
    if (n_reply < 40) {
        char hex[3 * 40 + 4];
        size_t n = pt_len > 40 ? 40 : pt_len, o = 0;
        for (size_t i = 0; i < n; i++)
            o += (size_t)snprintf(hex + o, sizeof(hex) - o, "%02X ", pt[i]);
        hex[o] = 0;
        dev_log(d, MIOT_LOG_INFO, "tx#%lu pt=%uB wire=%uB: %s%s",
                (unsigned long)++n_reply, (unsigned)pt_len, (unsigned)(pt_len + 6),
                hex, pt_len > 40 ? "..." : "");
    }
    out[0] = (uint8_t)(d->tx_seq & 0xFF);
    out[1] = (uint8_t)(d->tx_seq >> 8);
    d->tx_seq++;
    miot_ag2_send(&d->ag2, out, pt_len + 6);
    return 0;
}

/* ---------------- 生命周期 ---------------- */

void miot_device_init(miot_device_t *d, miot_port_t *port)
{
    memset(d, 0, sizeof(*d));
    d->port = port;
    d->tid = 0;
    miot_ag2_init(&d->ag2);
    d->ag2.send = ag2_send_cb;
    d->ag2.on_frame = ag2_frame_cb;
    d->ag2.user = d;
}

void miot_device_attach(miot_device_t *d, miot_prop_t *tbl, size_t cnt,
                        miot_get_fn get, miot_set_fn set, void *user)
{
    d->props = tbl;
    d->prop_cnt = cnt;
    d->on_get = get;
    d->on_set = set;
    d->user = user;
}

void miot_device_set_session_keys(miot_device_t *d, const uint8_t keys[MIOT_KEYS_LEN])
{
    memcpy(d->keys, keys, MIOT_KEYS_LEN);
    d->tx_seq = 0;
    d->session_ok = 1;
    dev_log(d, MIOT_LOG_INFO, "spec session ready (keys set)");
}

void miot_device_session_reset(miot_device_t *d)
{
    d->session_ok = 0;
    miot_ag2_init(&d->ag2);
    d->ag2.send = ag2_send_cb;
    d->ag2.on_frame = ag2_frame_cb;
    d->ag2.user = d;
}

void miot_device_on_rx(miot_device_t *d, const uint8_t *data, size_t len)
{
    /* ★调试用 rx trace：App 在设备页初始化时会先发一串请求（0x001A/0x001B
     * 上的 ag2 帧或 0x0010 上的明文 spec 包）。这些包如果不打印，设备侧看起来
     * 就是「App 连上后什么都不做，页面一直转圈」，无法定位它到底在等什么。
     * 只在长度有意义时打印，避免 CCCD 之类的空写刷屏。 */
    static uint32_t n_rx;
    if (len >= 4 && len <= 64 && n_rx < 400) {
        char hex[3 * 32 + 4];
        size_t n = len > 32 ? 32 : len, o = 0;
        for (size_t i = 0; i < n; i++)
            o += (size_t)snprintf(hex + o, sizeof(hex) - o, "%02X ", data[i]);
        hex[o] = 0;
        dev_log(d, MIOT_LOG_INFO, "rx#%lu len=%u: %s%s",
                (unsigned long)++n_rx, (unsigned)len, hex,
                len > 32 ? "..." : "");
    }
    miot_ag2_rx(&d->ag2, data, len);
}

/* ---------------- 属性表驱动的 get/set ---------------- */

static void fill_value(miot_spec_value_t *v, uint8_t siid, uint16_t piid,
                       uint8_t type, uint16_t result, const void *src)
{
    memset(v, 0, sizeof(*v));
    v->siid = siid;
    v->piid = piid;
    v->type = type;
    v->result = result;
    if (!src) return;
    switch (type) {
    case MIOT_TYPE_BOOL:
    case MIOT_TYPE_U8:    v->val.u8 = *(const uint8_t *)src; break;
    case MIOT_TYPE_U16:   v->val.u16 = *(const uint16_t *)src; break;
    case MIOT_TYPE_U32:
    case MIOT_TYPE_I32:   v->val.u32 = *(const uint32_t *)src; break;
    case MIOT_TYPE_FLOAT: v->val.f = *(const float *)src; break;
    default: break;
    }
}

static void handle_get(miot_device_t *d, const miot_spec_packet_t *pkt)
{
    /* ★static：MIOT_SPEC_MAX_ARGS 提到 12 后单数组约 1.7KB，放在 8KB 的
     * NimBLE host 栈上会和 dispatch 里的 packet 叠加，逼近溢出。
     * spec 通道是单连接串行处理（同一时刻只有一个 req 在跑），static 安全。 */
    static miot_spec_value_t vals[MIOT_SPEC_MAX_ARGS];
    for (int i = 0; i < pkt->argc; i++) {
        uint8_t siid = pkt->args[i].siid;
        uint16_t piid = pkt->args[i].piid;
        memset(&vals[i], 0, sizeof(vals[i]));

        miot_prop_t *mp = miot_props_find(d->props, d->prop_cnt, siid, piid);
        if (mp) {
            /* 从本地存储读出再按声明类型编码 */
            int32_t iv = (mp->size == 1) ? (int32_t)(*(uint8_t *)mp->ptr) : *(int32_t *)mp->ptr;
            fill_value(&vals[i], siid, piid, mp->type, 0, &iv);
            continue;
        }
        if (d->on_get && d->on_get(d->user, siid, piid, &vals[i])) {
            vals[i].siid = siid;
            vals[i].piid = piid;
            vals[i].result = 0;
            continue;
        }
        fill_value(&vals[i], siid, piid, 0, 1, NULL);   /* result=1 不认识 */
    }
    uint8_t pt[192];
    /* 把 App 实际请求的 siid.piid 打出来：页面缺读数时能一眼看出
     * 是「App 要的属性没注册」还是「注册了但值没上报」。 */
    char trace[160];
    int tn = 0;
    trace[0] = 0;
    for (int i = 0; i < pkt->argc; i++) {
        if (tn < (int)sizeof(trace) - 20) {
            tn += snprintf(trace + tn, sizeof(trace) - (size_t)tn, "%s%u.%u=%s",
                           i ? " " : "", pkt->args[i].siid, pkt->args[i].piid,
                           vals[i].result ? "ERR" : "ok");
        }
    }
    dev_log(d, MIOT_LOG_INFO, "spec GET %s", trace);
    int n = miot_spec_build_get_reply(pkt->tid, vals, pkt->argc, pt, sizeof(pt));
    if (n > 0) spec_reply(d, pt, (size_t)n);
}

static void handle_set(miot_device_t *d, const miot_spec_packet_t *pkt)
{
    static miot_spec_value_t res[MIOT_SPEC_MAX_ARGS];   /* 同上：省栈 */
    for (int i = 0; i < pkt->argc; i++) {
        const miot_spec_value_t *in = &pkt->args[i];
        uint16_t result = 1;
        miot_prop_t *mp = miot_props_find(d->props, d->prop_cnt, in->siid, in->piid);
        if (mp) {
            uint8_t raw[8];
            size_t rl = 0;
            switch (in->type) {
            case MIOT_TYPE_BOOL:
            case MIOT_TYPE_U8:    raw[0] = in->val.u8; rl = 1; break;
            case MIOT_TYPE_U16:   raw[0] = (uint8_t)in->val.u16; raw[1] = (uint8_t)(in->val.u16 >> 8); rl = 2; break;
            case MIOT_TYPE_U32:
            case MIOT_TYPE_I32:   memcpy(raw, &in->val.u32, 4); rl = 4; break;
            case MIOT_TYPE_FLOAT: memcpy(raw, &in->val.f, 4); rl = 4; break;
            default: break;
            }
            if (rl && miot_props_apply(mp, in->type, raw, (uint8_t)rl)) result = 0;
        } else if (d->on_set && d->on_set(d->user, in->siid, in->piid, in)) {
            result = 0;
        }
        dev_log(d, MIOT_LOG_INFO, "spec SET %u.%u type=%u val=%u%s",
                in->siid, in->piid, in->type,
                in->type == MIOT_TYPE_STRING ? 0u : (unsigned)in->val.u32,
                result ? " -> ERR" : "");
        memset(&res[i], 0, sizeof(res[i]));
        res[i].siid = in->siid;
        res[i].piid = in->piid;
        res[i].result = result;
    }
    uint8_t pt[64];
    int n = miot_spec_build_set_reply(pkt->tid, res, pkt->argc, pt, sizeof(pt));
    if (n > 0) spec_reply(d, pt, (size_t)n);
}

/* ---------------- 明文请求分派 ---------------- */

/* 固件可选接管 App 的 ACTION：体脂秤页面进「测量」时会发 ACTION siid=10 aiid=1，
 * 这就是设备端唯一能看到的「用户点了测量」信号（23:57 实测成对出现）。 */
__attribute__((weak)) void miot_demo_app_action_hook(uint8_t siid, uint16_t aiid)
{
    (void)siid; (void)aiid;
}

/* 设备可拿到 ACTION 的入参本体：体脂秤 13.1 sync 的第 3 个入参（piid=4）就是
 * 手机当前时间，板子没有 RTC，只能靠它把 13.4 current-time 校准对。 */
__attribute__((weak)) void miot_demo_action_args_hook(uint8_t siid, uint16_t aiid,
                                                      const miot_spec_value_t *args, int argc)
{
    (void)siid; (void)aiid; (void)args; (void)argc;
}

/* 设备可接管 action 的 out 参数（返回个数，0=按旧格式只回 respCode）。
 * 13.1 sync 需要 5 个 out，缺了 App 的同步流程就停在开屏。 */
__attribute__((weak)) int miot_device_action_out(uint8_t siid, uint16_t aiid,
                                                 miot_spec_value_t *outs, int max)
{
    (void)siid; (void)aiid; (void)outs; (void)max;
    return 0;
}


/* 解析失败时把明文打出来：App 的 SET 里可能出现我们没实现的类型
 * （i16/u64/i64）或超长字符串，静默丢弃会让它无限重发、页面一直转圈。 */
static void dump_hex(miot_device_t *d, const uint8_t *p, size_t len)
{
    char line[16 * 3 + 4];
    for (size_t off = 0; off < len; off += 16) {
        size_t n = len - off;
        if (n > 16) n = 16;
        int k = 0;
        for (size_t i = 0; i < n; i++) k += snprintf(line + k, sizeof(line) - (size_t)k, "%02X ", p[off + i]);
        dev_log(d, MIOT_LOG_WARN, "  pt[%02u] %s", (unsigned)off, line);
    }
}


void miot_device_dispatch_plain(miot_device_t *d, const uint8_t *pt, size_t len)
{
    miot_spec_packet_t pkt;
    int rc = miot_spec_parse(pt, len, &pkt);
    if (rc != 0) {
        dev_log(d, MIOT_LOG_WARN, "spec parse fail rc=%d len=%uB op=%02x tid=%u",
                rc, (unsigned)len, len > 4 ? pt[4] : 0, len > 3 ? (unsigned)(pt[2] | (pt[3] << 8)) : 0);
        dump_hex(d, pt, len > 128 ? 128 : len);
        return;
    }
    switch (pkt.op) {
    case MIOT_OP_VERSION_XCH: {
        uint8_t r[5];
        int n = miot_spec_build_version_reply(pkt.tid, r, sizeof(r));
        if (n > 0) spec_reply(d, r, (size_t)n);
        dev_log(d, MIOT_LOG_INFO, "spec version exchange done (dev ver=2)");
        break;
    }
    case MIOT_OP_ACTION: {
        miot_demo_app_action_hook(pkt.siid, pkt.piid);
        /* 入参逐项打印：13.1 sync 的第 5 个入参是整段用户档案 JSON，
         * 解析对不对一眼就能看出来（旧版这里会显示 aiid=1281）。 */
        for (int i = 0; i < pkt.argc; i++) {
            const miot_spec_value_t *a = &pkt.args[i];
            if (a->type == MIOT_TYPE_STRING)
                dev_log(d, MIOT_LOG_INFO, "  act in[%d] piid=%u str(%u)B: %.96s",
                        i, a->piid, (unsigned)strlen(a->val.str), a->val.str);
            else if (a->type == MIOT_TYPE_U32 || a->type == MIOT_TYPE_I32 ||
                     a->type == MIOT_TYPE_U64 || a->type == MIOT_TYPE_I64)
                dev_log(d, MIOT_LOG_INFO, "  act in[%d] piid=%u u32=%u",
                        i, a->piid, (unsigned)a->val.u32);
            else
                dev_log(d, MIOT_LOG_INFO, "  act in[%d] piid=%u t=%u val=%u",
                        i, a->piid, a->type, (unsigned)a->val.u32);
        }
        miot_demo_action_args_hook(pkt.siid, pkt.piid, pkt.args, (int)pkt.argc);
        /* 应答：默认 `[06][respCode16]`；若设备侧能提供 out 参数就按 kt4 带齐
         * （13.1 sync 的 6/7/8/9/10，缺一个 App 都不推进同步流程） */
        uint8_t r[192];
        static miot_spec_value_t outs[MIOT_SPEC_MAX_ARGS];   /* 同上：省栈 */
        memset(outs, 0, sizeof(outs));
        int no = miot_device_action_out(pkt.siid, pkt.piid, outs, (int)MIOT_SPEC_MAX_ARGS);
        int n = (no > 0)
              ? miot_spec_build_action_reply_ex(pkt.tid, 0, outs, (size_t)no, r, sizeof(r))
              : miot_spec_build_action_reply(pkt.tid, 0, r, sizeof(r));
        if (n > 0) spec_reply(d, r, (size_t)n);
        dev_log(d, MIOT_LOG_WARN, "spec ACTION siid=%u aiid=%u argc=%u -> respCode=0 outs=%d",
                pkt.siid, pkt.piid, pkt.argc, no);
        break;
    }
    case MIOT_OP_GET:
        handle_get(d, &pkt);
        break;
    case MIOT_OP_SET:
        handle_set(d, &pkt);
        break;
    default:
        dev_log(d, MIOT_LOG_WARN, "spec op=0x%02x unsupported", pkt.op);
        break;
    }
}

/* ---------------- 主动上报 ---------------- */

int miot_device_send_event(miot_device_t *d, uint8_t siid, uint16_t eiid,
                           const miot_spec_value_t *args, size_t argc)
{
    if (!d->session_ok) return -1;
    /* 8.2 结果帧是 28 字段 CSV（pt≈78B），128 够但余量太小，统一放 256 */
    uint8_t pt[256];
    uint16_t tid = d->tid++;
    int n = miot_spec_build_event(tid, siid, eiid, args, argc, pt, sizeof(pt));
    if (n <= 0) return n;
    spec_reply(d, pt, (size_t)n);
    return n;
}

int miot_device_send_props_changed(miot_device_t *d,
                                   const miot_spec_value_t *props, size_t count)
{
    if (!d->session_ok) return -1;
    uint8_t pt[256];
    uint16_t tid = d->tid++;
    int n = miot_spec_build_props_changed(tid, props, count, pt, sizeof(pt));
    if (n <= 0) return n;
    spec_reply(d, pt, (size_t)n);
    return n;
}

int miot_device_send_action_reply(miot_device_t *d, uint16_t tid, uint16_t resp_code)
{
    uint8_t r[7];
    int n = miot_spec_build_action_reply(tid, resp_code, r, sizeof(r));
    if (n <= 0) return n;
    return spec_reply(d, r, (size_t)n);
}

int miot_device_send_version_reply(miot_device_t *d, uint16_t tid)
{
    uint8_t r[5];
    int n = miot_spec_build_version_reply(tid, r, sizeof(r));
    if (n <= 0) return n;
    return spec_reply(d, r, (size_t)n);
}
