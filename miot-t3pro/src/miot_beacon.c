/**
 * @file miot_beacon.c
 * @brief MiBeacon v5 加密广播帧实现（格式逆向自真机，实测互证）
 */
#include "miot_ble/miot_beacon.h"
#include "miot_ble/miot_crypto.h"
#include <string.h>

/* ---------------- 对象编码：[obj_id 2B LE][len 1B][value] ---------------- */

size_t miot_beacon_obj_u8(uint8_t *p, uint16_t obj_id, uint8_t v)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = 1; p[3] = v;
    return 4;
}

size_t miot_beacon_obj_u16(uint8_t *p, uint16_t obj_id, uint16_t v)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = 2;
    p[3] = (uint8_t)(v & 0xFF); p[4] = (uint8_t)(v >> 8);
    return 5;
}

size_t miot_beacon_obj_u24(uint8_t *p, uint16_t obj_id, uint32_t v)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = 3;
    p[3] = (uint8_t)(v & 0xFF);
    p[4] = (uint8_t)((v >> 8) & 0xFF);
    p[5] = (uint8_t)((v >> 16) & 0xFF);
    return 6;
}

size_t miot_beacon_obj_i16(uint8_t *p, uint16_t obj_id, int16_t v)
{
    return miot_beacon_obj_u16(p, obj_id, (uint16_t)v);
}

size_t miot_beacon_obj_u32(uint8_t *p, uint16_t obj_id, uint32_t v)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = 4;
    p[3] = (uint8_t)(v & 0xFF);         p[4] = (uint8_t)((v >> 8) & 0xFF);
    p[5] = (uint8_t)((v >> 16) & 0xFF); p[6] = (uint8_t)((v >> 24) & 0xFF);
    return 7;
}

size_t miot_beacon_obj_f32(uint8_t *p, uint16_t obj_id, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    return miot_beacon_obj_u32(p, obj_id, bits);
}

size_t miot_beacon_obj_bytes(uint8_t *p, uint16_t obj_id, const uint8_t *v, size_t len)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = (uint8_t)len;
    memcpy(p + 3, v, len);
    return 3 + len;
}

/* ---------------- v5 加密帧 ----------------
 * FC(2) | PID(2) | cnt(1) | MAC(6) | ct(N) | counter_ext(3) | MIC(4)
 * nonce = MAC(6) | pid_le(2) | cnt_le(4)；aad = 0x11；tag 4B 放帧尾 */

/* ---------------- 未绑定身份心跳帧 ----------------
 * FC(2) | PID(2) | cnt(1) | MAC(6) | Capability(1) = 12B，不加密、无对象。
 * App 在「添加设备」里就靠这一帧把设备列出来：缺 Capability 或 registered
 * 位为 1，整帧会被静默丢弃，表现为「手机怎么扫都找不到」。 */
int miot_beacon_frame_hello(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                            uint8_t bond_ability, uint8_t *out, size_t cap)
{
    if (cap < 16) return -3;

    out[0] = 0x70;              /* bit4 MAC | bit5 Capability | bit6 Object Include */
    /* ★2026-11-12 13:1x 铁证修正（否决此前的 0x20/0x58 两轮猜测）：
     *   FC 高字节位布局：bit0=registered、bit1=请求被连接、bit2~3=authMode、
     *   bit4~7=version。App **靠 authMode 选认证分支**：
     *     authMode=0 → 二代 legacy p21 挑战应答（我们之前一直在走这条）
     *     authMode=2 → 标准认证（ECDH REGISTER + HKDF LOGIN，S400 真机形态）
     * 0x20/0x22 的 bit2~3 都是 00，所以 App 永远走 legacy：绑定"成功"、token
     * 是本地兜底值、详情页永远建不起 spec 会话（转圈→连接超时）。
     * 已绑定帧我们发的是 0x29（v2|registered|authMode=2），App 认；未绑定帧
     * 必须同样宣告 authMode=2 → **0x28**（配对窗口置 bit9 → 0x2A）。 */
    out[1] = 0x28;              /* v2 | registered=0 | authMode=2（标准认证待绑定） */
    out[2] = (uint8_t)(cfg->pid & 0xFF);
    out[3] = (uint8_t)(cfg->pid >> 8);
    out[4] = (uint8_t)(frame_cnt & 0xFF);
    memcpy(out + 5, cfg->mac_air, 6);
    out[11] = (uint8_t)(0x01 | ((bond_ability & 0x03) << 3));  /* bit0 可连接 | bit3~4 BondAbility */

    /* ★0x1011「待绑定」对象 —— 未绑定广播必须带，缺了它就是死局。
     *
     * 实测对拍（旧工程唯一跑通的那次，同一 App、同一 PID 0x03DD）：
     *   它的未绑定帧 = ... |cap 0x09| 11 10 01 00
     *   我们原来的   = ... |cap 0x08|（到此结束，无任何对象）
     * 后果：App 能扫到、能连上、能订阅 0x001B/0x001A/0x0001，但**永远不写
     * 0x0010 的 AUTH INIT、也永远不下发 12B token**，只是订阅完干等 1 秒后
     * 主动退订断开（reason 0x213），循环重试。它等的就是「这台设备已经进入
     * 待绑定状态」这个对象 —— 真机是按下蓝牙按钮后才在广播里带上它。
     *
     * 另有 cap bit0（可连接）也漏了：真机 0x09，我们 0x08。 */
    out[12] = 0x11;             /* 对象 0x1011 LE */
    out[13] = 0x10;
    out[14] = 0x01;             /* 长度 1B */
    out[15] = 0x00;             /* 0 = 待绑定 */
    return 16;
}

int miot_beacon_frame_v5(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                         const uint8_t *objs, size_t objs_len,
                         uint8_t *out, size_t cap)
{
    if (objs_len > 13) return -2;              /* 保证含公司码广播 ≤31B */
    const size_t total = 12 + objs_len + 3 + 4;
    if (cap < total) return -3;

    uint8_t nonce[12];
    memcpy(nonce, cfg->mac_air, 6);
    nonce[6] = (uint8_t)(cfg->pid & 0xFF);
    nonce[7] = (uint8_t)(cfg->pid >> 8);
    nonce[8]  = (uint8_t)(frame_cnt & 0xFF);
    nonce[9]  = (uint8_t)((frame_cnt >> 8) & 0xFF);
    nonce[10] = (uint8_t)((frame_cnt >> 16) & 0xFF);
    nonce[11] = (uint8_t)((frame_cnt >> 24) & 0xFF);

    int n = 0;
    out[n++] = 0x58;                            /* FC lo: obj|mac|enc */
    out[n++] = 0x59;                            /* FC hi: v5|registered（未注册 0x58 被 App 无视） */
    out[n++] = nonce[6];                        /* PID LE */
    out[n++] = nonce[7];
    out[n++] = nonce[8];                        /* frame_cnt（低字节） */
    memcpy(out + n, cfg->mac_air, 6);           /* MAC 字段：App 归属判定依据 */
    n += 6;

    /* 加密：miot_ccm_encrypt 输出 [ct][tag4]，帧内顺序要求
     * counter_ext(3) 夹在 ct 与 MIC 之间，故 tag 先落临时区再回填 */
    uint8_t aad = 0x11;
    uint8_t tmp[13 + 4];
    if (objs_len > sizeof(tmp) - 4) return -2;
    if (miot_ccm_encrypt(cfg->beacon_key, nonce, &aad, 1,
                         objs, objs_len, tmp) != 0)
        return -1;                              /* 未注册加密后端 */
    memcpy(out + n, tmp, objs_len);             /* ct */
    n += (int)objs_len;

    out[n++] = nonce[9];                        /* counter_ext(3) 在 MIC 之前 */
    out[n++] = nonce[10];
    out[n++] = nonce[11];
    memcpy(out + n, tmp + objs_len, 4);         /* MIC(4) 必须在最后 */
    n += 4;
    return n;
}

/* ---------------- v5 加密帧（无 MAC 形态，GATT notify 用） ----------------
 * FC(2) | PID(2) | cnt(1) | ct(N) | counter_ext(3) | MIC(4)
 *
 * 为什么需要这一形态：App 的 ATT MTU 固定 23，单包上限 20B。带 MAC 的 v5 帧
 * 光帧头就 11B，再加 counter_ext(3)+MIC(4)=18B，只剩 2B，连一个最小的 u8 对象
 * （obj_id 2 + len 1 + value 1 = 4B）都塞不进去 —— 于是只能推「空对象帧」，
 * App 收到后解析不出任何属性，订阅完 1s 就退订断开，页面永远「连接超时」。
 * GATT 通道上接收方本来就靠连接识别设备，MAC 字段冗余，真机 24B 测量帧同样
 * 用无 MAC 形态（见 miot_beacon.h 说明）。去掉 6B MAC 后：
 *   13 + objs_len，一个 3B 对象 = 19B ✓，一个 1B 对象 = 17B ✓
 * nonce 里仍然要带 MAC（CCM 的 nonce 与帧头无关）。 */
int miot_beacon_frame_v5_nomac(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                               const uint8_t *objs, size_t objs_len,
                               uint8_t *out, size_t cap)
{
    if (objs_len > 7) return -2;                    /* 13+7=20，超过就超 ATT 上限 */
    const size_t total = 6 + objs_len + 3 + 4;
    if (cap < total) return -3;

    uint8_t nonce[12];
    memcpy(nonce, cfg->mac_air, 6);
    nonce[6] = (uint8_t)(cfg->pid & 0xFF);
    nonce[7] = (uint8_t)(cfg->pid >> 8);
    nonce[8]  = (uint8_t)(frame_cnt & 0xFF);
    nonce[9]  = (uint8_t)((frame_cnt >> 8) & 0xFF);
    nonce[10] = (uint8_t)((frame_cnt >> 16) & 0xFF);
    nonce[11] = (uint8_t)((frame_cnt >> 24) & 0xFF);

    int n = 0;
    out[n++] = 0x48;                                /* FC lo: obj|enc，bit4(MAC)=0 */
    out[n++] = 0x59;                                /* FC hi: v5|registered */
    out[n++] = nonce[6];
    out[n++] = nonce[7];
    out[n++] = nonce[8];

    uint8_t aad = 0x11;
    uint8_t tmp[7 + 4];
    if (miot_ccm_encrypt(cfg->beacon_key, nonce, &aad, 1,
                         objs, objs_len, tmp) != 0)
        return -1;
    memcpy(out + n, tmp, objs_len);
    n += (int)objs_len;

    out[n++] = nonce[9];
    out[n++] = nonce[10];
    out[n++] = nonce[11];
    memcpy(out + n, tmp + objs_len, 4);
    n += 4;
    return n;
}

/* ---------------- 未绑定帧（12B）与 PSK 扫描探测形态表 ---------------- */

int miot_beacon_frame_unbound(uint16_t pid, const uint8_t mac_air[6],
                              uint8_t fc0, uint8_t fc1, uint8_t capability,
                              uint8_t frame_cnt, uint8_t *out, size_t cap)
{
    if (cap < 12) return -3;
    out[0] = fc0;
    out[1] = fc1;
    out[2] = (uint8_t)(pid & 0xFF);             /* PID LE */
    out[3] = (uint8_t)(pid >> 8);
    out[4] = frame_cnt;
    memcpy(out + 5, mac_air, 6);                /* 空中序：App 扫描归属依据 */
    out[11] = capability;
    return 12;
}

/*
 * 候选形态（按可能性排序）。解码依据（标准认证型号实测基线 30 58 10）：
 *   fc1 = version(高 4 位) | authMode(bit2..3) | registered(bit0)
 *   capability = BondAbility(bit3..4)
 * 第 0 项是标准认证型号被 App 认可的形态；PSK 型号认哪一项未知，
 * 排序理由见 docs/psk-auth.md §6.2。
 */
static const miot_adv_form_t PROBE_FORMS[] = {
    { 0x30, 0x58, 0x10, "v5 authMode=2 cap=10 (std baseline)" },
    { 0x30, 0x48, 0x10, "v4 authMode=2 cap=10 (legacy beacon)" },
    { 0x30, 0x54, 0x10, "v5 authMode=1 cap=10" },
    { 0x30, 0x50, 0x10, "v5 authMode=0 cap=10" },
    { 0x30, 0x5C, 0x10, "v5 authMode=3 cap=10" },
    { 0x30, 0x58, 0x08, "v5 authMode=2 cap=08 (BondAbility=1)" },
    { 0x30, 0x58, 0x00, "v5 authMode=2 cap=00" },
};

const miot_adv_form_t *miot_adv_probe_forms(size_t *count)
{
    if (count)
        *count = sizeof(PROBE_FORMS) / sizeof(PROBE_FORMS[0]);
    return PROBE_FORMS;
}

int miot_adv_parse(const uint8_t *sd, size_t len, miot_adv_info_t *out)
{
    if (len < 4) return -1;                     /* 至少 FC(2)+PID(2) */
    memset(out, 0, sizeof(*out));

    uint16_t frctrl = (uint16_t)(sd[0] | (sd[1] << 8));
    out->frctrl     = frctrl;
    out->version    = (uint8_t)(frctrl >> 12);
    out->encrypted  = (uint8_t)((frctrl >> 3) & 1);
    out->mac_include= (uint8_t)((frctrl >> 4) & 1);
    out->cap_include= (uint8_t)((frctrl >> 5) & 1);
    out->mesh       = (uint8_t)((frctrl >> 7) & 1);
    out->registered = (uint8_t)((frctrl >> 8) & 1);
    out->auth_mode  = (uint8_t)((frctrl >> 10) & 3);
    out->pid        = (uint16_t)(sd[2] | (sd[3] << 8));

    size_t off = 4;
    if (len < 5) return -2;                     /* frame_cnt 缺失 */
    out->frame_cnt = sd[off++];

    if (out->mac_include) {
        if (len < off + 6) return -3;
        out->mac = sd + off;
        off += 6;
    }
    if (out->cap_include) {
        if (len < off + 1) return -3;
        out->capability = sd + off;
        off += 1;
    }
    out->objs = sd + off;
    out->objs_len = len - off;
    return 0;
}
