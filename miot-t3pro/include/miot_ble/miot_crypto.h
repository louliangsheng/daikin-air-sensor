/**
 * @file miot_crypto.h
 * @brief 米家 BLE 加密抽象层
 *
 * 核心通过 miot_crypto_ops_t 函数指针对接加密后端（mbedTLS/OpenSSL/…），
 * SDK 本身不链接任何加密库。参考实现见 port/esp32/miot_port_esp32.c。
 *
 * 三套加密体系：
 *   1. 旧版 beacon 鉴权：RC4 + mixA/mixB 密钥派生（token 加密广播）
 *   2. 标准认证（0x0019 通道）：HKDF-SHA256 派生会话密钥
 *   3. spec 数据通道（k61 帧）：AES-CCM，帧 = [seq16 LE][ct][tag4]，
 *      nonce = iv(4B) || 0x00000000 || seq32 LE
 */
#ifndef MIOT_CRYPTO_H
#define MIOT_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 加密后端接口（注册后生效；未注册时相关函数返回错误） */
typedef struct miot_crypto_ops {
    void *ctx;
    /** HMAC-SHA256，返回 0 成功 */
    int (*hmac_sha256)(void *ctx, const uint8_t *key, size_t klen,
                       const uint8_t *data, size_t dlen, uint8_t out[32]);
    /** AES-CCM（tag 4 字节，支持 AAD），enc=1 加密 0 解密，返回 0 成功 */
    int (*aes_ccm)(void *ctx, int enc, const uint8_t key[16], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[4]);
    /* ---- 标准认证 REGISTER：ECDH P-256 ---- */
    /** 生成设备密钥对；pubkey64 = 公钥 X||Y（无 04 前缀）。返回 0 成功 */
    int (*ecdh_keygen)(void *ctx, uint8_t pubkey64[64]);
    /** 用 App 公钥（64B X||Y）算共享密钥。返回 0 成功 */
    int (*ecdh_shared)(void *ctx, const uint8_t app_pubkey64[64], uint8_t secret[32]);

    /* ---- legacy PSK：ECDSA P-256 签名（可为 NULL，见下） ---- */
    /** 用设备 ECDH 私钥对 msg 做 SHA256 摘要后签名，输出 r||s 共 64B。
     *  ★这是 PSK 唯一需要的新原语；后端不实现时 PSK 分支会自动
     *    退化为「不发 0x04 帧」并打标记，标准认证完全不受影响。 */
    int (*ecdsa_sign)(void *ctx, const uint8_t *msg, size_t len, uint8_t sig64[64]);
} miot_crypto_ops_t;

void miot_crypto_set_ops(const miot_crypto_ops_t *ops);

/* ---------- 1. 旧版 beacon 鉴权 ---------- */

void miot_rc4(const uint8_t *key, size_t klen, const uint8_t *in, uint8_t *out, size_t len);

/** mixA：m = MAC 空中序 6B，pid 16 位小端。派生 8B 密钥用于 token 加密广播 */
void miot_mix_a(const uint8_t m[6], uint16_t pid, uint8_t k[8]);
void miot_mix_b(const uint8_t m[6], uint16_t pid, uint8_t k[8]);

/* ---------- 2. HKDF-SHA256 ---------- */

int miot_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *salt, size_t salt_len,
                     const char *info, size_t info_len,
                     uint8_t *out, size_t out_len);

/* ---------- 3. 标准认证（REGISTER / LOGIN） ---------- */

int miot_hmac_sha256(const uint8_t *key, size_t klen,
                     const uint8_t *data, size_t dlen, uint8_t out[32]);
int miot_ecdh_keygen(uint8_t pubkey64[64]);
int miot_ecdh_shared(const uint8_t app_pubkey64[64], uint8_t secret[32]);
/** ECDSA P-256 签名（legacy PSK 的 0x04 帧用）。后端未实现返回 -2 */
int miot_ecdsa_sign(const uint8_t *msg, size_t len, uint8_t sig64[64]);
/** 后端是否提供 ecdsa_sign（1 有 / 0 无） */
int miot_ecdsa_available(void);

/* ---------- 4. spec 通道 k61 帧加解密 ---------- */

/** 帧格式 [seq16 LE][ct+tag4]，输出 = len+4 字节。返回 0 成功 */
int miot_spec_frame_encrypt(const uint8_t key[16], const uint8_t iv[4], uint16_t seq,
                            const uint8_t *pt, size_t len, uint8_t *out);
int miot_spec_frame_decrypt(const uint8_t key[16], const uint8_t iv[4], uint16_t seq,
                            const uint8_t *in, size_t len, uint8_t *pt /* len */);

/* ---------- 5. 通用 AES-CCM（带 AAD，REGISTER 加密 did 用） ---------- */

/** 输出 [ct][tag4] 连续 buffer。返回 0 成功 */
int miot_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, size_t len, uint8_t *out);
/** in = [ct][tag4]。返回 0 成功 */
int miot_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t len, uint8_t *pt);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_CRYPTO_H */
