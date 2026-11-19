/**
 * @file app_main.c
 * @brief 米家智能温湿度计 3 Pro（MJWSD07MMC / PID 0x9AB9）—— 可绑定版最小固件
 *
 * 一个完整可用的最小示例：米家 App 能走完「添加设备 → 注册绑定 → 连上读属性」，
 * 绑好之后温湿度 / CO₂ / 电量通过加密 MiBeacon 广播上报，小爱同学可以语音查询，
 * 米家页面与历史曲线正常（曲线是云端按时间窗聚合，见 README 的 FAQ）。
 *
 * 与同目录 thermo_gatt（温湿度计 2 / LYWSD03MMC）的关系：
 *   骨架完全相同（认证 + spec + 广播），差异只有三处，都在下面标了 ★：
 *     1) PID / 设备名 / spec 属性表（7.1001 7.1002 8.1029 4.1003）
 *     2) 广播对象号换新系列 0x5C01 / 0x5C02（沿用 3 mini 的 0x4C01/0x4C02 会静默丢帧）
 *     3) 一帧装不下四个对象 → 三帧轮换，第三帧在 CO₂ 与电量之间交替
 *
 * 链路（docs/protocol.md）：
 *   广播         MiBeacon v5 无 MAC 形态（FC 0x48/0x59）+ 0x038F V2 信标帧
 *   GATT 0xFE95  0x0010 命令/结果码 · 0x0019 认证 · 0x001A spec · 0x001B 第二写通道
 *   绑定         CMD_REGISTER(0xA2) → ECDH → 云下发 did(92B) → 云证书 → 回 0x11
 *   会话         CMD_LOGIN(0x24) → appRand/devRand → devConfirm/appConfirm → 0x21
 *   数据         spec 属性帧推在 0x0010 上
 *
 * ★真机是「按键连接模式」：平时广播不带可连接位，只有按一下设备按键才开一段
 *   唤醒窗口，App 才肯来连。本示例为了上手方便，平时就保持可连接（UND）——
 *   米家添加流程能直接走完。需要还原真机行为时，把 ADV_CONN_ALWAYS 改成 0。
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_device.h"
#include "miot_ble/miot_beacon.h"
#include "miot_ble/miot_profiles.h"

#include "gatt_miot.h"
#include "sensors.h"

void miot_port_esp32_crypto_init(void);

static const char *TAG = "T3PRO";

/* ★改参数只动这一块：
 *   T3PRO_PID   —— 米家智能温湿度计 3 Pro 国区 pdid 39609 = 0x9AB9
 *   T3PRO_NAME  —— 扫描响应里的设备名（真机是 MJWSD07MMC）
 *   读数        —— 见下方 v_temp/v_hum/v_co2/v_batt 与 sensor_tick()（板载真传感器） */
#define T3PRO_PID      0x9AB9
#define T3PRO_NAME     "MJWSD07MMC"
#define ADV_PERIOD_MS  1500      /* 一轮 3 帧 ≈ 4.5s 走完；网关侧够密 */
#define FRAME_SLOTS    3
/* ★命名空间必须独立：multi_dev 的档案槽是 "miot"（sel=0 夜灯2）/ "miot<sel>"。
 * 若这里也用 "miot"，本工程会读到夜灯2 的身份（did + bind_key）并以「已绑定」
 * 状态广播 3 Pro 的帧 —— 米家会把数据记到夜灯名下，3 Pro 反而添加不了。
 * 用独立名字后，multi_dev 与本工程可以来回烧，各自绑定互不影响。 */
#define NVS_NS         "t3pro"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"

/* 1=平时也保持可连接（默认，方便添加）；0=只在按键后开唤醒窗口（真机行为） */
#define ADV_CONN_ALWAYS  1
#define WAKE_WINDOW_MS   120000

/* ---------------- 真实读数（大金板载 HDC1080 + CM1106） ----------------
 * ★值必须缓慢波动，不要写死：蓝牙网关对"值没变"的属性通常只上报一次，
 * 云端历史曲线要积累多个不同数据点才开始绘制，写死的值会导致
 * 设备页顶部有实时读数、下方历史图表一直"数据为空"。
 * 真实传感器天然波动；读失败时保留上一次的值。 */
static uint32_t s_fcnt;                 /* 帧计数器 */
static int16_t  v_temp = 256;           /* 0.1℃   */
static uint8_t  v_hum  = 58;            /* %      */
static uint16_t v_co2  = 480;           /* ppm    */
static uint8_t  v_batt = 90;            /* %      —— 板子无电池检测，固定值 */
static uint32_t s_wake_until;           /* 按键唤醒窗口截止（ms） */
static uint32_t s_last_sensor;          /* 上次传感器轮询时刻 */

#define SENSOR_POLL_MS  10000           /* 与原 ESPHome cm1106 update_interval 一致 */

/* 轮询板载传感器；按位掩码只更新真正读成功的组（失败保留旧值） */
static void sensor_tick(void)
{
    int16_t t;
    uint8_t h;
    uint16_t c;
    int mask = daikin_sensors_read(&t, &h, &c);
    if (mask & DAIKIN_SENS_TH) {
        v_temp = t;
        v_hum = h;
    }
    if (mask & DAIKIN_SENS_CO2) {
        v_co2 = c;
    }
    if (mask) {
        ESP_LOGI(TAG, "sensor: %.1fC  %u%%  %uppm%s",
                 (double)v_temp / 10.0, v_hum, v_co2,
                 (mask & DAIKIN_SENS_TH) ? "" : " (th stale)");
    } else {
        ESP_LOGW(TAG, "sensor: all failed this round");
    }
}

static const char *const k_names[FRAME_SLOTS] = {
    "temperature", "humidity", "co2/battery"
};

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static int      s_idx;
static uint8_t  s_flen;
static int      s_logged_in;
static int      s_was_bound;
static uint32_t s_last_spec;            /* 上一次 spec 属性上报时刻 */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/** 按键唤醒窗口是否还开着（真机按下蓝牙按键后的一段时间） */
static bool win_active(void)
{
    return s_wake_until && (int32_t)(s_wake_until - now_ms()) > 0;
}

/** miot_port_t.time_now 的签名带 user 参数 */
static uint32_t port_time_now(void *user)
{
    (void)user;
    return now_ms();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---------------- 身份持久化 ----------------
 * 绑定时云端下发的 did / cloud_sign / bind_key 全在这个 blob 里。
 * 丢了它，App 每次连接都会把设备当新设备重新绑定一遍。 */
static void id_save(void *user, const miot_auth_identity_t *id)
{
    (void)user;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, NVS_KEY_ID, id, sizeof(*id));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "identity saved (%s), did[0..3]=%02X %02X %02X %02X",
             err == ESP_OK ? "ok" : "FAIL", id->did[0], id->did[1], id->did[2], id->did[3]);
}

static bool id_load(void *user, miot_auth_identity_t *id)
{
    (void)user;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*id);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_ID, id, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*id)) return false;
    ESP_LOGI(TAG, "identity loaded, did[0..3]=%02X %02X %02X %02X",
             id->did[0], id->did[1], id->did[2], id->did[3]);
    return true;
}

/* frame_cnt 掉电不回跳（防重放），每 10 帧落一次盘 */
static void fcnt_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) { s_fcnt = 1; return; }
    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_KEY_FCNT, &v) != ESP_OK) v = 1;
    nvs_close(h);
    s_fcnt = v;
}

static void fcnt_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_FCNT, s_fcnt);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- 日志与随机数 ---------------- */
static void port_log(void *user, int level, const char *tag, const char *fmt, ...)
{
    (void)user;
    va_list ap;
    va_start(ap, fmt);
    char buf[192];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (level == MIOT_LOG_ERROR)      ESP_LOGE(tag, "%s", buf);
    else if (level == MIOT_LOG_WARN)  ESP_LOGW(tag, "%s", buf);
    else                              ESP_LOGI(tag, "%s", buf);
}

static int port_random(void *user, uint8_t *out, size_t len)
{
    (void)user;
    esp_fill_random(out, len);
    return 0;
}

/* ---------------- 广播 ---------------- */

/**
 * 编码第 idx 帧。
 *
 * ★为什么用 v5_nomac 而不是 v5：BLE 广播的 service data 只有 23B 余量。
 *   带 MAC 的形态光帧头+MIC 就 18B，温度 float32 对象 7B → 25B，超限
 *   （ble_gap_adv_set_fields 直接返回 rc=4 EMSGSIZE，广播发不出去）。
 *   去掉 6B MAC 后 13+objs_len，最长的温度帧 20B，稳过。
 *   接收方（网关/App）本来就从 BLE 地址拿 MAC，帧里这份是冗余。
 */
static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = T3PRO_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    /* 未绑定：只发身份心跳（带 Capability、registered=0）。
     * 这一帧是 App「附近设备」列表的唯一依据，别掺任何对象数据进去。
     *
     * ★FC 高字节必须带 bit9（solicited，=「设备请求被连接」）：
     *   hello 帧默认给的是 0x28，App 能看到设备、点击后会**一直等这一位**
     *   才发起连接，等不到就干等 30s 报「连接超时」（2026-11-13 现场实证：
     *   设备端一个 CONNECT 事件都没有）。改成 0x2A 后立刻能连上。
     *   未绑定期间恒置这一位是安全的：没有网关会接管一台未注册的设备。 */
    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;       /* v2 | registered=0 | authMode=2 | solicited=1 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;
    switch (idx) {
    case 0:
        /* ★3 Pro 专属对象号：0x5C01 float32 ℃（不是 3 mini 的 0x4C01） */
        n = miot_beacon_obj_f32(objs, MB_OBJ_TEMP_F32_5C, (float)v_temp / 10.0f);
        break;
    case 1:
        /* ★0x5C02 uint8 %（不是 3 mini 的 0x4C02） */
        n = miot_beacon_obj_u8(objs, MB_OBJ_HUM_U8_5C, v_hum);
        break;
    default:
        /* 第三槽在 CO₂ 与电量之间交替：四个对象一帧装不下，真机也是分帧发的。
         * CO₂ 用 0x601D u16 ppm，电量用 0x4803 u8 %。 */
        if (s_fcnt % 2) n = miot_beacon_obj_u16(objs, MB_OBJ_CO2_U16, v_co2);
        else            n = miot_beacon_obj_u8 (objs, MB_OBJ_BATT_U8_48, v_batt);
        break;
    }

    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "frame rc=%d (crypto backend?)", fl);
        return fl;
    }
    /* ★按键唤醒窗口内把 bit9（solicited）置 1：真机按一下按键也是这么做的，
     * 等于对 App 喊「我在这儿、可以连」。0x59 = v5|registered|authMode=2，
     * 加 bit9 → 0x5B。窗口外必须清掉，长期置位会让网关以为设备一直有待传数据。 */
    if (win_active()) s_frame[1] = 0x5B;
    s_flen = (uint8_t)fl;

    /* 广播和 GATT 读到同一组值：手机连上时页面显示的就是当前读数 */
    t3pro_update((float)v_temp / 10.0f, v_hum, v_co2, v_batt);
    return fl;
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) return;        /* 已在广播，别重复启动（rc=6） */

    static const ble_uuid16_t uuid_fe95 = BLE_UUID16_INIT(0xFE95);

    uint8_t sd[32];
    int n = 0;
    sd[n++] = 0x95; sd[n++] = 0xFE;          /* MiBeacon service data */
    memcpy(sd + n, s_frame, s_flen);
    n += s_flen;

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    /* 必须显式带 16-bit Service UUID 列表：App 刷新数据时是按 0xFE95 过滤扫描的，
     * 只有 service data 而没有 UUID 列表时，Android 的 ScanFilter 匹配不上，
     * 设备就"扫不到"——绑定能成，但页面永远空着。
     * 31B 预算：uuid(4) + svc data(2+2+20) = 28，所以这里不再放 flags。 */
    f.uuids16 = &uuid_fe95;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    f.svc_data_uuid16 = sd;
    f.svc_data_uuid16_len = (uint8_t)n;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d (svc_data=%dB)", rc, n);
        return;
    }

    /* V2 信标帧：0x20 = 未注册。App 靠这一位判断该不该走绑定流程，
     * 绑定完成后改成 0x00，否则它会一直把设备当待添加。 */
    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(T3PRO_PID & 0xFF);
    mfr[m++] = (uint8_t)(T3PRO_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)T3PRO_NAME;
    rsp.name_len = sizeof(T3PRO_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "adv_rsp set failed");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
#if ADV_CONN_ALWAYS
    p.conn_mode = BLE_GAP_CONN_MODE_UND;     /* 随时可被 App 连接 */
#else
    /* 真机行为：只在唤醒窗口内可连接 */
    p.conn_mode = win_active() ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
#endif
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (win_active()) {
        /* 唤醒窗口内用快广播（30~50ms），App 才来得及在窗口内连上；
         * 平时未绑定 100~200ms、绑定后 800ms 保活（官方建议值）。 */
        p.itvl_min = 48;
        p.itvl_max = 80;
    } else {
        p.itvl_min = s_auth.bound ? 800 : 160;
        p.itvl_max = s_auth.bound ? 800 : 320;
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p,
                           gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

/* 重新广播：先停，避免 set_fields 在广播中改长度被拒 */
static void adv_refresh(void)
{
    if (gatt_miot_is_connected()) return;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    adv_start();
}

/* ---------------- GAP 事件 ---------------- */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            gatt_miot_on_connect(event->connect.conn_handle, BLE_ATT_MTU_DFLT);
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "phone connected -> adv stopped");
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        gatt_miot_on_disconnect();
        miot_auth_on_disconnect(&s_auth);
        miot_device_session_reset(&s_dev);
        s_logged_in = 0;
        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            build_frame(s_idx);
        }
        adv_start();
        ESP_LOGI(TAG, "disconnected reason=0x%02x -> adv resumed", event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        gatt_miot_on_mtu(event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

/* ---------------- spec 属性上报（手机连着时用） ----------------
 * 广播是给网关的，这条是给手机的：App 详情页顶部读数走 spec，
 * 四个属性一次性推过去（siid/piid 与 t3pro_init 的属性表严格对应）。 */
static void spec_report(void)
{
    miot_spec_value_t pc[4];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 7; pc[0].piid = 1001; pc[0].type = MIOT_TYPE_FLOAT;
    pc[0].val.f = (float)v_temp / 10.0f;
    pc[1].siid = 7; pc[1].piid = 1002; pc[1].type = MIOT_TYPE_U8;
    pc[1].val.u8 = v_hum;
    pc[2].siid = 8; pc[2].piid = 1029; pc[2].type = MIOT_TYPE_U16;
    pc[2].val.u16 = v_co2;
    pc[3].siid = 4; pc[3].piid = 1003; pc[3].type = MIOT_TYPE_U8;
    pc[3].val.u8 = v_batt;
    miot_device_send_props_changed(&s_dev, pc, 4);
}

/* ---------------- 启动 ---------------- */
static void on_sync(void)
{
    uint8_t addr[6];
    if (ble_hs_util_ensure_addr(0) == 0 &&
        ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0) {
        /* ★帧里的 MAC 字段必须是 LSB 序：云端/网关拿扫描到的 BLE 地址按 LSB 序
         * 拼 nonce（mac||pid||cnt||counter_ext），MAC 一错 nonce 就错，
         * 加密帧它永远解不开 —— 表现为设备在线、广播正常、页面永远没有读数。 */
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    }
    ESP_LOGI(TAG, "host synced, MAC %02X:%02X:%02X:%02X:%02X:%02X  bound=%d",
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0], s_auth.bound);

    s_idx = 0;
    build_frame(s_idx);
    adv_start();
    gatt_miot_dump_handles();

    /* 绑定态决定 V2 帧的 registered 位，这里同步一次 */
    s_was_bound = s_auth.bound;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------- SW1 面板键（GPIO34 —— 原 ESPHome yaml 里 id:RESET 的那颗）
 * 注意：GPIO34 是仅输入脚、无内部上下拉，板上自带偏置；
 * 开机采样空闲电平，之后「电平偏离空闲」= 按下，兼容高/低有效两种接法。
 *   短按（≥60ms）= 真机「蓝牙按键」：开 120 秒唤醒窗口（solicited=1 + 快广播）
 *   长按 5s     = 恢复出厂（擦 NVS）
 *
 * ★必须在运行期持续监听，不能只在 app_main 里读一次：早前那版只在启动
 *   瞬间检查电平，用户配对时怎么按 BOOT 都没反应，App 干等 30s 报「连接超时」
 *   （2026-11-13 现场实证：设备端零 CONNECT 事件）。 */
#define SW1_PIN 34
static int s_sw1_idle;                  /* 空闲电平（开机采样一次） */

static void btn_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (gpio_get_level(SW1_PIN) != s_sw1_idle) {
            int ms = 0;
            while (gpio_get_level(SW1_PIN) != s_sw1_idle) {
                vTaskDelay(pdMS_TO_TICKS(100));
                ms += 100;
                if (ms >= 5000) {
                    ESP_LOGW(TAG, "SW1 held 5s -> erase NVS (factory reset)");
                    nvs_flash_erase();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
            if (ms >= 60) {
                s_wake_until = now_ms() + WAKE_WINDOW_MS;
                ESP_LOGW(TAG, "SW1 press %dms -> wake window %ds (solicited adv + fast)",
                         ms, WAKE_WINDOW_MS / 1000);
                build_frame(s_idx);
                adv_refresh();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* SW1(GPIO34) 两种按法（运行期持续监听，见下方 btn_task）：
     *   短按 —— 真机「蓝牙按键」的等价动作：开 120s 唤醒窗口（solicited=1 + 快广播）
     *   长按 5s —— 恢复出厂：清掉绑定身份，广播回到未注册态。
     * 真机靠长按复位重新配对，这里同理 —— 否则一旦绑定过一次，
     * App 会认为设备已属于某个账号，再也进不了"添加设备"流程。
     * GPIO34 仅输入、无内部上下拉，只配 input，电平极性开机自适应。 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 34,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(50));
    s_sw1_idle = gpio_get_level(34);
    ESP_LOGI(TAG, "SW1 on GPIO34, idle level=%d (0=low-active,1=high-active)", s_sw1_idle);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);

    /* 板载传感器（HDC1080 I2C 19/21 + CM1106 UART1 16/17） */
    daikin_sensors_init();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

    /* 设备层：spec 属性 + 会话。
     * ★t3pro_init 用的是 3 Pro 的真实 spec（7.1001 / 7.1002 / 8.1029 / 4.1003），
     *   不要换成 thermometer_init —— 那是 2 代的 2.1/2.2/2.3，App 按 PID 查表
     *   查不到就整条丢弃，页面一片 "--"。 */
    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_prop_t *tbl; size_t cnt;
    t3pro_init(&s_dev, &s_port, &tbl, &cnt);
    ESP_LOGI(TAG, "spec table: %u props (7.1001 / 7.1002 / 8.1029 / 4.1003)", (unsigned)cnt);

    /* 认证层：notify 走 0x0010 / 0x0019，身份落 NVS */
    miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.notify_0010 = gatt_miot_notify_auth_0010;
    ap.notify_0019 = gatt_miot_notify_auth_0019;
    ap.save        = id_save;
    ap.load        = id_load;
    ap.random      = port_random;
    miot_auth_init(&s_auth, &ap);

    /* ECDH 预生成：ESP32-C3 上约 390ms，放在 0xA2 处理里会让 device info
     * 迟到、App 以 -51 断开 */
    uint32_t t0 = now_ms();
    miot_auth_prepare(&s_auth);
    ESP_LOGI(TAG, "ECDH prepare %lums", (unsigned long)(now_ms() - t0));

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return;
    }
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(T3PRO_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);

    uint32_t last_adv = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        /* 登录成功：把会话密钥塞给设备层，spec 通道才能收发 */
        if (s_auth.logged_in && !s_logged_in) {
            s_logged_in = 1;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** session established: spec channel ready ***");
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d (did stored, adv switches to registered) ***",
                     s_was_bound);
            build_frame(s_idx);
            adv_refresh();
        }

        /* 唤醒窗口到期：把广播里的 solicited 位清掉，回到常态形态
         * （长期置位会让网关以为设备一直有待传数据） */
        {
            static bool win_was;
            bool win_now = win_active();
            if (win_was && !win_now) {
                ESP_LOGI(TAG, "wake window closed -> adv back to %s",
                         s_auth.bound ? "0x59" : "0x2A");
                build_frame(s_idx);
                adv_refresh();
            }
            win_was = win_now;
        }

        /* 每 10s 轮询一次板载传感器（读失败保留旧值） */
        if (t - s_last_sensor >= SENSOR_POLL_MS) {
            s_last_sensor = t;
            sensor_tick();
        }

        if (!gatt_miot_is_connected()) {
            if (t - last_adv >= ADV_PERIOD_MS) {
                last_adv = t;
                s_idx = (s_idx + 1) % FRAME_SLOTS;
                s_fcnt++;
                if ((s_fcnt % 10) == 0) fcnt_save();
                if (build_frame(s_idx) >= 0) {
                    adv_refresh();
                    ESP_LOGD(TAG, "adv#%d %-13s frame=%dB cnt=%lu",
                             s_idx + 1, k_names[s_idx], s_flen, (unsigned long)s_fcnt);
                }
            }
        } else if (s_logged_in && t - s_last_spec >= 5000) {
            s_last_spec = t;
            spec_report();
            ESP_LOGI(TAG, "spec pushed (%.1fC / %u%% / %uppm / batt %u%%)",
                     (double)v_temp / 10.0, v_hum, v_co2, v_batt);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
