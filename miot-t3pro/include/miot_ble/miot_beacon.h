/**
 * @file miot_beacon.h
 * @brief MiBeacon v5 加密广播帧 —— 覆盖不建连接的传感器设备
 *
 * 适用设备：甲醛检测仪 JQJCY01YM、门窗传感器2 MCCGQ02HL、人体传感器2 RTCGQ02LM、
 * 水浸传感器 SJWS01LM、温湿度计2 t9 / 3 mini MJWSD06MMC、体脂秤2 XMTZC05HM 探针等。
 *
 * 帧格式（真机 btsnoop/ESPHome 互证实测）：
 *   FC(2,LE) | PID(2,LE) | frame_cnt(1) | MAC(6,空中序)
 *   | Capability(1,仅未绑定) | 对象密文(N) | counter_ext(3) | MIC(4)
 *
 * FC 是 2 字节位图，位号跨字节连续（官方 ble-mibeacon.md §Frame Control）：
 *   bit3 isEncrypted | bit4 MAC Include | bit5 Capability Include
 *   bit6 Object Include | bit7 Mesh
 *   bit8 registered | bit9 solicited | bit10~11 authMode | bit12~15 version
 *
 * 已绑定上报帧 FC = 58 59（enc|mac|obj / v5|auth=2|registered=1）。
 * **未绑定必须用 hello 形态**：bit5 官方原文「设备未绑定前，这一位强制为 1」，
 * 且 bit8 必须为 0，否则 App 认为设备已被别家绑定，不进待添加列表。
 * 旧版这里写死 58/59（缺 Capability、声明已注册），是 App 扫不到的直接原因。
 *   加密：AES-CCM(beaconkey 16B)，nonce = MAC(6)|pid_le(2)|cnt_le(4)，
 *         aad = 0x11，tag 4B 放帧尾（SDK 解析：先取末 4B 为 MIC，再往前 3B 为 counter）
 *   帧内必须带 MAC 字段：App 按帧内 MAC 做设备归属判定。
 */
#ifndef MIOT_BEACON_H
#define MIOT_BEACON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 标准 MiBeacon 对象 ID（实测自真机广播） ---------- */
/* ★通用事件（Event，不是 Property）：网关按边沿判定，收到后可以主动作为
 * central 连上来。0x0001=「连接」，0x0003=「靠近」。这是第三方 MiBeacon
 * 网关唯一会被「叫醒」去建立 GATT 连接的手段 —— 秤这类 Spec 设备没有
 * 能装体重的 Property 对象，只能靠事件把网关引过来，再由会话推数据。 */
#define MB_OBJ_EV_CONNECT     0x0001  /* 1B：设备请求被连接 */
#define MB_OBJ_EV_NEAR        0x0003  /* 1B：设备靠近（唤醒网关） */
#define MB_OBJ_MOTION_LIGHT   0x000F  /* 3B：b1=移动 b2=光照>0 有光（人体/夜灯共用） */
#define MB_OBJ_BUTTON         0x1001  /* 3B：类型/步数/动作 0=单击 1=双击 2=长按 */
#define MB_OBJ_TEMP_I16       0x1004  /* 2B：int16 0.1℃（t9/cgdk3/JQJCY01YM） */
#define MB_OBJ_HUM_U16        0x1006  /* 2B：uint16 0.1% */
#define MB_OBJ_BATTERY        0x100A  /* 1B：电量 % */
#define MB_OBJ_HCHO_U16       0x1010  /* 2B：uint16 0.01 mg/m³（甲醛） */
#define MB_OBJ_SWITCH         0x1012  /* 1B：0=关 1=开（驱蚊器 WX08ZM） */
#define MB_OBJ_MARGIN         0x1013  /* 1B：驱蚊片余量 % */
#define MB_OBJ_LEAK           0x1014  /* 1B：0=正常 >0=漏水（SJWS01LM） */
#define MB_OBJ_NO_MOTION      0x1017  /* 4B：距上次移动秒数（RTCGQ02LM） */
#define MB_OBJ_LIGHT          0x1018  /* 1B：0=暗 非0=亮（MCCGQ02HL） */
#define MB_OBJ_DOOR           0x1019  /* 1B：0=开 1=关 2=超时未关 3=复位（MCCGQ02HL） */
#define MB_OBJ_TEMP_F32       0x4C01  /* 4B：float32 ℃（MJWSD06MMC，miot 2.1001，mi=19457） */
#define MB_OBJ_HUM_U8         0x4C02  /* 1B：uint8 %（MJWSD06MMC，miot 2.1002，mi=19458） */
#define MB_OBJ_BATT_U8_48     0x4803  /* 1B：电量 %（MJWSD06MMC，miot 3.1003，mi=18435） */
/* ★温湿度计 3 Pro（MJWSD07MMC / PID 0x9AB9）用的是**新系列对象号**，
 * 不是 3 mini 的 0x4C01/0x4C02。2026-11-13 现场实证：
 * 发 0x4C01/0x4C02 时页面温湿度恒为 "--"；改成 0x5C01/0x5C02 后
 * 温度 26.1℃ 与湿度同时出现，小爱语音也能查到 —— 网关→云端→语音全链路通。
 * 编码与旧号完全一致（0x5C01 float32 4B / 0x5C02 uint8 1B，ble_monitor 同）。 */
#define MB_OBJ_TEMP_F32_5C    0x5C01  /* 4B：float32 ℃（MJWSD07MMC，miot 7.1001） */
#define MB_OBJ_HUM_U8_5C      0x5C02  /* 1B：uint8 %（MJWSD07MMC，miot 7.1002） */
#define MB_OBJ_CO2_U16        0x601D  /* 2B：uint16 ppm（MJWSD07MMC，miot 8.1029） */
/* 无线开关事件对象：载荷恒 1B = 按键号（单键恒 1；双键 1=左 2=右）。
 * 三键位分开成三个对象，App 侧据此拆成「单击/双击/长按」三个触发器。
 * 实测：XMWXKG01YL（0x1949 双键）与 XMWXKG01CM（0x2387 单键）通用。 */
#define MB_OBJ_BTN_CLICK      0x4E0C  /* 1B：单击 */
#define MB_OBJ_BTN_DOUBLE     0x4E0D  /* 1B：双击 */
#define MB_OBJ_BTN_HOLD       0x4E0E  /* 1B：长按 */

/* ---- 花花草草监测仪 HHCCJCY01（PID 0x0098），花盆/户外款同构 ----
 * 四个传感器对象里 0x1007 是**三字节** uint24（不是 uint32），必须用
 * miot_beacon_obj_u24；写成 4B 会让后续对象全部错位，App 静默丢弃整帧。
 * 0x1008 是土壤湿度，别和 0x1006（空气湿度）混；两者在同一设备上并存。 */
#define MB_OBJ_ILLUM_U24      0x1007  /* 3B：uint24 LE，1 lx（与 0x000F 的移动+光照不同构） */
#define MB_OBJ_SOIL_MOIST_U8  0x1008  /* 1B：uint8 % */
#define MB_OBJ_SOIL_EC_U16    0x1009  /* 2B：uint16 LE µS/cm，与 spec soil-ec 同量纲 */

/** beacon 广播配置（认证/绑定阶段获得，需持久化） */
typedef struct miot_beacon_cfg {
    uint16_t pid;                    /* 产品 ID（米家后台分配，如 S400=0x30D9） */
    uint8_t  mac_air[6];             /* 广播 MAC，空中（LSB）序 */
    uint8_t  beacon_key[16];         /* beaconkey：绑定时生成（随机或派生），掉电保存 */
} miot_beacon_cfg_t;

/* ---------- 对象编码辅助（返回写入字节数） ---------- */

size_t miot_beacon_obj_u8(uint8_t *p, uint16_t obj_id, uint8_t v);
size_t miot_beacon_obj_u16(uint8_t *p, uint16_t obj_id, uint16_t v);
/** 3 字节小端（光照对象 0x1007 专用；v 只用低 24 位） */
size_t miot_beacon_obj_u24(uint8_t *p, uint16_t obj_id, uint32_t v);
size_t miot_beacon_obj_i16(uint8_t *p, uint16_t obj_id, int16_t v);
size_t miot_beacon_obj_u32(uint8_t *p, uint16_t obj_id, uint32_t v);
size_t miot_beacon_obj_f32(uint8_t *p, uint16_t obj_id, float v);
size_t miot_beacon_obj_bytes(uint8_t *p, uint16_t obj_id, const uint8_t *v, size_t len);

/**
 * 构造 MiBeacon v5 加密数据帧。
 * @param objs     已编码的对象明文（miot_beacon_obj_xxx 产出）
 * @param out      输出帧（放入广播 manufacturer data，前加公司码 0x038F 时总长 ≤31B）
 * @return 帧长，负数失败（未注册加密后端 / objs 过长 / cap 不足）
 *
 * 注意：单帧对象总明文 ≤13B 才能保证含公司码的广播 ≤31B；
 * 多对象时按「三路轮换」分帧（温度/湿度/电量各占一帧交替广播）。
 *
 * ⚠️ 31B 硬上限（v85 实测定案）：广播包 = flags(3) + service data
 *    「长度1 + UUID2 + 95FE2 + 帧」，即**帧本身可用 ≤26B**。超限时 NimBLE 返回
 *    rc=4（BLE_HS_EMSGSIZE）；若此时广播已被 stop，设备会静默到下一个心跳，
 *    表现为「App 一直正在连接 → 超时」。规避顺序：
 *    ① 拆帧：一件事一个对象，别把多对象塞进一帧；
 *    ② 仍超限 → 改「无 MAC 形态」：FC 由 58/59 改 48/49 并跳过 6B MAC 字段
 *       （真机 24B 测量帧同款，网关按广播源地址归属）。本函数只产出带 MAC 形态，
 *       无 MAC 形态需调用方按此规则自行裁剪，详见 docs/testing.md 踩坑 6。
 */
int miot_beacon_frame_v5(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                         const uint8_t *objs, size_t objs_len,
                         uint8_t *out, size_t cap);

/**
 * 同上，但走「无 MAC 形态」：FC 0x48/0x59，帧里不带 6B MAC。
 *
 * 专给 GATT notify 用 —— App 的 ATT MTU 固定 23（单包 20B），带 MAC 的形态
 * 光帧头+尾部就 18B，只剩 2B，连一个 u8 对象都放不下；去掉 MAC 后 13+objs_len，
 * 一个 3B 对象 = 19B。GATT 上接收方靠连接识别设备，MAC 冗余。
 *
 * @return 帧长（13+objs_len），负数失败
 */
int miot_beacon_frame_v5_nomac(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                               const uint8_t *objs, size_t objs_len,
                               uint8_t *out, size_t cap);

/* ---------- 未绑定（可被 App 发现）广播帧与扫描探测 ---------- */

/**
 * 构造绑定前广播帧（未加密，App 靠它发现设备并列出「添加」入口）。
 *
 *   FC(2) | PID(2 LE) | frame_cnt(1) | MAC(6 空中序) | capability(1) = 12B
 *
 * 基线取自标准认证型号实测：fc0=0x30（MAC|Capability，未加密不含 Object）、
 * fc1=0x58（v5 | authMode=2 | registered=0）、capability=0x10（BondAbility=2<<3）。
 * ⚠️ PSK 型号（驱蚊器 0x040A 等）App 扫描阶段认哪组值从未实测过 —— 用
 * miot_adv_probe_forms() 的候选表轮播探测，命中形态由 GAP 连接回调打日志。
 *
 * @return 帧长（12），负数失败（cap 不足）
 */
int miot_beacon_frame_unbound(uint16_t pid, const uint8_t mac_air[6],
                              uint8_t fc0, uint8_t fc1, uint8_t capability,
                              uint8_t frame_cnt, uint8_t *out, size_t cap);

/** 一个广播形态：fc1 低 3 位按基线解码 = bit0 registered | bit2..3 authMode */
typedef struct miot_adv_form {
    uint8_t       fc0;
    uint8_t       fc1;
    uint8_t       capability;
    const char   *desc;          /* 串口打印用，ASCII */
} miot_adv_form_t;

/**
 * PSK 扫描探测候选形态表（按可能性排序，第 0 项是标准认证基线）。
 * 轮播法：每 5s 换下一项重建广播；App 一旦发起连接，立即在串口打
 * 「SCANNED-ON: form i」——那一种就是该型号被 App 认可的形态，
 * 实测后把它固化为默认，其余项从表里删掉。
 */
const miot_adv_form_t *miot_adv_probe_forms(size_t *count);

/**
 * 解析一段 MiBeacon service data 载荷（嗅探模式用：抓到真机广播后
 * 逐字段打印，照抄进 miot_adv_probe_forms 首项即可）。
 *
 * 位定义（ble_monitor 通用解析与本仓库实测互证）：
 *   frctrl = sd[0] | sd[1]<<8
 *   bit3 = 加密     bit4 = 含 MAC   bit5 = 含 capability
 *   bit6 = 保留     bit7 = mesh     bit8 = registered（已绑定）
 *   bit10..11 = authMode（0..3，2=标准认证）   bit12..15 = beacon 版本
 *
 * @param sd   service data 里 95 FE 之后的帧（不含 UUID）
 * @return 0 成功；负数失败（长度不足以容纳 FC/PID 或声明的字段）
 */
typedef struct miot_adv_info {
    uint16_t       frctrl;        /* 原始 16bit LE */
    uint8_t        version;       /* 4 / 5 */
    uint8_t        encrypted;     /* bit3 */
    uint8_t        mac_include;   /* bit4 */
    uint8_t        cap_include;   /* bit5 */
    uint8_t        mesh;          /* bit7 */
    uint8_t        registered;    /* bit8 */
    uint8_t        auth_mode;     /* bit10..11 */
    uint16_t       pid;
    uint8_t        frame_cnt;
    const uint8_t *mac;           /* 帧内 MAC（空中序），无则 NULL */
    const uint8_t *capability;    /* 无则 NULL */
    const uint8_t *objs;          /* MAC/capability 之后的剩余字节（对象或密文） */
    size_t         objs_len;
} miot_adv_info_t;

int miot_adv_parse(const uint8_t *sd, size_t len, miot_adv_info_t *out);

/* ---------- 12B 身份心跳帧（本仓库 esp32/thermo_gatt 用的形态） ---------- */

/**
 * 构造「未绑定」身份心跳帧（MiBeacon v5，12B）。
 *
 * 帧 = FC(2) | PID(2) | cnt(1) | MAC(6) | Capability(1)，不含 Object、不加密。
 * 为什么长这样：
 *   - 没有 bind_key 就加不了密，未绑定态的加密只会让 App 拿到一堆废字节；
 *   - 官方强制 bit5（Capability Include）=1，因为 App 要靠 BondAbility 决定
 *     走哪种绑定流程，缺了这字节整帧被丢弃；
 *   - bit8 registered 必须为 0，1 表示「已被别的账号绑定」，App 直接不显示。
 *
 * Capability 取 BondAbility=2（后绑定：扫描后直连，设备侧本地确认），
 * 对应本仓库的 0xFE95 GATT 直连认证路径。
 * 与上面的 miot_beacon_frame_unbound() 等价，只是把 fc0/fc1 固化成实测基线
 * （fc0=0x30 / fc1=0x58），调用方少传三个参数。
 *
 * @param bond_ability 0=无绑定 1=前绑定 2=后绑定 3=Combo
 * @return 12，或负值（cap 不足）
 */
int miot_beacon_frame_hello(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                            uint8_t bond_ability, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_BEACON_H */
