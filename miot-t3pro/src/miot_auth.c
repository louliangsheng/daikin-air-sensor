/**
 * @file miot_auth.c
 * @brief 米家 BLE 标准认证状态机实现（REGISTER + LOGIN）
 *
 * 所有报文格式、状态推进条件、应答通道均来自固件实测 + App 反汇编互证，
 * 注释里保留关键坑位说明（照抄勿改）。
 */
#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ================= 日志 ================= */

static void alog(const miot_auth_t *a, const char *fmt, ...)
{
    (void)a;   /* 预留：平台可在此接入 port 日志系统 */
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[miot-auth] %s\n", buf);
}

/* ================= legacy PSK 的 HKDF 标签（setup=首绑 / login=重连） =================
 * 与标准认证的 "mible-setup-info" / "mible-login-info" 同结构，只是标签不同。 */
static const char k_psk_salt[]       = "smartcfg-setup-salt";
static const char k_psk_info[]       = "smartcfg-setup-info";
static const char k_psk_login_salt[] = "smartcfg-login-salt";
static const char k_psk_login_info[] = "smartcfg-login-info";

/* ================= 底层发送 ================= */

static void n19(const miot_auth_t *a, const uint8_t *d, size_t n)
{
    if (a->port.notify_0019) a->port.notify_0019(a->port.user, d, n);
}

static void n10(const miot_auth_t *a, const uint8_t *d, size_t n)
{
    if (a->port.notify_0010) a->port.notify_0010(a->port.user, d, n);
}

static void result_code(const miot_auth_t *a, uint8_t code)
{
    uint8_t f[4] = { code, 0, 0, 0 };      /* ★ 结果码必须发 0x0010 */
    n10(a, f, 4);
}

/* ag2 ACK（status=1 就绪 / 0 收齐确认） */
static void ag_ack(miot_auth_t *a, uint8_t status)
{
    uint8_t f[4] = { 0x00, 0x00, 0x01, status };
    n19(a, f, 4);
}

/* channel data 包：[00 00][cmd=2][msgType][payload] —— 业务消息必须包这层，
 * 裸发会被 App 当管理包丢弃（-55 notify timeout 的根因） */
static void ch_send(miot_auth_t *a, uint8_t msg_type, const uint8_t *payload, size_t len)
{
    uint8_t buf[144];
    if (len + 4 > sizeof(buf)) return;
    buf[0] = 0x00; buf[1] = 0x00;
    buf[2] = 0x02;
    buf[3] = msg_type;
    if (len) memcpy(buf + 4, payload, len);
    n19(a, buf, len + 4);
}

/* ag2 SINGLE：[00 00][02][pkgType][payload]（不分片） */
static void ag_single(miot_auth_t *a, uint8_t pkg_type, const uint8_t *d, size_t n)
{
    (void)pkg_type;
    ch_send(a, 0, d, n);   /* device info 走 channel data 包 msgType=0 */
}

static void ag_parcel(miot_auth_t *a, uint8_t pkg_type, const uint8_t *d, size_t n)
{
    if (n > sizeof(a->tx_buf)) return;
    uint8_t cnt = (uint8_t)((n + 17) / 18);
    if (!cnt) cnt = 1;
    uint8_t start[6] = { 0x00, 0x00, 0x00, pkg_type, cnt, 0x00 };
    n19(a, start, 6);
    alog(a, "ag tx START pkg=%u cnt=%u (%uB)", (unsigned)pkg_type, (unsigned)cnt, (unsigned)n);
    /* ★2026-11-11 手机侧实证：App 对 START 只回一次 ACK(1)，**从不为数据帧回
     * ACK(0)** —— 说明我们节流 80ms 推的帧它根本没收到/没接受。
     * 参考工程（真机验证：64B 公钥、32B devConfirm、整条证书链）的做法是
     * START 之后**立即连发全部数据帧**，中间不留间隔。这里对齐参考实现。 */
    size_t off = 0;
    for (uint8_t i = 1; i <= cnt; i++) {
        size_t chunk = n - off;
        if (chunk > 18) chunk = 18;
        uint8_t f[20];
        f[0] = i; f[1] = 0x00;
        memcpy(f + 2, d + off, chunk);
        n19(a, f, 2 + chunk);
        off += chunk;
    }
    alog(a, "ag tx parcel done %u frames", (unsigned)cnt);
}

/* ================= 通用随机数 ================= */

static void auth_random(miot_auth_t *a, uint8_t *out, size_t len)
{
    if (a->port.random && a->port.random(a->port.user, out, len) == 0) return;
    for (size_t i = 0; i < len; i++) {          /* 伪随机退化 */
        out[i] = (uint8_t)(out[i] * 1103515245u + 12345u);
        out[i] ^= (uint8_t)(out[i] >> 3);
    }
}

/* ================= REGISTER ================= */

/* device info：channel data(msgType=0)，载荷 [ver_lo=2][ver_hi=0][caps=0][0]+did。
 * ★载荷必须 ≥4B，少一字节 App 报 -47（recvDeviceInfo data format error）。
 * ★did 回传前去前后导 0x00 —— 云端 did 是「左补零到 20B」，原样回传会被
 *   当成 did 字符导致 bindDidToServerV2 失败（App 写 0x14 后断开）。 */
static void send_device_info(miot_auth_t *a)
{
    uint8_t info[4 + 20];
    info[0] = 0x02; info[1] = 0x00; info[2] = 0x00; info[3] = 0x00;
    size_t n = 4, off = 0, dl = 20;
    while (off < dl && a->id.did[off] == 0x00) off++;
    while (dl > off && a->id.did[dl - 1] == 0x00) dl--;
    dl -= off;
    if (dl) memcpy(info + 4, a->id.did + off, dl), n += dl;
    /* ★必须走 ag2 分帧：4+did(20) = 24B，而 MTU 23 下单帧上限 20B。
     * 之前用 ag_single 直接发，被 GATT 层按 "len > mtu-3" 静默丢弃，
     * 表现为 App 收不到 device info、停在「连接超时」。 */
    if (n <= 16) ag_single(a, 0x00, info, n);
    else         ag_parcel(a, 0x00, info, n);
    alog(a, "device info sent (did=%uB, %uB payload)", (unsigned)dl, (unsigned)n);
}

void miot_auth_set_did(miot_auth_t *a, const uint8_t *did, size_t len)
{
    if (!a || !did || !len) return;
    if (len > sizeof(a->id.did)) len = sizeof(a->id.did);
    memset(a->id.did, 0, sizeof(a->id.did));
    memcpy(a->id.did, did, len);
    alog(a, "did set from 0x0013 (%uB)", (unsigned)len);
    if (a->port.save && a->bound) a->port.save(a->port.user, &a->id);
}

void miot_auth_prepare(miot_auth_t *a)
{
    if (a->key_ready) return;
    if (miot_ecdh_keygen(a->pubkey64) == 0) {
        a->key_ready = 1;
        alog(a, "ECDH keypair ready");
    } else {
        alog(a, "ECDH keygen FAIL (crypto backend registered?)");
    }
}

static void register_start(miot_auth_t *a)
{
    miot_auth_prepare(a);
    if (!a->key_ready) return;
    a->state = MIOT_AUTH_ST_REG_PUBKEY;
    a->ecdh_ok = 0;
    send_device_info(a);
    /* ★-55 兜底：App 收到 device info 才会回 0x15（要公钥）。这条 notify 一旦
     *   在手机侧没落地（首次连接时 ATT 还没完全就绪），App 干等 16s 就报
     *   「notify 超时」= -55。这里排一次重发窗口，poll 里按状态推进。 */
    a->reg_resend_tries = 1;
    a->reg_resend_at_ms = 1;            /* 1 = 待排期（poll 转成 now+150） */
}

/* 注册完成（收到 App 公钥）：ECDH → HKDF 派生身份 → 回公钥+devRandom+devConfirm */
static void register_on_app_pubkey(miot_auth_t *a, const uint8_t *pk64)
{
    uint8_t secret[32];
    static const uint8_t zeros16[16] = { 0 };
    if (miot_ecdh_shared(pk64, secret) != 0) {
        alog(a, "REGISTER: ECDH fail");
        return;
    }
    memcpy(a->shared, secret, 32);
    a->has_shared = 1;
    a->ecdh_ok = 1;

    /* token = out[0:12]，bind_key = [12:28]，dev_key = [28:44] */
    uint8_t out[64];
    miot_hkdf_sha256(secret, 32, zeros16, 16, "mible-setup-info", 16, out, 64);
    memcpy(a->id.token, out, 12);
    memcpy(a->id.bind_key, out + 12, 16);
    memcpy(a->id.dev_key, out + 28, 16);

    /* 回设备公钥（64B，去 04 前缀，ag2 分片 pkg=3） */
    ag_parcel(a, 0x03, a->pubkey64, 64);

    /* 紧接着 devRandom(0x0D) + devConfirm(0x0C)，App 才会算 token。
     * devConfirm = HMAC-SHA256(shared32, devRand || zeros16) */
    uint8_t devrand[16], body[32], mac[32];
    auth_random(a, devrand, 16);
    memcpy(body, devrand, 16);
    memcpy(body + 16, zeros16, 16);
    miot_hmac_sha256(a->shared, 32, body, 32, mac);
    ag_parcel(a, 0x0D, devrand, 16);
    ag_parcel(a, 0x0C, mac, 32);
    alog(a, "REGISTER: pubkey + devRandom + devConfirm sent");
    a->state = MIOT_AUTH_ST_REG_ENCDATA;
}

/* 92B 加密 did：AES-CCM(dev_key, iv=0x10..0x1B, aad="devID", tag4)，
 * 明文 88B = did(20) + cloud_sign(64) + utc(4)。试偏移 0/2/4（App 版本差异）。
 * ★解出并保存 did 才能让 App 重连时认出设备，否则每次连接都当新设备反复绑定 */
static void register_on_encrypted_did(miot_auth_t *a, const uint8_t *d, size_t len)
{
    static const uint8_t iv12[12] = { 0x10,0x11,0x12,0x13,0x14,0x15,
                                      0x16,0x17,0x18,0x19,0x1a,0x1b };
    uint8_t out[128];
    for (int off = 0; off <= 4; off += 2) {
        if ((size_t)off + 5 > len) break;
        size_t clen = len - (size_t)off;
        if (miot_ccm_decrypt(a->id.dev_key, iv12, (const uint8_t *)"devID", 5,
                             d + off, clen - 4, out) == 0) {
            memcpy(a->id.did, out, 20);
            memcpy(a->id.sign, out + 20, 64);
            memcpy(&a->id.utc, out + 84, 4);
            /* 云端已下发 did = 设备身份有效。★必须当场置 bound：
             * 否则本次上电一直是 bound=0，广播继续发未绑定心跳帧（无属性），
             * App/网关照收不到任何读数，要等重启从 NVS 重新 load 才恢复。 */
            a->bound = a->ecdh_ok ? 1 : 0;
            if (a->port.save) a->port.save(a->port.user, &a->id);
            alog(a, "REGISTER: did stored (utc=%u) bound=%d",
                 (unsigned)a->id.utc, a->bound);
            return;
        }
    }
    alog(a, "REGISTER: encrypted did decrypt FAIL (%uB)", (unsigned)len);
}

static void register_on_cert(miot_auth_t *a, size_t len)
{
    (void)len;
    /* 收到云证书必须回 [11 00 00 00]（0x0010），否则 App 一直显示设备初始化 */
    result_code(a, MIOT_AUTH_R_REG_OK);
    a->state = MIOT_AUTH_ST_IDLE;
    alog(a, "REGISTER COMPLETE (result 0x11 on 0x0010)");
}

static void ra_on_data(miot_auth_t *a, const uint8_t *d, size_t len)
{
    if (a->state == MIOT_AUTH_ST_REG_PUBKEY && len == 64) {
        register_on_app_pubkey(a, d);
        return;
    }
    if (a->state == MIOT_AUTH_ST_REG_ENCDATA && len == 16) {
        a->state = MIOT_AUTH_ST_REG_CERT;           /* writeEncryptData 暂存忽略 */
        return;
    }
    if ((a->state == MIOT_AUTH_ST_REG_CERT || a->state == MIOT_AUTH_ST_REG_ENCDATA) &&
        len == 92) {
        register_on_encrypted_did(a, d, len);        /* 米家 11.9.623：did 先于证书 */
        return;
    }
    if ((a->state == MIOT_AUTH_ST_REG_CERT || a->state == MIOT_AUTH_ST_REG_ENCDATA) &&
        (len == 24 || len == 163)) {
        register_on_cert(a, len);                    /* 24B 旧版 / 163B(11.9.623 ag2 pkg=7) */
        return;
    }
    if (len == 4 && d[0] == 0x13 && d[1] == 0 && d[2] == 0 && d[3] == 0) {
        /* bind result = 成功：提交 token/bind_key。★0x13 不是终点：
         * App 随后还发 92B did + 云证书，保持 CERT 态等回执 */
        a->bound = a->ecdh_ok ? 1 : 0;
        if (a->port.save) a->port.save(a->port.user, &a->id);
        alog(a, "*** STD REGISTER: bind result OK (bound=%d) ***", a->bound);
        a->state = MIOT_AUTH_ST_REG_CERT;
        return;
    }
    alog(a, "REGISTER: unhandled %uB (state=%d)", (unsigned)len, (int)a->state);
}

/* ================= LOGIN =================
 * ikm = token12；salt = appRand || devRand；
 * E = HKDF-SHA256(ikm, salt, "mible-login-info", 64)
 * devConfirm = HMAC(E[0:16],  devRand || appRand)
 * appConfirm = HMAC(E[16:32], appRand || devRand)   ← 两边数据顺序相反，照抄 */

static void login_start(miot_auth_t *a)
{
    a->state = MIOT_AUTH_ST_LOGIN_AWAIT_RAND;
    a->logged_in = 0;
    a->conf_pending = 0;
    alog(a, "CMD_LOGIN -> wait appRandom(0x0B)");

    /* PSK 重连：App 要先拿到设备公钥才能算 ECDH，所以这里主动发一帧 0x03。
     * 标准认证由 App 先给公钥，顺序相反，故只在 PSK 侧加这一帧。 */
    if (a->mode == MIOT_AUTH_MODE_PSK || a->psk_state != MIOT_PSK_IDLE) {
        miot_auth_prepare(a);
        if (a->key_ready) {
            ag_parcel(a, 0x03, a->pubkey64, 64);
            alog(a, "PSK LOGIN: dev pubkey(0x03) sent, wait app pubkey");
        }
    }
}

static void login_on_app_random(miot_auth_t *a, const uint8_t *app_rand)
{
    memcpy(a->app_rand, app_rand, 16);
    auth_random(a, a->dev_rand, 16);

    uint8_t salt[32];
    memcpy(salt, a->app_rand, 16);
    memcpy(salt + 16, a->dev_rand, 16);

    /* PSK 重连：与标准 LOGIN 同构，ikm 从 token(12B) 换成 ECDH 派生的
     * psk_key(32B)，标签换 smartcfg-login-*。其余字节序照抄标准分支。 */
    int psk = (a->mode == MIOT_AUTH_MODE_PSK) || (a->psk_ecdh_ok && a->bound);
    if (psk && a->psk_ecdh_ok) {
        miot_hkdf_sha256(a->psk_key, 32, salt, 32,
                         k_psk_login_info, sizeof(k_psk_login_info) - 1,
                         a->session, 64);
        alog(a, "LOGIN(PSK): session via smartcfg-login");
    } else {
        miot_hkdf_sha256(a->id.token, 12, salt, 32, "mible-login-info", 16, a->session, 64);
    }

    uint8_t body[32];
    memcpy(body, a->dev_rand, 16);          /* devRand 在前 */
    memcpy(body + 16, a->app_rand, 16);
    miot_hmac_sha256(a->session, 16, body, 32, a->conf);

    ag_parcel(a, 0x0D, a->dev_rand, 16);    /* 先发 devRandom */
    /* devConfirm 延迟 120ms 首发（让 0x0D 的 ACK 先走完），之后 400ms 重发 ×4。
     * ★不重发时 App 偶尔收不到 0x0C，15s 后按 notify timeout 断开 */
    a->conf_pending = 1;
    a->conf_tries = 0;
    a->conf_deadline_ms = 0;                /* 由 poll 按相对时间推进（见下） */
    a->state = MIOT_AUTH_ST_LOGIN_SENT_CONF;
    alog(a, "LOGIN: devRandom(0x0D) sent, devConfirm staged (0x0C)");
}

static void login_send_conf(miot_auth_t *a)
{
    ag_parcel(a, 0x0C, a->conf, 32);
    a->conf_tries++;
    alog(a, "LOGIN: devConfirm sent (#%d)", a->conf_tries);
}

static void login_on_app_confirm(miot_auth_t *a, const uint8_t *d, size_t len)
{
    uint8_t rands[32], mac[32];
    memcpy(rands, a->app_rand, 16);         /* appRand 在前 */
    memcpy(rands + 16, a->dev_rand, 16);
    miot_hmac_sha256(a->session + 16, 16, rands, 32, mac);

    bool ok = (len >= 32 && memcmp(d, mac, 32) == 0) ||
              (len >= 16 && memcmp(d, mac, 16) == 0);
    if (!ok) {
        alog(a, "LOGIN: app confirm MISMATCH (len=%u)", (unsigned)len);
        result_code(a, MIOT_AUTH_R_LOGIN_FAIL);          /* 0xE2 → App -52 */
        a->state = MIOT_AUTH_ST_IDLE;
        a->conf_pending = 0;
        return;
    }
    a->conf_pending = 0;
    a->logged_in = 1;
    a->state = MIOT_AUTH_ST_IDLE;
    result_code(a, MIOT_AUTH_R_LOGIN_OK);                /* 0x21 → App 连接完成 */
    alog(a, "*** STD LOGIN COMPLETE (0x21 on 0x0010) ***");
}

/* ================= legacy PSK =================
 *
 * 与标准认证并列的一条链，不是替代：同一 ECDH P-256，只把 HKDF 标签从
 * "mible-setup-*" 换成 "smartcfg-setup-*"，并在握手末尾多一帧 0x04 dev_sign。
 * 数据面（session_key / AES-CCM）与标准认证完全一致，直接复用。
 *
 * ★ 本段依据 App 静态分析实现，**尚未实机验证**（见 docs/psk-auth.md）。
 *   三处不确定项全部做成显式变体 + trace，实测一次即可定死，不需要改结构：
 *     - 0x04 签什么        → psk_sign_variant（默认 DEV_APP_RAND）
 *     - 启动帧 26B 的布局  → 原样留存并打印，不解析
 *     - App 公钥何时到达   → 收到 64B 即算，appRand 晚到则重算重发
 */

/* 原始帧 hex（仅 psk_trace 打开时；实测抓帧靠它，不需要任何 App 侧工具） */
static void psk_hex(const miot_auth_t *a, const char *tag, const uint8_t *d, size_t n)
{
    if (!a->psk_trace) return;
    char line[96];
    size_t p = 0;
    p += (size_t)snprintf(line + p, sizeof(line) - p, "[psk] %s %uB:", tag, (unsigned)n);
    for (size_t i = 0; i < n && p + 4 < sizeof(line); i++)
        p += (size_t)snprintf(line + p, sizeof(line) - p, " %02X", d[i]);
    printf("%s\n", line);
}

/* PSK 派生：ikm = ECDH 共享密钥。
 * 首绑用 smartcfg-setup-*；已绑定后的重连用 smartcfg-login-*（同结构换标签）。
 * ★重连标签是推断（见 docs/psk-auth.md §4.3），实测对不上时改这里一处即可。 */
static void psk_derive(miot_auth_t *a, const uint8_t secret[32])
{
    int relogin = a->bound;   /* 有持久身份 = 重连，用 login 标签 */
    miot_hkdf_sha256(secret, 32,
                     (const uint8_t *)(relogin ? k_psk_login_salt : k_psk_salt),
                     relogin ? sizeof(k_psk_login_salt) - 1 : sizeof(k_psk_salt) - 1,
                     relogin ? k_psk_login_info : k_psk_info,
                     relogin ? sizeof(k_psk_login_info) - 1 : sizeof(k_psk_info) - 1,
                     a->psk_key, 64);
    alog(a, "PSK: kdf=%s", relogin ? "smartcfg-login" : "smartcfg-setup");
}

/* dev_sign：三种候选输入变体，实测时用 miot_auth_set_psk_options 切换 */
static void psk_make_sign(miot_auth_t *a)
{
    uint8_t body[80];
    size_t n;
    switch (a->psk_sign_variant) {
    case MIOT_PSK_SIGN_DEV_RAND:
        memcpy(body, a->psk_dev_rand, 16); n = 16;
        break;
    case MIOT_PSK_SIGN_SHARED_RAND:
        memcpy(body, a->shared, 32);
        memcpy(body + 32, a->psk_dev_rand, 16);
        memcpy(body + 48, a->psk_app_rand, 16);
        n = 64;
        break;
    case MIOT_PSK_SIGN_DEV_APP_RAND:
    default:
        memcpy(body, a->psk_dev_rand, 16);
        memcpy(body + 16, a->psk_app_rand, 16);
        n = 32;
        break;
    }
    if (miot_ecdsa_sign(body, n, a->psk_sign) != 0) {
        a->psk_sign_len = 0;
        alog(a, "PSK: ecdsa backend missing -> dev_sign skipped (variant %d)",
             a->psk_sign_variant);
        return;
    }
    a->psk_sign_len = 64;
}

/* devConfirm：与标准认证同构（HMAC(dev_key, devRand||appRand)），
 * ★若实测 App 校验不通过，优先怀疑这里应改用 app_key —— 已留 trace。 */
static void psk_make_confirm(miot_auth_t *a, uint8_t out[32])
{
    uint8_t body[32];
    memcpy(body, a->psk_dev_rand, 16);
    memcpy(body + 16, a->psk_app_rand, 16);
    miot_hmac_sha256(a->psk_key, 32, body, 32, out);
}

/* 发握手五帧：info(0x00) → devRand(0x0D) → devPub(0x03) → confirm(0x0C) → sign(0x04) */
static void psk_send_handshake(miot_auth_t *a)
{
    uint8_t conf[32];
    /* 启动帧到达时已回过 device info；App 直接给公钥（跳过启动帧）时才补发 */
    if (!a->psk_start_len) send_device_info(a);
    ag_parcel(a, 0x0D, a->psk_dev_rand, 16);
    ag_parcel(a, 0x03, a->pubkey64, 64);
    psk_make_confirm(a, conf);
    ag_parcel(a, 0x0C, conf, 32);
    if (a->psk_sign_len == 64) ag_parcel(a, 0x04, a->psk_sign, 64);
    a->psk_state = MIOT_PSK_WAIT_SERVER_PUB;
    alog(a, "PSK: handshake sent (info/0D/03/0C%s)", a->psk_sign_len ? "/04" : "");
}

/* 收到 App 公钥：ECDH → 派生 → 出全套握手帧 */
static void psk_on_app_pubkey(miot_auth_t *a, const uint8_t *pk64)
{
    uint8_t secret[32];
    if (miot_ecdh_shared(pk64, secret) != 0) {
        alog(a, "PSK: ECDH fail");
        return;
    }
    memcpy(a->shared, secret, 32);
    a->has_shared = 1;
    a->ecdh_ok = 1;
    a->psk_ecdh_ok = 1;
    psk_derive(a, secret);

    /* 重连：密钥就绪即停，握手帧由 login_on_app_random 统一发，
     * 否则会和标准 LOGIN 的 devRand/confirm 重发两张皮 */
    if (a->state == MIOT_AUTH_ST_LOGIN_AWAIT_RAND) {
        alog(a, "PSK LOGIN: app pubkey received, keys ready");
        return;
    }

    auth_random(a, a->psk_dev_rand, 16);
    psk_make_sign(a);
    psk_send_handshake(a);
    alog(a, "PSK: ECDH ok, keys derived (smartcfg-setup)");
}

/* 启动帧（26B）：原样留存 + 打印，不解析——布局待实测确认 */
static void psk_on_start(miot_auth_t *a, const uint8_t *d, size_t len)
{
    a->psk_start_len = len > sizeof(a->psk_start) ? sizeof(a->psk_start) : len;
    memcpy(a->psk_start, d, a->psk_start_len);
    a->psk_state = MIOT_PSK_WAIT_START;
    a->logged_in = 0;              /* bound 保持不动：它决定用 setup 还是 login 标签 */
    alog(a, "PSK: start frame %uB captured (waiting app pubkey)", (unsigned)len);
    psk_hex(a, "start", d, a->psk_start_len);
    send_device_info(a);                       /* 先回 device info，App 才会继续 */
}

/* 16B：服务端公钥。到达即说明 App 已认可握手，进入等绑定结果 */
static void psk_on_server_pub(miot_auth_t *a, const uint8_t *d, size_t len)
{
    memcpy(a->psk_server_pub, d, len > 16 ? 16 : len);
    a->psk_state = MIOT_PSK_WAIT_BIND;
    alog(a, "PSK: server pub received -> wait bind result");
}

/* 13B 绑定结果：尾部可能是 did（ASCII 数字），能认出来就落盘 */
static void psk_on_bind_result(miot_auth_t *a, const uint8_t *d, size_t len)
{
    const uint8_t *p = d;
    size_t n = len;
    while (n && *p == 0x00) { p++; n--; }
    while (n && p[n - 1] == 0x00) n--;

    int all_digit = n > 0 && n <= 20;
    for (size_t i = 0; i < n && all_digit; i++)
        if (p[i] < '0' || p[i] > '9') all_digit = 0;

    if (all_digit) {
        memset(a->id.did, 0, sizeof(a->id.did));
        memcpy(a->id.did, p, n);
        a->bound = 1;
        if (a->port.save) a->port.save(a->port.user, &a->id);
        alog(a, "PSK: *** BOUND, did stored (%uB) ***", (unsigned)n);
    } else {
        /* 数字串认不出来时仍按成功处理：did 会在下次 LOGIN/查询时补齐，
         * 但不能让绑定流程卡死 */
        a->bound = a->psk_ecdh_ok ? 1 : 0;
        alog(a, "PSK: bind result %uB (did not parsed, bound=%d)", (unsigned)len, a->bound);
    }
    a->psk_state = MIOT_PSK_DONE;
    result_code(a, MIOT_AUTH_R_REG_OK);        /* 与标准认证同一个 A4 结果通道 */
}

/* PSK 收帧总入口（已在 0x0019 解出业务载荷后调用） */
static int psk_on_payload(miot_auth_t *a, const uint8_t *d, size_t len)
{
    psk_hex(a, "rx", d, len);

    if (len == 64) { psk_on_app_pubkey(a, d); return 1; }
    if (len == 16) {
        if (a->psk_state == MIOT_PSK_WAIT_START) {
            /* appRand 晚于公钥到达：重算 confirm/sign 并重发，覆盖顺序差异 */
            memcpy(a->psk_app_rand, d, 16);
            if (a->psk_ecdh_ok) { psk_make_sign(a); psk_send_handshake(a); }
            alog(a, "PSK: appRand arrived late -> handshake resent");
        } else {
            psk_on_server_pub(a, d, len);
        }
        return 1;
    }
    if (len == 26 || len == 28) {              /* 启动帧（含/不含 2B 前置头） */
        if (a->psk_state == MIOT_PSK_IDLE || a->psk_state == MIOT_PSK_WAIT_START) {
            psk_on_start(a, d, len);
            return 1;
        }
    }
    if (len == 13) { psk_on_bind_result(a, d, len); return 1; }
    if (len == 4 && d[0] == 0x13) {            /* 与标准认证共用的 bind ok 短帧 */
        psk_on_bind_result(a, d, len);
        return 1;
    }
    return 0;
}

void miot_auth_set_mode(miot_auth_t *a, miot_auth_mode_t mode)
{
    a->mode = mode;
    if (mode == MIOT_AUTH_MODE_PSK) a->psk_state = MIOT_PSK_WAIT_START;
}

void miot_auth_set_psk_options(miot_auth_t *a, int trace, int sign_variant)
{
    a->psk_trace = trace;
    a->psk_sign_variant = sign_variant;
}

/* ================= 0x0019 三层解析 ================= */

static void dispatch_payload(miot_auth_t *a, uint8_t pkg, const uint8_t *d, size_t len)
{
    /* ---- legacy PSK 优先（它和标准认证用同一批操作码，必须先分流） ---- */
    if (a->mode == MIOT_AUTH_MODE_PSK) {
        /* 重连：等的是 App 公钥，不是 appRandom */
        if (a->state == MIOT_AUTH_ST_LOGIN_AWAIT_RAND && len == 64) {
            psk_on_app_pubkey(a, d);
            return;
        }
        /* 重连的挑战应答与标准 LOGIN 同构，只换 KDF 的 ikm 与标签，
         * 所以这两个状态直接落到下面的标准分支（见 login_on_app_random） */
        if (a->state != MIOT_AUTH_ST_LOGIN_AWAIT_RAND &&
            a->state != MIOT_AUTH_ST_LOGIN_SENT_CONF) {
            if (!psk_on_payload(a, d, len))
                alog(a, "PSK: unhandled %uB (psk_state=%d)", (unsigned)len, (int)a->psk_state);
            return;
        }
    } else if (a->psk_state != MIOT_PSK_IDLE && psk_on_payload(a, d, len)) {
        return;
    }
    /* AUTO：标准流程还没起、突然来了启动帧 → 判定为 PSK 设备 */
    if (a->mode == MIOT_AUTH_MODE_AUTO && a->psk_state == MIOT_PSK_IDLE &&
        (len == 26 || len == 28) &&
        (a->state == MIOT_AUTH_ST_IDLE || a->state == MIOT_AUTH_ST_REG_PUBKEY)) {
        alog(a, "AUTO: PSK start frame detected -> switch to PSK chain");
        psk_on_payload(a, d, len);
        return;
    }

    if (a->state == MIOT_AUTH_ST_REG_PUBKEY ||
        a->state == MIOT_AUTH_ST_REG_ENCDATA ||
        a->state == MIOT_AUTH_ST_REG_CERT) {
        ra_on_data(a, d, len);
        return;
    }
    if (a->state == MIOT_AUTH_ST_LOGIN_AWAIT_RAND && pkg == 0x0B && len >= 16) {
        login_on_app_random(a, d);
        return;
    }
    if (a->state == MIOT_AUTH_ST_LOGIN_SENT_CONF && pkg == 0x0A) {
        login_on_app_confirm(a, d, len);
        return;
    }
    alog(a, "dispatch: no consumer (pkg=%u len=%u state=%d)",
         (unsigned)pkg, (unsigned)len, (int)a->state);
}

static void on_ch_data(miot_auth_t *a, uint8_t seq, const uint8_t *d, size_t len)
{
    /* legacy PSK：channel data 是它唯一的数据面，全部交给 PSK 判定 */
    if (a->mode == MIOT_AUTH_MODE_PSK) { psk_on_payload(a, d, len); return; }
    if (a->psk_state != MIOT_PSK_IDLE && psk_on_payload(a, d, len)) return;

    /* 注册：channel data 直达业务层 */
    if (a->state == MIOT_AUTH_ST_REG_PUBKEY && len == 64) { ra_on_data(a, d, len); return; }
    if (a->state == MIOT_AUTH_ST_REG_ENCDATA && len == 16) { ra_on_data(a, d, len); return; }
    if (a->state == MIOT_AUTH_ST_REG_CERT && len == 24)    { ra_on_data(a, d, len); return; }
    /* 登录：短包走 channel single（seq=业务 pkg） */
    if ((a->state == MIOT_AUTH_ST_LOGIN_AWAIT_RAND && seq == 0x0B && len >= 16) ||
        (a->state == MIOT_AUTH_ST_LOGIN_SENT_CONF && seq == 0x0A && len >= 16)) {
        dispatch_payload(a, seq, d, len);
        return;
    }
    alog(a, "ch data seq=%u len=%u unhandled (state=%d)", (unsigned)seq,
         (unsigned)len, (int)a->state);
}

/* App 通道建链测试包：16 字节、每字节 = 长度+2（即 0x12）。校验通过回同款 */
static void mng_test_reply(miot_auth_t *a)
{
    uint8_t t[4 + 16];
    t[0] = 0x00; t[1] = 0x00; t[2] = 0x05; t[3] = 0x01;
    memset(t + 4, 18, 16);                  /* 每字节 = 16+2 */
    n19(a, t, sizeof(t));
    a->chan_ready = 1;
    alog(a, "channel READY (mng_ack test 16x0x12 sent)");
    /* ★device info 的时机（2026-11-11 手机侧实证修正）：
     * App 从 connect 到 cancelOpen 只有约 990ms（固定硬窗口）。之前这里排期
     * 150ms 后才发，恰好赶在它放弃之后 —— 手机日志里 START/数据帧一次都没
     * 出现（onNotify 只有 6B 的 mng 和 20B 的测试包）。
     * 现在：优先在收到 App 测试包回执(code=1)的瞬间发（那才是它真正就绪的
     * 时刻）；回执不来则由 poll 在 250ms 兜底。 */
    a->info_sent = 0;
    a->info_at_ms = 0;                      /* 0 = 未排期，交给回执/兜底 */
}

void miot_auth_on_0019(miot_auth_t *a, const uint8_t *d, size_t len)
{
    psk_hex(a, "0019", d, len);

    /* ---- 第 1 层：ag2 控制帧 [00 00][type][...] ---- */
    if (len >= 3 && d[0] == 0x00 && d[1] == 0x00) {
        uint8_t type = d[2];
        if (type == 0x00 && len >= 6) {                     /* START */
            a->ag_pkg = d[3];
            a->ag_expect = d[4];
            a->ag_got = 0;
            a->ag_len = 0;
            ag_ack(a, 0x01);                 /* ★必须 ACK 就绪，否则 App 5s 超时重发 a2 → -28 */
            return;
        }
        if (type == 0x01 && len >= 4) {                     /* App 对我们数据的 ACK */
            /* 分片已改为「START 后立即连发」，不再依赖 ACK 驱动。 */
            return;
        }
        if (type == 0x02 && len >= 4) {                     /* SINGLE */
            dispatch_payload(a, d[3], d + 4, len - 4);
            return;
        }
        if (type == 0x03) return;                           /* SACK */
        if (type == 0x05) {                                 /* mng_ack */
            uint8_t code = (len >= 4) ? d[3] : 0xFF;
            if (code == 0x00 && !a->chan_ready) mng_test_reply(a);
            else if (code == 0x01) {
                /* App 回执测试包 → 建链这关过了。此刻它才准备好收业务帧，
                 * 立刻把 device info 交过去，它才能判定「我是哪台设备」
                 * 并决定走哪条认证分支。 */
                if (!a->info_sent) { a->info_sent = 1; a->info_at_ms = 0; send_device_info(a); }
            }
            return;
        }
        if (type == 0x04) {                                 /* mng：code=1 是 App 测试包 */
            uint8_t code = (len >= 4) ? d[3] : 0xFF;
            if (code == 0x01) mng_test_reply(a);
            return;
        }
        /* 其余 type 落到 channel 解析 */
    }

    /* ---- 第 2 层：ag2 数据帧 [fno 1B][00][chunk<=18] ---- */
    if (len >= 2 && d[1] == 0x00 && d[0] >= 1 && d[0] <= 64 && a->ag_expect > 0) {
        size_t chunk = len - 2;
        if (a->ag_len + chunk > sizeof(a->ag_buf)) { a->ag_len = 0; a->ag_got = 0; return; }
        memcpy(a->ag_buf + a->ag_len, d + 2, chunk);
        a->ag_len += chunk;
        a->ag_got++;
        /* ★按帧号判收齐（d[0]==expect 即末帧），不用计数——163B 证书 22 帧
         * 时实测 got 会少 1，导致永远不 ACK、App 干等 3.6s 重发 */
        if (d[0] < a->ag_expect) return;
        a->ag_expect = 0;
        ag_ack(a, 0x00);
        dispatch_payload(a, a->ag_pkg, a->ag_buf, a->ag_len);
        return;
    }

    /* ---- 第 3 层：channel 包 [00 00][cmd][seq][payload] ---- */
    if (len >= 4 && d[0] == 0x00 && d[1] == 0x00) {
        uint8_t cmd = d[2], seq = d[3];
        if (cmd == 0x02) { on_ch_data(a, seq, d + 4, len - 4); return; }
        if (cmd == 0x03) return;                            /* ack */
        if (cmd == 0x05) { n19(a, d, len); return; }        /* 建链参数回显 */
        /* cmd=0x04 等忽略 */
        return;
    }

    /* ---- 兜底：无 channel 头的裸包（旧版 App 路径） ---- */
    if (a->state == MIOT_AUTH_ST_REG_PUBKEY && len == 64) {
        ra_on_data(a, d, len);
        return;
    }
    alog(a, "0019 unhandled %uB (state=%d)", (unsigned)len, (int)a->state);
}

/* ================= 0x0010 命令 ================= */

void miot_auth_on_0010(miot_auth_t *a, const uint8_t *d, size_t len)
{
    psk_hex(a, "0010", d, len);
    if (len == 1 && d[0] == MIOT_AUTH_CMD_MTU) {
        /* 通道建链入口：回 mng(code=0) [num][dmtu]。
         * ★dmtu 必须给非 0（实测 num=6 dmtu=18），给 0 会算出负长度测试包，
         * 整条建链走不下去 —— App「设备初始化卡住」的起点 */
        uint8_t mtu[6] = { 0, 0, 0x04, 0x00, 0x06, 0x12 };
        n19(a, mtu, 6);
        a->chan_ready = 0;
        alog(a, "a4 -> mng(code=0) num=6 dmtu=18");
        return;
    }
    if (len == 4 && d[0] == MIOT_AUTH_CMD_LOGIN && d[1] == 0 && d[2] == 0 && d[3] == 0) {
        login_start(a);
        return;
    }
    if (len == 4 && (d[0] & 0x7F) == 0x22 && d[1] == 0 && d[2] == 0 && d[3] == 0) {
        /* 0xA2 = CMD_REGISTER(0x22|0x80)。★无论本地是否已绑定都走注册流程：
         * App 写 a2 后一定等 device info，把它当 login 只会 16s 超时报 -55 */
        alog(a, "CMD_REGISTER(0x%02X) -> register flow", d[0]);
        if (a->state == MIOT_AUTH_ST_IDLE) register_start(a);
        else { a->state = MIOT_AUTH_ST_REG_PUBKEY; send_device_info(a);
               a->reg_resend_tries = 1; a->reg_resend_at_ms = 1; }
        return;
    }
    if (len == 4 && d[0] == 0x13 && d[1] == 0 && d[2] == 0 && d[3] == 0) {
        /* bind result 成功。★真机 App 把它发在 0x0010 上（不是 0x0019），
         * 只认 0x0019 会让这一步被静默丢弃，bound 永远不置位 */
        a->bound = a->ecdh_ok ? 1 : 0;
        if (a->port.save) a->port.save(a->port.user, &a->id);
        alog(a, "*** STD REGISTER(0x0010): bind result OK (bound=%d) ***", a->bound);
        a->state = MIOT_AUTH_ST_REG_CERT;
        return;
    }
}

/* ================= 周期轮询 ================= */

void miot_auth_poll(miot_auth_t *a, uint32_t now_ms)
{
    /* device info 兜底排期：正常由 App 的测试包回执(code=1)立即触发；
     * 回执不来时 250ms 后自行发出（App 的窗口只有约 990ms，不能更久）。 */
    if (a->chan_ready && !a->info_sent) {
        if (a->info_at_ms == 0) {
            a->info_at_ms = now_ms + 250;
        } else if ((int32_t)(now_ms - a->info_at_ms) >= 0) {
            a->info_sent = 1;
            a->info_at_ms = 0;
            alog(a, "device info fired (fallback 250ms, no echo)");
            send_device_info(a);
        }
    }

    /* REGISTER 阶段 device info 重发：150ms 后仍停在「等 App 公钥」说明上一
     * 条 notify 手机侧没收到（-55 现场：设备侧 rc=0、App 侧零反应）。重发最多
     * 3 次，App 对重复的 device info 是幂等解析，不会误判。 */
    if (a->state == MIOT_AUTH_ST_REG_PUBKEY && a->reg_resend_at_ms) {
        if (a->reg_resend_at_ms == 1) {
            a->reg_resend_at_ms = now_ms + 150;
        } else if ((int32_t)(now_ms - a->reg_resend_at_ms) >= 0) {
            if (a->reg_resend_tries < 4) {
                a->reg_resend_tries++;
                a->reg_resend_at_ms = now_ms + 400;
                alog(a, "device info RESEND #%d (no app pubkey yet)",
                     a->reg_resend_tries - 1);
                send_device_info(a);
            } else {
                a->reg_resend_at_ms = 0;
            }
        }
    }

    if (a->state != MIOT_AUTH_ST_LOGIN_SENT_CONF || !a->conf_pending) return;
    if (a->conf_tries >= 4) { a->conf_pending = 0; return; }

    /* 首发延迟 120ms，重发间隔 400ms。用简单状态推进：
     * deadline==0 表示从未发过 → 立即记 deadline；到点发送 */
    if (a->conf_deadline_ms == 0) {
        a->conf_deadline_ms = now_ms + 120;
        return;
    }
    if ((int32_t)(now_ms - a->conf_deadline_ms) >= 0) {
        login_send_conf(a);
        a->conf_deadline_ms = now_ms + 400;
    }
}

/* ================= 生命周期 ================= */

void miot_auth_init(miot_auth_t *a, const miot_auth_port_t *port)
{
    memset(a, 0, sizeof(*a));
    a->port = *port;
    a->mode = MIOT_AUTH_MODE_AUTO;          /* 两种链都会说，由 App 的帧决定走哪条 */
    a->psk_state = MIOT_PSK_IDLE;
    a->psk_sign_variant = MIOT_PSK_SIGN_DEV_APP_RAND;
    if (a->port.load) {
        if (a->port.load(a->port.user, &a->id)) a->bound = 1;
    }
}

void miot_auth_on_disconnect(miot_auth_t *a)
{
    a->logged_in = 0;
    a->state = MIOT_AUTH_ST_IDLE;
    a->conf_pending = 0;
    a->ag_len = 0;
    a->ag_expect = 0;
    a->ag_got = 0;
    a->chan_ready = 0;
    a->info_sent = 0;                  /* 新连接要重新交一次 device info */
    a->info_at_ms = 0;
    a->tx_n = 0;
    a->tx_i = 0;
    memset(a->session, 0, sizeof(a->session));

    /* PSK 子状态复位：AUTO 下回到 IDLE（下次由启动帧重新判定），
     * 纯 PSK 模式直接等下一轮启动帧；psk_key / bound 都不动。 */
    a->psk_state = (a->mode == MIOT_AUTH_MODE_PSK) ? MIOT_PSK_WAIT_START : MIOT_PSK_IDLE;
    a->psk_sign_len = 0;
    a->psk_start_len = 0;
    memset(a->psk_app_rand, 0, sizeof(a->psk_app_rand));
}
