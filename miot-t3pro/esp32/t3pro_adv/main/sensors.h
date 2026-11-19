/**
 * @file sensors.h
 * @brief 大金 BRY88AB151K 板载传感器读数（HDC1080 温湿度 + CM1106 CO2）
 *
 * 引脚与原 ESPHome 配置一致：
 *   HDC1080  I2C  SDA=19 SCL=21（th_bus，地址 0x40）
 *   PM2005   I2C  SDA=23 SCL=22（pm25_bus，0x28 —— 本固件不读，仅开机扫描用）
 *   CM1106   UART1 RX=16 TX=17  9600 8N1
 */
#ifndef DAIKIN_SENSORS_H
#define DAIKIN_SENSORS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 读数结果位掩码 */
#define DAIKIN_SENS_TH   0x01   /* 温湿度读数有效 */
#define DAIKIN_SENS_CO2  0x02   /* CO2 读数有效 */

/** 初始化 I2C 总线 + UART1，打印引脚预留状态与两条总线的设备扫描。 */
bool daikin_sensors_init(void);

/**
 * 读一轮传感器。返回位掩码：DAIKIN_SENS_TH / DAIKIN_SENS_CO2。
 * 只有对应位置位时，该组出参才是有效值；两组互不影响。
 * @param temp_x10  温度 ×10（如 256 = 25.6℃）      [TH 有效时]
 * @param hum_pct   湿度整数百分比                   [TH 有效时]
 * @param co2_ppm   CO2 ppm                          [CO2 有效时]
 */
int daikin_sensors_read(int16_t *temp_x10, uint8_t *hum_pct, uint16_t *co2_ppm);

#ifdef __cplusplus
}
#endif
#endif /* DAIKIN_SENSORS_H */
