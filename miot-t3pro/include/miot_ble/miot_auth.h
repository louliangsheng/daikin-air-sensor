/**
 * @file miot_auth.h
 * @brief 米家 BLE 标准认证（REGISTER 绑定 + LOGIN 会话）状态机
 *
 * 完整镜像米家 App 端 BleStandardAuthRegisterConnector / LoginConnector 协议，
 * 全部流程经米家 Android 8.x 实机互通验证（含 11.9.623 版本的通道握手变体）。
 *
 * GATT 通道分工（移植方需提供两个可 notify 的特征值）：
 *   0x0019  认证数据通道 —— ag2 分帧 + channel 包 + 业务载荷
 *   0x0010  结果码通道   —— App 只认这里的 [0x11]/[0x21] 等结果（关键坑！
 *                           发在 0x0019 上 App 永远不判成功）
 *
 * 用法：
 * @code
 *   miot_auth_init(&auth, &port);
 *   miot_auth_prepare(&auth);                  // 建议开机时预生成 ECDH 密钥
 *   // GATT 写回调：
 *   void on_0010_write(d, n) { miot_auth_on_0010(&auth, d, n); }
 *   void on_0019_write(d, n) { miot_auth_on_0019(&auth, d, n); }
 *   // 主循环 50~100ms 调一次（devConfirm 重发）：
 *   miot_auth_poll(&auth, now_ms());
 *   // LOGIN 成功后：
 *   miot_device_set_session_keys(&dev, miot_auth_session_keys(&auth));
 * @endcode
 */
#ifndef MIOT_AUTH_H
#define MIOT_AUTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 0x0010 结果码（App connector 只在 0x0010 上判定） ---- */
#define MIOT_AUTH_R_REG_OK     0x11   /* 注册成功（收到云证书后发） */
#define MIOT_AUTH_R_LOGIN_OK   0x21   /* 登录成功 */
#define MIOT_AUTH_R_LOGIN_FAIL 0xE2   /* 登录失败（confirm 不符） */
#define MIOT_AUTH_R_REG_FAIL   0x22   /* 注册失败 */
#define MIOT_AUTH_CMD_LOGIN    0x24   /* App → CMD_LOGIN */
#define MIOT_AUTH_CMD_REG      0xA2   /* App → CMD_REGISTER (0x22|0x80) */
#define MIOT_AUTH_CMD_MTU      0xA4   /* App → 通道 MTU 初始化 */

/** 绑定身份（注册完成后持久化；重连时 load 回来） */
typedef struct miot_auth_identity {
    uint8_t  token[12];        /* HKDF("mible-setup-info")[0:12] */
    uint8_t  bind_key[16];     /* [12:28]，beacon 广播加密用 */
    uint8_t  dev_key[16];      /* [28:44]，加密 did 解密用 */
    uint8_t  did[20];          /* 云端设备 ID（92B 加密帧解出） */
    uint8_t  sign[64];         /* cloud sign */
    uint32_t utc;
    uint16_t pid;              /* 产品 ID */
    uint8_t  mac[6];           /* 空中序 */
} miot_auth_identity_t;

/** 移植回调集合 */
typedef struct miot_auth_port {
    void *user;
    /** 0x0019 认证数据通道 notify（必实现） */
    int  (*notify_0019)(void *user, const uint8_t *d, size_t len);
    /** 0x0010 结果码通道 notify（必实现） */
    int  (*notify_0010)(void *user, const uint8_t *d, size_t len);
    /** 身份持久化（可 NULL；NULL 时重启需重新绑定） */
    void (*save)(void *user, const miot_auth_identity_t *id);
    bool (*load)(void *user, miot_auth_identity_t *id);
    /** 真随机数（devRandom 用；可 NULL 时退化为伪随机） */
    int  (*random)(void *user, uint8_t *out, size_t len);
} miot_auth_port_t;

/* 状态机状态（调试可读） */
typedef enum {
    MIOT_AUTH_ST_IDLE = 0,
    MIOT_AUTH_ST_REG_PUBKEY,     /* 等待 App 64B 公钥 */
    MIOT_AUTH_ST_REG_ENCDATA,    /* 公钥已交换，等 16B encrypt-data */
    MIOT_AUTH_ST_REG_CERT,       /* 等加密 did(92B) / 云证书(24|163B) */
    MIOT_AUTH_ST_LOGIN_AWAIT_RAND, /* 登录：等 appRandom(0x0B) */
    MIOT_AUTH_ST_LOGIN_SENT_CONF,  /* 登录：devConfirm 已发，等 appConfirm(0x0A) */
} miot_auth_state_t;

/** 认证模式。走哪条由 App 按型号决定，固件只能「两种都会说」。 */
typedef enum {
    MIOT_AUTH_MODE_AUTO = 0,   /* 默认：标准认证为主，PSK 启动帧到达时自动切入 */
    MIOT_AUTH_MODE_STANDARD,   /* 只走标准认证（ECDH + REGISTER/LOGIN） */
    MIOT_AUTH_MODE_PSK,        /* 只走 legacy PSK（驱蚊器 / 夜灯重绑） */
} miot_auth_mode_t;

/** PSK 子状态（仅 mode=AUTO|PSK 时有效） */
typedef enum {
    MIOT_PSK_IDLE = 0,
    MIOT_PSK_WAIT_START,        /* 等 App 启动帧 / 公钥 */
    MIOT_PSK_WAIT_SERVER_PUB,   /* 握手 5 帧已发完，等 16B server pub */
    MIOT_PSK_WAIT_BIND,         /* 等 13B 绑定结果 */
    MIOT_PSK_DONE,              /* 绑定完成 */
} miot_psk_state_t;

/** 0x04 dev_sign 的签名输入变体（见 docs/psk-auth.md §5，实测时切换） */
#define MIOT_PSK_SIGN_DEV_APP_RAND  0   /* SHA256(devRand || appRand) */
#define MIOT_PSK_SIGN_DEV_RAND      1   /* SHA256(devRand) */
#define MIOT_PSK_SIGN_SHARED_RAND   2   /* SHA256(shared || devRand || appRand) */

typedef struct miot_auth {
    /* 配置 */
    miot_auth_port_t port;
    miot_auth_mode_t mode;             /* 默认 AUTO */
    int      psk_trace;                /* 1 = 打印 0x0010/0x0019 原始帧（实测用） */
    int      psk_sign_variant;         /* MIOT_PSK_SIGN_*，默认 0 */

    /* 对外状态 */
    miot_auth_state_t    state;
    miot_auth_identity_t id;
    int  bound;                        /* 已绑定（身份有效） */
    int  logged_in;                    /* 会话密钥有效 */
    uint8_t session[64];               /* LOGIN 派生 E：Ekey|Dkey|ivTx|ivRx|… */

    /* 内部：REGISTER */
    uint8_t pubkey64[64];
    int     key_ready;
    uint8_t shared[32];
    int     has_shared;
    int     ecdh_ok;

    /* 内部：LOGIN */
    uint8_t app_rand[16];
    uint8_t dev_rand[16];
    uint8_t conf[32];
    int     conf_pending;              /* devConfirm 待发/待重发 */
    uint32_t conf_deadline_ms;
    int     conf_tries;

    /* 内部：0x0019 ag2 重组 */
    uint8_t  ag_buf[208];              /* 163B 云证书必须装得下 */
    size_t   ag_len;
    uint8_t  ag_pkg, ag_expect, ag_got;
    int      chan_ready;
    int      info_sent;                /* 本次连接的 device info 已交出去（防重发） */
    uint32_t info_at_ms;               /* device info 的发送时刻（1=pending 未排期） */
    int      reg_resend_tries;         /* REGISTER 阶段 device info 已重发次数 */
    uint32_t reg_resend_at_ms;         /* 下一次重发时刻；1 = 待排期（poll 里改成 now+100） */

    /* 内部：ag2 分片发送队列（START → 等 App ACK(1) → 逐帧发 → 每帧等 ACK(0)）。
     * 一次性把 START 和数据帧全推出去，App 还没就绪，数据帧等于白发。 */
    uint8_t  tx_buf[64];
    uint8_t  tx_len, tx_n, tx_i;
    uint32_t tx_ms;                    /* 下一帧的发送时刻（poll 驱动） */

    /* 内部：legacy PSK（与标准认证互不影响） */
    miot_psk_state_t psk_state;
    uint8_t  psk_key[64];              /* dev_key(32) || app_key(32) */
    uint8_t  psk_dev_rand[16];
    uint8_t  psk_app_rand[16];
    uint8_t  psk_server_pub[16];
    uint8_t  psk_start[32];            /* 启动帧原样留存（供 trace 与实测比对） */
    size_t   psk_start_len;
    uint8_t  psk_sign[64];             /* 已发出的 dev_sign */
    int      psk_ecdh_ok;
    int      psk_sign_len;             /* 0 = 未发（后端无 ECDSA） */
} miot_auth_t;

void miot_auth_init(miot_auth_t *a, const miot_auth_port_t *port);

/** 开机调用：预生成 ECDH 密钥对（ESP32-C3 上要 ~390ms，别放在 a2 处理里做，
 *  否则 device info 迟到、App 以 -51 断开）。失败可在 a2 时自动重试。 */
void miot_auth_prepare(miot_auth_t *a);

/** 0x0010 写回调（App 命令：0xA4 MTU / 0xA2 注册 / 0x24 登录） */
void miot_auth_on_0010(miot_auth_t *a, const uint8_t *d, size_t len);

/** 0x0019 写回调（认证数据通道，内含 ag2/通道/业务三层解析） */
void miot_auth_on_0019(miot_auth_t *a, const uint8_t *d, size_t len);

/** 周期调用（50~100ms 一次）：LOGIN devConfirm 延迟首发与重发 */
void miot_auth_poll(miot_auth_t *a, uint32_t now_ms);

/* ---- 状态查询 ---- */
static inline int miot_auth_is_bound(const miot_auth_t *a) { return a->bound; }
static inline int miot_auth_is_logged_in(const miot_auth_t *a) { return a->logged_in; }
/** LOGIN 成功后的 spec 会话密钥（前 40B 直接喂 miot_device_set_session_keys） */
static inline const uint8_t *miot_auth_session_keys(const miot_auth_t *a) { return a->session; }
static inline const miot_auth_identity_t *miot_auth_identity(const miot_auth_t *a) { return &a->id; }

/** 断连时调用：清会话（保留绑定身份与 ECDH 密钥） */
void miot_auth_on_disconnect(miot_auth_t *a);

/** legacy 绑定流程里 App 会把 20B Device ID 写在 0x0013 上，必须转存进来：
 *  device info 帧（channel msgType=0）就是靠它告诉 App「我是哪台设备」，
 *  did 为空时 App 收不到身份，卡在「设备初始化」不往下走认证。 */
void miot_auth_set_did(miot_auth_t *a, const uint8_t *did, size_t len);

/* ---- legacy PSK ---- */

/** 设定认证模式（默认 AUTO）。须在 init 之后、首次连接之前调用。 */
void miot_auth_set_mode(miot_auth_t *a, miot_auth_mode_t mode);

/** PSK 调试选项：trace=1 打印 0x0010/0x0019 全部原始帧；
 *  sign_variant 见 MIOT_PSK_SIGN_*（实测时逐个试）。 */
void miot_auth_set_psk_options(miot_auth_t *a, int trace, int sign_variant);

static inline miot_psk_state_t miot_auth_psk_state(const miot_auth_t *a) { return a->psk_state; }
/** PSK 是否走完（did 已由云端下发并落盘） */
static inline int miot_auth_psk_done(const miot_auth_t *a) {
    return a->psk_state == MIOT_PSK_DONE;
}

#ifdef __cplusplus
}
#endif
#endif /* MIOT_AUTH_H */
