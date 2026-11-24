/**
 * @file gatt_miot.c
 * @brief 0xFE95 服务与四个特征值的 NimBLE 实现
 */
#include "gatt_miot.h"

#include <string.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "MIOT-GATT";

/* 服务与特征值 UUID（SIG 分配给小米的 MiBeacon 服务 0xFE95）
 * 米家 App 侧的完整形式是 0000FE95-0000-1000-8000-00805F9B34FB */
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(0xFE95);
static const ble_uuid16_t s_c10_uuid = BLE_UUID16_INIT(0x0010);
static const ble_uuid16_t s_c19_uuid = BLE_UUID16_INIT(0x0019);
static const ble_uuid16_t s_c1a_uuid = BLE_UUID16_INIT(0x001A);
static const ble_uuid16_t s_c1b_uuid = BLE_UUID16_INIT(0x001B);

static miot_auth_t   *s_auth;
static miot_device_t *s_dev;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = BLE_ATT_MTU_DFLT;

static uint16_t s_h10, s_h19, s_h1a, s_h1b;

/* 各级通道的收发计数，出问题时先看这里：写得进来但没有回包，通常是状态机
 * 停在别的分支；一条写都没有，则是 App 压根没认这个服务。 */
static uint32_t s_wr10, s_wr19, s_wr1a, s_wr1b;

/* 0x0010 是同一条通道上的多路复用：认证命令（A4/A2/24，长度 1 或 4）
 * 和认证之后的数据帧（spec，ag2 封装，首字节 0x00）都从这里进来。
 * 按已知认证命令白名单分流，剩下的都交给 spec 层。 */
static int is_auth_cmd(const uint8_t *d, uint16_t len)
{
    if (len == 1 && d[0] == 0xA4) return 1;                    /* 建链 */
    if (len != 4) return 0;
    if (d[1] || d[2] || d[3]) return 0;
    return d[0] == 0x24 || (d[0] & 0x7F) == 0x22;              /* LOGIN / REGISTER */
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    uint16_t u = ble_uuid_u16(ctxt->chr->uuid);

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* 米家不读这些特征值；读请求给一个空返回即可 */
        return 0;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > 512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[512];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (u) {
    case 0x0010:
        s_wr10++;
        /* 命令很短（1B 或 4B），打印首字节足够定位是哪一步 */
        ESP_LOGI(TAG, "wr 0010 #%lu len=%u cmd=%02X", (unsigned long)s_wr10, got, got ? buf[0] : 0);
        if (is_auth_cmd(buf, got)) {
            if (s_auth) miot_auth_on_0010(s_auth, buf, got);
        } else if (s_dev) {
            miot_device_on_rx(s_dev, buf, got);
        }
        break;
    case 0x0019:
        s_wr19++;
        ESP_LOGD(TAG, "wr 0019 #%lu len=%u", (unsigned long)s_wr19, got);
        if (s_auth) miot_auth_on_0019(s_auth, buf, got);
        break;
    case 0x001A:
        s_wr1a++;
        ESP_LOGD(TAG, "wr 001A #%lu len=%u", (unsigned long)s_wr1a, got);
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    case 0x001B:
        s_wr1b++;
        ESP_LOGD(TAG, "wr 001B #%lu len=%u", (unsigned long)s_wr1b, got);
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_c10_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h10,
    },
    {
        .uuid = &s_c19_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h19,
    },
    {
        .uuid = &s_c1a_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1a,
    },
    {
        .uuid = &s_c1b_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_h1b,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

void gatt_miot_register_svcs(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "0xFE95 service registered (0010/0019/001A/001B)");
}

void gatt_miot_bind(miot_auth_t *auth, miot_device_t *dev)
{
    s_auth = auth;
    s_dev = dev;
}

void gatt_miot_on_connect(uint16_t conn_handle, uint16_t mtu)
{    s_conn = conn_handle;
    s_mtu = mtu ? mtu : BLE_ATT_MTU_DFLT;
    s_wr10 = s_wr19 = s_wr1a = s_wr1b = 0;
    ESP_LOGI(TAG, "connected conn=%u mtu=%u", conn_handle, s_mtu);
}

void gatt_miot_on_disconnect(void)
{
    ESP_LOGI(TAG, "disconnected (wr 0010=%lu 0019=%lu 001A=%lu 001B=%lu)",
             (unsigned long)s_wr10, (unsigned long)s_wr19,
             (unsigned long)s_wr1a, (unsigned long)s_wr1b);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = BLE_ATT_MTU_DFLT;
}

void gatt_miot_dump_handles(void)
{
    /* value handle；CCCD 一般紧跟其后（+1/+2 视是否多个描述符而定）。
     * App 的 SUBSCRIBE 事件里给的是 CCCD handle，对照这里就能知道它订的是哪条通道。 */
    ESP_LOGI(TAG, "handles: 0010=%u 0019=%u 001A=%u 001B=%u (cccd ~ +1)",
             s_h10, s_h19, s_h1a, s_h1b);
}

void gatt_miot_on_mtu(uint16_t mtu)
{
    s_mtu = mtu ? mtu : BLE_ATT_MTU_DFLT;
    ESP_LOGI(TAG, "mtu -> %u", s_mtu);
}

int gatt_miot_is_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

static uint16_t s_notify_fail;

static int notify_chr(uint16_t handle, const uint8_t *d, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || handle == 0) return -1;

    uint16_t max = s_mtu > 3 ? (uint16_t)(s_mtu - 3) : 20;
    if (len > max) {
        /* 协议层没按 MTU 分片，说明对端协商出来的 MTU 比预期小（或没协商）。
         * 直接截断会送出半帧，宁可不发。 */
        ESP_LOGW(TAG, "notify h=%u len=%u > mtu-3=%u, dropped", handle, (unsigned)len, max);
        return -1;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)len);
    if (!om) return -1;

    int rc = ble_gatts_notify_custom(s_conn, handle, om);
    if (rc != 0 && rc != BLE_HS_EDONE) {
        /* 未订阅时返回 ATT 错误是正常的：App 还没写 CCCD。
         * 但如果是「一直没人订阅」，这里就是数据到不了 App 的原因，
         * 所以前几次用 WARN 打出来，别让它静默消失。 */
        if (s_notify_fail < 6) {
            s_notify_fail++;
            ESP_LOGW(TAG, "notify h=%u len=%u rc=%d (nobody subscribed?)",
                     handle, (unsigned)len, rc);
        }
    } else {
        s_notify_fail = 0;
    }
    return (rc == 0 || rc == BLE_HS_EDONE) ? 0 : -1;
}

int gatt_miot_notify_spec(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* spec 数据走 0x0010 的 notify。
     * 米家 App 只订阅 0x0010(16) 和 0x0019(19)，从订阅过 0x001A —— 之前推
     * 到 0x001A 等于往没人监听的管道灌水，App 页面因此永远是空。
     * 0x0010 是 miIO 主通道：App 写命令、设备在同一条上 notify 事件/回复。 */
    return notify_chr(s_h10, d, len);
}

int gatt_miot_notify_auth_0010(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    return notify_chr(s_h10, d, len);
}

int gatt_miot_notify_auth_0019(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    return notify_chr(s_h19, d, len);
}
