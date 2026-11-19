/**
 * @file miot_profiles.h
 * @brief 已验证设备档案 —— 经米家 App 实机互通验证的属性表与上报接口
 *
 * 每个档案 = 一张属性表（miot_prop_t[]）+ 初始化/上报函数。
 * 全部只描述「米家连接与通信」，不包含传感器驱动（实际操作由你的硬件层实现，
 * 通过 on_set 回调或直接改状态变量接入）。
 */
#ifndef MIOT_PROFILES_H
#define MIOT_PROFILES_H

#include "miot_ble/miot_device.h"
#include "miot_ble/miot_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 1. 体脂秤 S400（cgllc.scale.s400 同构） ============
 * 验证状态：绑定/用户资料/spec 读写/事件上报全流程 ✓
 * （事件被 App 解析并广播到插件；插件出数需匹配其订阅 topic，进行中） */
void scale_s400_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt);
/** 完整测量上报序列：过程流(8.1×4) → 结果(8.2) → 11.1022 → 属性变更(11.2/11.3)
 *  host/示例用：一坨全发。真机上必须**按步分帧、帧间留空**，
 *  因为 App 的 ag2 接收窗口同一毫秒只吃第一帧（docs/testing.md 关键要点 2）。 */
void scale_s400_report(miot_device_t *dev, uint32_t weight_01g,
                       uint32_t impedance_01, uint32_t ts);
/** 分步版：step = 0..SCALE_S400_STEPS-1，每步只发一帧，由调用方按 gap 表节流。
 *  weightDoneStatus 只影响 step 3（8.2 结果）的下标 5 字段：
 *    0=normalData 直接入库 / 2=userError「是你吗」 / 4=multipleUsers 弹选人。
 *  ★离线且用户识别不出来（真机屏显 "----"）时必须发 2/4，App 才会把这条
 *  放进「待认领数据」；一直发 0 的话不管体重多少都直接进当前用户历史。 */
#define SCALE_S400_STEPS 7
int  scale_s400_report_step_ex(miot_device_t *dev, int step,
                               uint32_t weight_01g, uint32_t impedance_01, uint32_t ts,
                               uint8_t weight_done_status);
/** 默认 normalData 版（等价 _ex(..., 0)），在线称重继续用它 */
int  scale_s400_report_step(miot_device_t *dev, int step,
                            uint32_t weight_01g, uint32_t impedance_01, uint32_t ts);
/** 离线称重落库：手机不在场（无会话）时也要把体重/阻抗写进属性表。
 *  真机此时把数据存秤里（最多 160 条，官方说明），等"下一次连上手机"补传。
 *  ★队列本体和 NVS 持久化在 app_main（设备层）；这里只写属性表 + 由调用方
 *  通过 scale_s400_offline_set_count() 同步 13.6 / 13.1 out[0] 的条数。 */
void scale_s400_store_offline(uint32_t weight_01g, uint32_t impedance_01, uint32_t ts);
/** 同步离线消息计数（13.6 属性 + 13.1 out[0]）：存一条 +1、补传一条 -1。
 *  一直是 0 的话 App 认为"秤里没数据"，待认领/历史都不会有（实测踩到）。 */
void scale_s400_offline_set_count(uint8_t n);
/** siid 扫射（私有 spec 事件 siid 未知时探查） */
void scale_s400_sweep(miot_device_t *dev, const char *s1, const char *s2);
/** 13.1 sync 的 out 参数（离线消息数 / SN / MAC / 固件 / 电量）。
 *  ★App 开屏必发 13.1，应答缺这 5 个 out 时同步流程不推进，页面停在开屏。 */
int  scale_s400_action_out(uint8_t siid, uint8_t aiid,
                           miot_spec_value_t *outs, int max);
/** 填身份信息（MAC / SN / 固件版本），只影响 13.1 应答内容 */
void scale_s400_set_identity(const char *mac, const char *sn, const char *fw);
/** 13.4 current-time：App 用它对时，同时也是记录时间戳的来源 */
void scale_s400_set_now(uint32_t epoch);
/** 登录成功后主动推一帧状态（3.1005 体重 / 3.1003 电量 / 13.4 时间）：
 *  App 订阅完 spec 通道后等设备说话，1s 内零收包就退订重连 */
void scale_s400_push_status(miot_device_t *dev);

/* ============ 2. 温湿度计（cgllc-cgdk3 / miaomiaoce-t9 双 spec 兼容） ============
 * 验证状态：绑定 + spec 读写 ✓（温度 float 2.1001 / 3.1001，湿度、电量） */
void thermometer_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt);
/** 周期上报温湿度（properties_changed 双 siid 兼容） */
void thermometer_report(miot_device_t *dev, float temp_c, float hum_pct,
                        uint32_t ts);

/* ============ 2b. 温湿度计 3 Pro（MJWSD07MMC，PID 0x9AB9） ============
 * 这台是**双通道**设备：既发加密 MiBeacon 广播（网关用），又支持蓝牙直连
 * 读 spec（手机详情页用）。之前按纯广播型处理（s_tbl=NULL），手机连上来
 * 一个属性都读不到 → 页面恒空，只剩网关一条路。
 * 官方 spec（XiaomiGateway3 #1776 实机核对）：
 *   7.p.1001 温度 float ℃ / 7.p.1002 湿度 uint8 % /
 *   8.p.1029 CO₂ uint16 ppm / 4.p.1003 电量 uint8 %
 * 广播对象是**新系列**：0x5C01 温度 float32 / 0x5C02 湿度 uint8 /
 * 0x601D CO₂ uint16 / 0x4803 电量 uint8（沿用 3 mini 的 0x4C01/0x4C02
 * 会表现为「设备在线但数值全空」，见 2026-11-13 实证）。 */
void t3pro_init(miot_device_t *dev, miot_port_t *port,
                miot_prop_t **tbl, size_t *cnt);
/** 造广播帧时顺手把同一组值同步进 spec 属性表（手机读到的就是当前值） */
void t3pro_update(float temp_c, uint8_t hum_pct, uint16_t co2_ppm, uint8_t batt);

/* ============ 3. 开关型设备（夜灯 / 驱蚊器 0x040A） ============
 * 验证状态：绑定 + spec 开关下发/回报 ✓（set 后立刻回报 0x1012，
 * 页面开关才停在正确位置） */
void switch_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt);

/* ============ 5. 夜灯2（MUE4094RT） ============
 * 验证状态：v54 固件完整打通 ✓（spec 下行：亮度/延时/开关 + 电量回报） */
void night_light_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt);

/* ============ 6. 驱蚊器智能版（WX08ZM，PID 0x040A） ============
 * 验证状态：绑定 + 认证 + spec 上报 ✓（zimi.mosq.v1：2.1 药量 / 3.1 电量 / 4.1 开关） */
void repeller_init(miot_device_t *dev, miot_port_t *port,
                   miot_prop_t **tbl, size_t *cnt);
/** 上报 2.1 药量 + 3.1 电量 + 4.1 开关，一帧三条。
 *  ⚠️ PID 0x040A 对应 zimi.mosq.v1，不是 v2：开关在 4.1(bool)，药量在 2.1(uint8)，
 *  且 v1 没有 mode 属性。报错 siid/piid 或类型会让 App 卡在「设备初始化」。 */
void repeller_report(miot_device_t *dev, uint8_t power, uint8_t battery,
                     uint8_t margin, uint32_t ts);
/** 设备侧当前开关状态（App 下发 set 后立即更新），上报前先读它做镜像 */
uint8_t repeller_power(void);

/* ============ 4. 无线按钮（XMWXKG01LM） ============
 * 验证状态：绑定 + 按钮配置属性读写(3.1020) ✓ */
void button_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt);

/* ============ 通用：绑定/认证与设备层的标准接线 ============
 * 返回 port 初始化好的 auth 实例（内部已接 notify 回调框架）。
 * 移植方只需提供 0x0010/0x0019 的 notify 实现。 */
void profiles_wire_auth(miot_auth_t *auth, miot_device_t *dev,
                        int (*notify_0010)(void *, const uint8_t *, size_t),
                        int (*notify_0019)(void *, const uint8_t *, size_t),
                        void *user);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_PROFILES_H */
