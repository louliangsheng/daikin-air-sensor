/**
 * @file miot_profiles.c
 * @brief 已验证设备档案实现
 *
 * 每个档案的 spec 表 / 事件格式都来自对应真机壳与米家 App 的实测互通，
 * 帧格式勿改（详见 docs/protocol.md）。
 */
#include "miot_ble/miot_profiles.h"
#include <stdio.h>
#include <string.h>

/* ================= 公共：测量事件 string 参数辅助 ================= */

static int send_event_str(miot_device_t *dev, uint8_t siid, uint16_t eiid,
                          uint16_t piid, const char *s)
{
    miot_spec_value_t a;
    memset(&a, 0, sizeof(a));
    a.piid = piid;
    a.type = MIOT_TYPE_STRING;
    snprintf(a.val.str, sizeof(a.val.str), "%s", s);
    return miot_device_send_event(dev, siid, eiid, &a, 1);
}

/* ================= 1. 体脂秤 S400 ================= */

static uint8_t  s400_battery = 92;      /* 3.1003 电量 % */
static uint8_t  s400_unit    = 0;       /* 0=kg 1=lb */
static uint8_t  s400_mode    = 0;       /* 10.1 work-mode: 0=normal 1=baby 2=balance 3=visitor 4=single */
static uint32_t s400_weight  = 0;       /* 最近体重，0.1kg 单位（页面直接显示） */
static uint32_t s400_imp     = 0;       /* 最近阻抗 0.1Ω */
static uint16_t s400_plugin_ver = 3;    /* 13.1 pluginversion */
static uint8_t  s400_time_flag  = 1;    /* 13.3 */
static uint8_t  s400_offline_cnt = 0;   /* 13.6 offline-msg-cnt（由 app_main 队列同步） */
static uint32_t s400_now = 0;           /* 13.4 current-time */

/* 13.1 sync 的 out 参数里要回给 App 的身份信息（真机从 flash 读，这里由 app 层填入） */
static char s400_sn[24]  = "S400-0000001";
static char s400_mac[18] = "10:00:3B:CE:42:03";
static char s400_fw[20]  = "1.0.0";

void scale_s400_set_identity(const char *mac, const char *sn, const char *fw)
{
    if (mac) snprintf(s400_mac, sizeof(s400_mac), "%s", mac);
    if (sn)  snprintf(s400_sn,  sizeof(s400_sn),  "%s", sn);
    if (fw)  snprintf(s400_fw,  sizeof(s400_fw),  "%s", fw);
}

/* 13.1 sync（yunmai.ms103 规格：in=[1,2,4,3,5] out=[6,7,8,9,10]）
 * out 依次是：离线消息数 / SN / MAC / 固件版本 / 电量。
 * ★必须带齐 5 个：只回 respCode 时 App 同步流程不推进，开屏停住（v67 实测）。 */
int scale_s400_action_out(uint8_t siid, uint8_t aiid,
                          miot_spec_value_t *outs, int max)
{
    if (siid != 13 || aiid != 1 || max < 5) return 0;
    memset(outs, 0, sizeof(*outs) * 5u);

    outs[0].piid = 6;  outs[0].type = MIOT_TYPE_U8;
    outs[0].val.u8 = s400_offline_cnt;
    outs[1].piid = 7;  outs[1].type = MIOT_TYPE_STRING;
    snprintf(outs[1].val.str, sizeof(outs[1].val.str), "%s", s400_sn);
    outs[2].piid = 8;  outs[2].type = MIOT_TYPE_STRING;
    snprintf(outs[2].val.str, sizeof(outs[2].val.str), "%s", s400_mac);
    outs[3].piid = 9;  outs[3].type = MIOT_TYPE_STRING;
    snprintf(outs[3].val.str, sizeof(outs[3].val.str), "%s", s400_fw);
    outs[4].piid = 10; outs[4].type = MIOT_TYPE_U8;
    outs[4].val.u8 = s400_battery;
    return 5;
}

static miot_prop_t s400_props[] = {
    { 3, 1003, MIOT_TYPE_U8,  MIOT_PROP_R,  1, &s400_battery, 0, 0,   NULL, "battery"   },
    { 3, 1004, MIOT_TYPE_U8,  MIOT_PROP_RW, 1, &s400_unit,    0, 1,   NULL, "unit"      },
    { 3, 1005, MIOT_TYPE_U32, MIOT_PROP_R,  4, &s400_weight,  0, 0,   NULL, "weight"    },
    { 3, 1006, MIOT_TYPE_U32, MIOT_PROP_R,  4, &s400_imp,     0, 0,   NULL, "impedance" },
    /* 10.1 必须注册：插件开屏会 GET 一次 work-mode，未注册拿到 result=1（不认识），
     * 页面按「不支持」处理，测量入口点不动（docs/protocol.md §11）。 */
    { 10, 1,   MIOT_TYPE_U8,  MIOT_PROP_RW, 1, &s400_mode,    0, 4,   NULL, "work-mode" },
    /* 13 syncinfo：开屏 13.1 之后 App 会把这些当设备身份读一遍 */
    { 13, 1,   MIOT_TYPE_U16, MIOT_PROP_R,  2, &s400_plugin_ver, 0, 0xFFFF, NULL, "pluginversion" },
    { 13, 2,   MIOT_TYPE_U8,  MIOT_PROP_R,  1, &s400_unit,    0, 1,   NULL, "unit"      },
    { 13, 3,   MIOT_TYPE_BOOL,MIOT_PROP_R,  1, &s400_time_flag, 0, 1, NULL, "time-valid" },
    { 13, 4,   MIOT_TYPE_U32, MIOT_PROP_R,  4, &s400_now,     0, 0,   NULL, "current-time" },
    { 13, 6,   MIOT_TYPE_U8,  MIOT_PROP_R,  1, &s400_offline_cnt, 0, 0xFF, NULL, "offline-msg-cnt" },
    { 13, 10,  MIOT_TYPE_U8,  MIOT_PROP_R,  1, &s400_battery, 0, 100, NULL, "battery"   },
};

void scale_s400_set_now(uint32_t epoch) { s400_now = epoch; }

/* 登录成功后立刻推一帧当前状态（体重 / 时间 / 电量）。
 * 真机行为：App 订阅完 spec 通道后等设备主动说话，1s 内零收包就退订重连，
 * 界面停在「设备初始化中」。所以连接一建立就得先吐一帧，不能等用户操作。 */
void scale_s400_push_status(miot_device_t *dev)
{
    miot_spec_value_t p[3];
    memset(p, 0, sizeof(p));
    p[0].siid = 3;  p[0].piid = 1005; p[0].type = MIOT_TYPE_U32; p[0].val.u32 = s400_weight;
    p[1].siid = 3;  p[1].piid = 1003; p[1].type = MIOT_TYPE_U8;  p[1].val.u8  = s400_battery;
    p[2].siid = 13; p[2].piid = 4;    p[2].type = MIOT_TYPE_U32; p[2].val.u32 = s400_now;
    miot_device_send_props_changed(dev, p, 3);
}

static bool s400_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 2 && piid == 1001) { out->type = MIOT_TYPE_FLOAT; out->val.f = 0.0f; return true; }
    return false;
}

static bool s400_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user;
    /* 云端 access 是 []，插件 setProps 会被 JS 框架拦掉，一般到不了这里；
     * 但 native 通道仍有直接写 10.1 的情况，接受比拒绝安全 —— 拒绝会让页面
     * 停在「上秤」不动。 */
    if (siid == 10 && piid == 1) { s400_mode = in->val.u8; return true; }
    return false;
}

void scale_s400_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, s400_props, sizeof(s400_props) / sizeof(s400_props[0]),
                       s400_get, s400_set, NULL);
    if (tbl) *tbl = s400_props;
    if (cnt) *cnt = sizeof(s400_props) / sizeof(s400_props[0]);
}

/* 单位换算（★全链路最容易错的一处，2026-11-08 卡在「上秤」就是它）：
 *   入参 weight_01g 是「克」，App 侧要 kg×10（插件 keyNeedDivide10，÷10 得 kg）。
 *   旧实现直接塞克（64500）→ 插件算成 6450kg，超量程丢弃 → 页面停在「上秤」。 */
static uint32_t s400_kg10(uint32_t grams) { return grams / 100u; }

/* 离线称重落库（无会话路径）。
 * 真机「体重称重模式：无需连接蓝牙，上秤即可完成测量」——数据先存秤里，
 * 等网关/手机连上来取。我们这里把值写进属性表，spec GET 3.1005/3.1006
 * 会直接返回它，所以网关一登录就能读到这次离线测量的结果。 */
void scale_s400_store_offline(uint32_t weight_01g, uint32_t impedance_01, uint32_t ts)
{
    (void)ts;
    s400_weight = s400_kg10(weight_01g);
    s400_imp    = impedance_01;
    /* 队列与计数由 app_main（设备层）管理，存一条后它会调 offline_set_count()。
     * 这里只保证 spec GET 3.1005/3.1006 拿到最新一次离线测量的值。 */
}

void scale_s400_offline_set_count(uint8_t n)
{
    s400_offline_cnt = n;
}

/* 一帧一步：0..3 = 8.1 过程流，4 = 8.2 结果，5 = 11.1022，6 = 11.2/11.3 属性变更。
 * ★必须由调用方节流（App 的 ag2 接收窗口同一毫秒只吃一帧，连发会整帧丢）。*/
int scale_s400_report_step_ex(miot_device_t *dev, int step,
                              uint32_t weight_01g, uint32_t impedance_01, uint32_t ts,
                              uint8_t weight_done_status)
{
    char s[160];
    uint32_t w = s400_kg10(weight_01g);

    if (step < 0 || step >= SCALE_S400_STEPS) return -1;

    if (step <= 2) {
        s400_weight = w;
        s400_imp = impedance_01;
        /* 8.1（piid=1）"体重(kg×10),状态"：
         *   0=weighting 1=bodyWeight 2=weighingHeart 3=overweight
         *   4=timeout   5=smallWeight 6=done */
        switch (step) {
        case 0: strcpy(s, "0,0"); break;
        case 1: snprintf(s, sizeof(s), "%u,0", (unsigned)(w * 9 / 10)); break;
        default: snprintf(s, sizeof(s), "%u,1", (unsigned)w); break;
        }
        return send_event_str(dev, 8, 1, 1, s);
    }

    if (step == 3) {
        /* 8.2（★piid=1，以 docs/protocol.md §4.4 的抓包样本为准：
         *   07 08 02 00 01 01 00 16 a0 → eiid=2, argc=1, piid=1, type=10, len=22）
         * ★保持 28 字段完整版：23:12 实测这一版能出数（type16 修正为 0xA0.. 之后）。
         *   插件 uploadBodyDataObj 按下标取：
         *   [3]=weight(kg×10) [4]=heartRate [5]=weightDoneStatus(0=正常)
         *   [6]=设备时间(unix 秒) [7]=bfp 体脂率 [8]=slm 骨骼肌 */
        /* [5]=weightDoneStatus：0=normalData 直接入库 / 2=userError / 4=multipleUsers。
         * ★离线且用户认不出来（真机屏显 "----"）必须非 0，App 才进「待认领数据」；
         *  恒发 0 的话不管体重多少都被算到当前用户头上（15:37 实测：45.6kg 也进历史）。 */
        snprintf(s, sizeof(s),
                 "0,0,0,%u,0,%u,%u,185,275,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0",
                 (unsigned)w, (unsigned)weight_done_status, (unsigned)ts);
        return send_event_str(dev, 8, 2, 1, s);
    }

    if (step == 4) {
        /* done 必须放在结果之后：真机是「稳定 → 出结果 → 结束称重」，
         * 先发 done 插件会提前收掉称重流程，后面的 8.2 就没人接了。 */
        return send_event_str(dev, 8, 1, 1, "0,6");
    }

    uint32_t metrics = (w & 0x7FF) | ((impedance_01 & 0x3FFF) << 18);

    if (step == 5) {
        /* 11.1022 收尾事件：duid(1B) + 复合字段(4B) + 时间戳(4B) */
        miot_spec_value_t a3[3];
        memset(a3, 0, sizeof(a3));
        a3[0].piid = 1; a3[0].type = MIOT_TYPE_U8;  a3[0].val.u8  = 1;
        a3[1].piid = 2; a3[1].type = MIOT_TYPE_U32; a3[1].val.u32 = metrics;
        a3[2].piid = 3; a3[2].type = MIOT_TYPE_U32; a3[2].val.u32 = ts;
        return miot_device_send_event(dev, 11, 1022, a3, 3);
    }

    /* 11.2 / 11.3 双保险 */
    miot_spec_value_t pc[2];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 11; pc[0].piid = 2; pc[0].type = MIOT_TYPE_U32; pc[0].val.u32 = metrics;
    pc[1].siid = 11; pc[1].piid = 3; pc[1].type = MIOT_TYPE_U32; pc[1].val.u32 = ts;
    return miot_device_send_props_changed(dev, pc, 2);
}

int scale_s400_report_step(miot_device_t *dev, int step,
                           uint32_t weight_01g, uint32_t impedance_01, uint32_t ts)
{
    return scale_s400_report_step_ex(dev, step, weight_01g, impedance_01, ts, 0);
}

void scale_s400_report(miot_device_t *dev, uint32_t weight_01g,
                       uint32_t impedance_01, uint32_t ts)
{
    for (int i = 0; i < SCALE_S400_STEPS; i++)
        scale_s400_report_step(dev, i, weight_01g, impedance_01, ts);
}

void scale_s400_sweep(miot_device_t *dev, const char *s1, const char *s2)
{
    static const uint8_t siids[] = { 2, 3, 4, 5, 6, 7, 9, 10, 12, 14, 15, 16 };
    for (size_t i = 0; i < sizeof(siids); i++) {
        send_event_str(dev, siids[i], 1, 1, s1);
        send_event_str(dev, siids[i], 2, 1, s2);
    }
}

/* ================= 2. 温湿度计（cgdk3 / t9 双 spec） =================
 * ★同一属性在两个 spec 里 siid 不同（cgdk3: 2.x；miaomiaoce-t9: 3.x），
 * 属性表两份都注册，插件用哪个 spec 都能读。 */

static float    th_temp  = 25.0f;    /* ℃ */
static float    th_hum   = 56.0f;    /* %RH（cgdk3 spec 是 float） */
static uint8_t  th_hum_t9 = 56;      /* t9 spec 湿度是 uint8 % */
static uint8_t  th_battery = 88;

static miot_prop_t th_props[] = {
    /* cgdk3：2.1001 温度 float / 2.1008 湿度 float / 2.1003 电量 */
    { 2, 1003, MIOT_TYPE_U8, MIOT_PROP_R, 1, &th_battery, 0, 0, NULL, "battery" },
    /* t9：3.1001 温度 float / 3.1002 湿度 uint8 / 3.1003 电量 */
    { 3, 1003, MIOT_TYPE_U8, MIOT_PROP_R, 1, &th_battery, 0, 0, NULL, "battery" },
    { 3, 1002, MIOT_TYPE_U8, MIOT_PROP_R, 1, &th_hum_t9,  0, 0, NULL, "humidity" },
};

static bool th_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if ((siid == 2 && piid == 1001) || (siid == 3 && piid == 1001)) {
        out->type = MIOT_TYPE_FLOAT; out->val.f = th_temp; return true;   /* 温度 float */
    }
    if (siid == 2 && piid == 1008) { out->type = MIOT_TYPE_FLOAT; out->val.f = th_hum; return true; }
    if (siid == 2 && piid == 1002) { out->type = MIOT_TYPE_FLOAT; out->val.f = th_hum; return true; } /* dk1 兼容 */
    if (siid == 2 && piid == 3)    { out->type = MIOT_TYPE_U8;    out->val.u8 = th_battery; return true; } /* t2 标准电量 */
    if (siid == 2 && piid == 1003) { out->type = MIOT_TYPE_U8; out->val.u8 = th_battery; return true; }
    if (siid == 2 && piid == 1)    { out->type = MIOT_TYPE_FLOAT; out->val.f = th_temp; return true; }
    if (siid == 2 && piid == 2)    { out->type = MIOT_TYPE_FLOAT; out->val.f = (float)th_hum_t9; return true; }
    if (siid == 3 && piid == 1)    { out->type = MIOT_TYPE_FLOAT; out->val.f = th_temp; return true; }
    return false;
}

static bool th_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;
}

void thermometer_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, th_props, sizeof(th_props) / sizeof(th_props[0]),
                       th_get, th_set, NULL);
    if (tbl) *tbl = th_props;
    if (cnt) *cnt = sizeof(th_props) / sizeof(th_props[0]);
}

void thermometer_report(miot_device_t *dev, float temp_c, float hum_pct, uint32_t ts)
{
    th_temp   = temp_c;
    th_hum    = hum_pct;
    th_hum_t9 = (uint8_t)(hum_pct + 0.5f);

    /* miaomiaoce.sensor_ht.t2 的官方 spec 就是 siid 2 下的三个属性：
     *   2.1 temperature        float ℃
     *   2.2 relative-humidity  float %
     *   2.3 battery-level      uint8 %
     * 旧实现发的 2.1001/2.1008/3.1002 属于 cgdk3 / t9 等其它型号的私有 spec，
     * 这个 model 里根本不存在，App 全部丢弃 —— 页面就是空的。 */
    miot_spec_value_t pc[3];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 2; pc[0].piid = 1; pc[0].type = MIOT_TYPE_FLOAT; pc[0].val.f  = temp_c;
    pc[1].siid = 2; pc[1].piid = 2; pc[1].type = MIOT_TYPE_FLOAT; pc[1].val.f  = hum_pct;
    pc[2].siid = 2; pc[2].piid = 3; pc[2].type = MIOT_TYPE_U8;    pc[2].val.u8 = th_battery;
    miot_device_send_props_changed(dev, pc, 3);
    (void)ts;
}

/* ================= 2b. 温湿度计 3 Pro（PID 0x9AB9） =================
 * 与 3 mini 的关键区别：3 Pro 支持蓝牙直连（App 详情页会显示「蓝牙 未连接手机」），
 * 也就是它有一条 GATT spec 通道。之前把它当纯广播型（属性表 NULL），手机
 * 连上来 READ 任何属性都拿不到东西，页面只能靠网关。补上真机 spec 后，
 * 手机在旁边直接能出数，网关通路另算。 */
static float    t3_temp = 25.0f;
static uint8_t  t3_hum  = 56;
static uint16_t t3_co2  = 480;
static uint8_t  t3_batt = 90;

static miot_prop_t t3_props[] = {
    { 7, 1001, MIOT_TYPE_FLOAT, MIOT_PROP_R, 4, &t3_temp, 0, 0, NULL, "temperature" },
    { 7, 1002, MIOT_TYPE_U8,    MIOT_PROP_R, 1, &t3_hum,  0, 0, NULL, "humidity" },
    { 8, 1029, MIOT_TYPE_U16,   MIOT_PROP_R, 2, &t3_co2,  0, 0, NULL, "co2" },
    { 4, 1003, MIOT_TYPE_U8,    MIOT_PROP_R, 1, &t3_batt, 0, 0, NULL, "battery" },
};

static bool t3_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 7 && piid == 1001) { out->type = MIOT_TYPE_FLOAT; out->val.f  = t3_temp; return true; }
    if (siid == 7 && piid == 1002) { out->type = MIOT_TYPE_U8;    out->val.u8 = t3_hum;  return true; }
    if (siid == 8 && piid == 1029) { out->type = MIOT_TYPE_U16;   out->val.u16 = t3_co2; return true; }
    if (siid == 4 && piid == 1003) { out->type = MIOT_TYPE_U8;    out->val.u8 = t3_batt; return true; }
    return false;
}

static bool t3_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;
}

void t3pro_init(miot_device_t *dev, miot_port_t *port,
                miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, t3_props, sizeof(t3_props) / sizeof(t3_props[0]),
                       t3_get, t3_set, NULL);
    if (tbl) *tbl = t3_props;
    if (cnt) *cnt = sizeof(t3_props) / sizeof(t3_props[0]);
}

void t3pro_update(float temp_c, uint8_t hum_pct, uint16_t co2_ppm, uint8_t batt)
{
    t3_temp = temp_c;
    t3_hum  = hum_pct;
    t3_co2  = co2_ppm;
    t3_batt = batt;
}

/* ================= 3. 开关型（夜灯 / 驱蚊器） ================= */

static uint8_t sw_on = 0;

static void sw_on_set(const struct miot_prop *p)
{
    (void)p;
    /* 真机行为：收到 set 后立刻回报一次 properties_changed(switch 0x1012 对应
     * spec 属性)，页面开关才停在正确位置 —— 副作用回调里置上报标志 */
}

static miot_prop_t sw_props[] = {
    { 2, 1012, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &sw_on, 0, 1, sw_on_set, "switch" },
};

static bool sw_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 2 && piid == 1012) { out->type = MIOT_TYPE_BOOL; out->val.u8 = sw_on; return true; }
    return false;
}

static bool sw_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;   /* 属性表已覆盖 */
}

void switch_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, sw_props, sizeof(sw_props) / sizeof(sw_props[0]),
                       sw_get, sw_set, NULL);
    if (tbl) *tbl = sw_props;
    if (cnt) *cnt = sizeof(sw_props) / sizeof(sw_props[0]);
}

/* ================= 4. 无线按钮（XMWXKG01LM） ================= */

static uint16_t btn_cfg = 1;    /* 3.1020 按钮配置：插件下发后需读回 */

static miot_prop_t btn_props[] = {
    { 3, 1020, MIOT_TYPE_U16, MIOT_PROP_RW, 2, &btn_cfg, 0, 0, NULL, "btn-config" },
    { 3, 1003, MIOT_TYPE_U8,  MIOT_PROP_R,  1, &th_battery, 0, 0, NULL, "battery" },
};

static bool btn_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 3 && piid == 1020) { out->type = MIOT_TYPE_U16; out->val.u16 = btn_cfg; return true; }
    return false;
}

static bool btn_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;
}

void button_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, btn_props, sizeof(btn_props) / sizeof(btn_props[0]),
                       btn_get, btn_set, NULL);
    if (tbl) *tbl = btn_props;
    if (cnt) *cnt = sizeof(btn_props) / sizeof(btn_props[0]);
}

/* ================= 5. 夜灯2（MUE4094RT） =================
 * v54 固件完整打通：2.1 亮度%(1-100) / 2.2 延时ms(5000-60000) /
 * 2.3 开关 / 3.1 电量。on_set 副作用 = 存 NVS + 重设 LED。 */

static uint8_t  nl_bright  = 50;
static uint32_t nl_dur     = 10000;
static uint8_t  nl_power   = 0;
static uint8_t  nl_battery = 80;

static void nl_on_any(const struct miot_prop *p) { (void)p; /* nl_save(); led_set(); */ }

static miot_prop_t nl_props[] = {
    { 2, 1, MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &nl_bright,  1,    100,   nl_on_any, "bright"  },
    { 2, 2, MIOT_TYPE_U32,  MIOT_PROP_RW, 4, &nl_dur,     5000, 60000, nl_on_any, "dur"     },
    { 2, 3, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &nl_power,   0,    0,     nl_on_any, "power"   },
    { 3, 1, MIOT_TYPE_U8,   MIOT_PROP_R,  1, &nl_battery, 0,    0,     NULL,      "battery" },
};

void night_light_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, nl_props, sizeof(nl_props) / sizeof(nl_props[0]),
                       NULL, NULL, NULL);
    if (tbl) *tbl = nl_props;
    if (cnt) *cnt = sizeof(nl_props) / sizeof(nl_props[0]);
}

/* ================= 6. 驱蚊器智能版（WX08ZM，PID 0x040A） =================
 * PID 0x040A = 十进制 1034，就是米家驱蚊器智能版，对应 spec
 *   urn:miot-spec-v2:device:mosquito-dispeller:0000A065:zimi-v1:1  (zimi.mosq.v1)
 * ★★ 2026-11-07 实测踩坑：早前照 zimi.mosq.v2 抄的四属性表（2.1 on / 2.2 mode /
 *   4.1 repellent-left-level / 3.1 battery）在这个 PID 上全错位 —— v1 里
 *   2.1 是「驱蚊片余量(uint8 只读)」、4.1 才是「开关(bool 读写)」、根本没有 2.2。
 *   于是 App 收到的第一帧就是「2.1=bool」，类型与 spec 不符，解析直接失败，
 *   设备页永远停在「设备初始化 / 蓝牙连接中」，即使认证、加密、推包全部正常。
 *   对照 v1 官方 spec 只有三个属性：
 *     2.1 repellent-left-level  uint8 只读 0~100   ← 驱蚊片余量
 *     3.1 battery-level         uint8 只读 0~100   ← 电量
 *     4.1 on                    bool  读写        ← 开关
 * 真机行为：set 4.1 后立刻回报 properties_changed(4.1)。 */

static uint8_t rp_on      = 0;
static uint8_t rp_margin  = 76;
static uint8_t rp_battery = 65;

static void rp_on_power(const struct miot_prop *p)
{
    (void)p;    /* 真机：置 pending 标志，下一拍发 properties_changed(4.1) */
}

static miot_prop_t rp_props[] = {
    { 2, 1, MIOT_TYPE_U8,   MIOT_PROP_R,  1, &rp_margin,  0, 100, NULL,        "margin"  },
    { 3, 1, MIOT_TYPE_U8,   MIOT_PROP_R,  1, &rp_battery, 0, 100, NULL,        "battery" },
    { 4, 1, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &rp_on,      0, 1,   rp_on_power, "power"   },
};

void repeller_init(miot_device_t *dev, miot_port_t *port,
                   miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, rp_props, sizeof(rp_props) / sizeof(rp_props[0]),
                       NULL, NULL, NULL);
    if (tbl) *tbl = rp_props;
    if (cnt) *cnt = sizeof(rp_props) / sizeof(rp_props[0]);
}

uint8_t repeller_power(void)
{
    return rp_on ? 1 : 0;
}

void repeller_report(miot_device_t *dev, uint8_t power, uint8_t battery,
                     uint8_t margin, uint32_t ts)
{
    rp_on      = power ? 1 : 0;
    rp_battery = battery;
    rp_margin  = margin;

    /* 按 zimi.mosq.v1 真实 spec 报全三个属性：2.1 药量 / 3.1 电量 / 4.1 开关。
     * 一帧 3 条 = 24B 明文（wire 30B），ag2 自动拆 18B+12B 两片，
     * 与温湿度计已验证的同款路径一致（它一帧 33B 也是这么过的）。 */
    miot_spec_value_t pc[3];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 2; pc[0].piid = 1; pc[0].type = MIOT_TYPE_U8;   pc[0].val.u8 = rp_margin;
    pc[1].siid = 3; pc[1].piid = 1; pc[1].type = MIOT_TYPE_U8;   pc[1].val.u8 = rp_battery;
    pc[2].siid = 4; pc[2].piid = 1; pc[2].type = MIOT_TYPE_BOOL; pc[2].val.u8 = rp_on ? 1 : 0;
    miot_device_send_props_changed(dev, pc, 3);
    (void)ts;
}

/* ================= 通用：auth ↔ device 标准接线 ================= */

void profiles_wire_auth(miot_auth_t *auth, miot_device_t *dev,
                        int (*notify_0010)(void *, const uint8_t *, size_t),
                        int (*notify_0019)(void *, const uint8_t *, size_t),
                        void *user)
{
    static miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.user = user;
    ap.notify_0010 = notify_0010;
    ap.notify_0019 = notify_0019;
    miot_auth_init(auth, &ap);
    (void)dev;
}
