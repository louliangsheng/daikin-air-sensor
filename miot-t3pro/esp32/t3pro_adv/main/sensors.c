/**
 * @file sensors.c
 * @brief 大金 BRY88AB151K 板载传感器驱动
 *
 * HDC1080：TI 标准流程 —— 写指针 0x00 → 等转换（≤15ms）→ 连读 4 字节。
 * CM1106：查询帧 {0x11,0x01,0x01,CRC}，应答 8 字节，ppm=DF1<<8|DF2。
 *
 * ★自适应总线定位（修复温湿度无应答）：
 *   开机与运行期失败时都扫描 th_bus(19/21) 与 pm_bus(23/22)，
 *   自动把 HDC1080(0x40) 挂到实际应答的那条总线上；
 *   同时打印 SDA/SCL 空闲电平与引脚预留状态，区分
 *   「总线卡死 / 引脚被占 / 设备不在 / 地址不对」四类故障。
 */
#include "sensors.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DAIKIN_SENS";

#define I2C_SDA        19
#define I2C_SCL        21
#define PM_SDA         23
#define PM_SCL         22
#define HDC1080_ADDR   0x40
#define HDC1080_CONV_MS 20

#define CM_UART        UART_NUM_1
#define CM_TX_PIN      17
#define CM_RX_PIN      16
#define CM_BAUD        9600
#define CM_RESP_TIMEOUT_MS 300

static i2c_master_bus_handle_t s_bus;       /* 实际挂 HDC1080 的总线 */
static i2c_master_dev_handle_t s_hdc;
static bool s_i2c_ok;
static bool s_uart_ok;
static int  s_th_fail_streak;
static uint32_t s_last_diag;
static bool s_diag_running;

static uint8_t cm_crc(const uint8_t *buf, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum += buf[i];
    return (uint8_t)(256 - sum);
}

/** 扫描一条总线，返回发现数；found_addrs 可为 NULL */
static int i2c_scan_on(i2c_master_bus_handle_t bus, uint8_t *found_addrs, int max_n)
{
    int found = 0;
    char list[80];
    size_t pos = 0;
    list[0] = '\0';
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, pdMS_TO_TICKS(15)) == ESP_OK) {
            if (found < max_n && found_addrs) found_addrs[found] = addr;
            found++;
            if (pos < sizeof(list) - 6) {
                pos += snprintf(list + pos, sizeof(list) - pos, " %02X", addr);
            }
        }
    }
    ESP_LOGI(TAG, "i2c scan: %d dev%s", found, found ? list : " (empty)");
    return found;
}

/** 打印总线静态状态：引脚预留 + 空闲电平（卡死/被占一目了然） */
static void diag_bus_state(void)
{
    ESP_LOGI(TAG, "diag: rsv 19=%d 21=%d 23=%d 22=%d | idle SDA19=%d SCL21=%d SDA23=%d SCL22=%d",
             (int)esp_gpio_is_reserved(BIT64(19)),
             (int)esp_gpio_is_reserved(BIT64(21)),
             (int)esp_gpio_is_reserved(BIT64(23)),
             (int)esp_gpio_is_reserved(BIT64(22)),
             gpio_get_level(19), gpio_get_level(21),
             gpio_get_level(23), gpio_get_level(22));
}

/* ---------------- bit-bang I2C 探针（绕开 I2C 外设） ----------------
 * 判据：总线空闲高 + 位敲发 START/地址后第 9 个时钟读 SDA：
 *   SDA=0 → 设备 ACK（硬件通，问题在驱动）
 *   SDA=1 → 无 ACK（线没接到设备 / 设备没上电）
 */
#define BB_HALF_US  5   /* ~100kHz：半周期 5µs */

static void bb_delay(void)
{
    esp_rom_delay_us(BB_HALF_US);
}

/** 位敲探测一个 7-bit 地址；返回 0=ACK, 1=NACK, 2=总线异常 */
static int bb_probe(int sda, int scl, uint8_t addr7)
{
    /* 配成开漏 GPIO，先释放两线 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = false,
    };
    gpio_config(&io);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    bb_delay();

    if (gpio_get_level(sda) == 0 || gpio_get_level(scl) == 0) {
        return 2;                       /* 空闲就低：总线被拉死 */
    }

    /* START: SDA 1→0 while SCL=1 */
    gpio_set_level(sda, 0); bb_delay();
    gpio_set_level(scl, 0); bb_delay();

    /* 地址字节 MSB first，R/W=0（写） */
    uint8_t byte = (uint8_t)(addr7 << 1);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(sda, (byte >> i) & 1);
        bb_delay();
        gpio_set_level(scl, 1); bb_delay();
        gpio_set_level(scl, 0); bb_delay();
    }

    /* 第 9 时钟读 ACK：释放 SDA，抬 SCL，读电平 */
    gpio_set_level(sda, 1);
    bb_delay();
    gpio_set_level(scl, 1); bb_delay();
    int ack = gpio_get_level(sda);      /* 0 = ACK */
    gpio_set_level(scl, 0); bb_delay();

    /* STOP: SDA 0→1 while SCL=1 */
    gpio_set_level(sda, 0); bb_delay();
    gpio_set_level(scl, 1); bb_delay();
    gpio_set_level(sda, 1); bb_delay();

    return ack == 0 ? 0 : 1;
}

static const char *bb_str(int r)
{
    return r == 0 ? "ACK" : (r == 1 ? "NACK" : "BUS-STUCK");
}

/** 配置引脚为开漏并释放两线（位敲前置） */
static void bb_release(int sda, int scl)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = false,
    };
    gpio_config(&io);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    bb_delay();
}

/** 全地址位敲扫描，只打印 ACK 的设备；返回发现数 */
static int bb_scan_full(int sda, int scl, const char *name)
{
    int found = 0;
    char list[96];
    size_t pos = 0;
    list[0] = '\0';
    bb_release(sda, scl);
    if (gpio_get_level(sda) == 0 || gpio_get_level(scl) == 0) {
        ESP_LOGE(TAG, "bb %s: BUS-STUCK (SDA=%d SCL=%d)", name,
                 gpio_get_level(sda), gpio_get_level(scl));
        return -1;
    }
    for (uint16_t a = 0x08; a <= 0x77; a++) {
        if (bb_probe(sda, scl, (uint8_t)a) == 0) {
            found++;
            if (pos < sizeof(list) - 6) {
                pos += snprintf(list + pos, sizeof(list) - pos, " 0x%02X", a);
            }
        }
    }
    ESP_LOGI(TAG, "bb %s: %d dev%s", name, found, found ? list : " (none)");
    return found;
}

/* ---- 位敲读写原语（绕开坏掉的 I2C 外设） ---- */

/** 发 START + addr+(R/W)，rw=0 写 / rw=1 读；成功返回 0，NACK 返回 1 */
static int bb_start_rw(int sda, int scl, uint8_t addr7, int rw)
{
    bb_release(sda, scl);
    gpio_set_level(sda, 0); bb_delay();          /* START */
    gpio_set_level(scl, 0); bb_delay();
    uint8_t byte = (uint8_t)((addr7 << 1) | (rw & 1));
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(sda, (byte >> i) & 1);
        bb_delay();
        gpio_set_level(scl, 1); bb_delay();
        gpio_set_level(scl, 0); bb_delay();
    }
    gpio_set_level(sda, 1); bb_delay();
    gpio_set_level(scl, 1); bb_delay();
    int ack = gpio_get_level(sda);
    gpio_set_level(scl, 0); bb_delay();
    return ack == 0 ? 0 : 1;
}

/** 写方向 START（兼容旧调用） */
static int bb_start_w(int sda, int scl, uint8_t addr7)
{
    return bb_start_rw(sda, scl, addr7, 0);
}

/** 读一字节并回 ACK(ack=1)/NACK(ack=0) */
static uint8_t bb_read_byte(int sda, int scl, int send_ack)
{
    uint8_t v = 0;
    gpio_set_level(sda, 1);                       /* 释放 SDA */
    for (int i = 0; i < 8; i++) {
        gpio_set_level(scl, 1); bb_delay();
        v = (uint8_t)((v << 1) | (gpio_get_level(sda) & 1));
        gpio_set_level(scl, 0); bb_delay();
    }
    gpio_set_level(sda, send_ack ? 0 : 1); bb_delay();  /* ACK 位 */
    gpio_set_level(scl, 1); bb_delay();
    gpio_set_level(scl, 0); bb_delay();
    gpio_set_level(sda, 1); bb_delay();
    return v;
}

/** 写一字节，返回 0=ACK */
static int bb_write_byte(int sda, int scl, uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(sda, (b >> i) & 1);
        bb_delay();
        gpio_set_level(scl, 1); bb_delay();
        gpio_set_level(scl, 0); bb_delay();
    }
    gpio_set_level(sda, 1); bb_delay();
    gpio_set_level(scl, 1); bb_delay();
    int ack = gpio_get_level(sda);
    gpio_set_level(scl, 0); bb_delay();
    return ack == 0 ? 0 : 1;
}

/** STOP */
static void bb_stop(int sda, int scl)
{
    gpio_set_level(sda, 0); bb_delay();
    gpio_set_level(scl, 1); bb_delay();
    gpio_set_level(sda, 1); bb_delay();
}

/**
 * 位敲读 HDC1080：写指针 0x00 → 等 20ms → 读 4 字节（温+湿）。
 * 成功写入出参并返回 true。
 */
static bool bb_read_hdc1080(int sda, int scl, int16_t *temp_x10, uint8_t *hum_pct)
{
    if (bb_start_w(sda, scl, HDC1080_ADDR) != 0) {
        bb_stop(sda, scl);
        return false;
    }
    if (bb_write_byte(sda, scl, 0x00) != 0) {     /* 指向温度寄存器 */
        bb_stop(sda, scl);
        return false;
    }
    bb_stop(sda, scl);
    vTaskDelay(pdMS_TO_TICKS(HDC1080_CONV_MS));

    /* ★读数据必须用读地址（R/W=1），不是写地址 —— 否则设备 NACK */
    if (bb_start_rw(sda, scl, HDC1080_ADDR, 1) != 0) {
        bb_stop(sda, scl);
        return false;
    }
    uint8_t t_h = bb_read_byte(sda, scl, 1);
    uint8_t t_l = bb_read_byte(sda, scl, 1);
    uint8_t h_h = bb_read_byte(sda, scl, 1);
    uint8_t h_l = bb_read_byte(sda, scl, 0);       /* 最后 NACK */
    bb_stop(sda, scl);

    uint16_t t_raw = (uint16_t)((t_h << 8) | t_l);
    uint16_t h_raw = (uint16_t)((h_h << 8) | h_l);
    if (t_raw == 0x0000 || t_raw == 0xFFFF) return false;
    float t = ((float)t_raw / 65536.0f) * 165.0f - 40.0f;
    float h = ((float)h_raw / 65536.0f) * 100.0f;
    if (t < -40.0f || t > 85.0f) return false;
    if (h < 0.0f) h = 0.0f;
    if (h > 100.0f) h = 100.0f;
    *temp_x10 = (int16_t)(t * 10.0f + (t >= 0 ? 0.5f : -0.5f));
    *hum_pct = (uint8_t)(h + 0.5f);
    return true;
}

/** 记住位敲可用的温湿度总线（找不到硬件驱动时的备用路径） */
static int s_bb_sda = -1, s_bb_scl = -1;

/** 对两条总线做全地址位敲，锁定温湿度芯片所在总线 */
static void bb_scan_all(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));   /* 传感器上电就绪裕量 */

    int n1 = bb_scan_full(I2C_SDA, I2C_SCL, "bb 19/21");
    int n2 = bb_scan_full(PM_SDA, PM_SCL, "bb 23/22");

    /* 0x40 单独重试一次（冷启动偶发不应答） */
    int r40 = bb_probe(I2C_SDA, I2C_SCL, HDC1080_ADDR);
    ESP_LOGI(TAG, "bb 0x40 retry on 19/21 -> %s", bb_str(r40));
    if (r40 == 0) {
        s_bb_sda = I2C_SDA; s_bb_scl = I2C_SCL;
    } else {
        r40 = bb_probe(PM_SDA, PM_SCL, HDC1080_ADDR);
        ESP_LOGI(TAG, "bb 0x40 retry on 23/22 -> %s", bb_str(r40));
        if (r40 == 0) { s_bb_sda = PM_SDA; s_bb_scl = PM_SCL; }
    }
    if (s_bb_sda >= 0) {
        ESP_LOGI(TAG, "HDC1080 reachable via bitbang on %d/%d", s_bb_sda, s_bb_scl);
    } else {
        ESP_LOGW(TAG, "HDC1080 0x40 not ACKing anywhere (n19=%d n23=%d)", n1, n2);
    }
}

/** 临时建一条总线扫描后销毁（不与常驻 bus 冲突） */
static int scan_pins(int sda, int scl, const char *name, uint8_t *found, int max_n)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s init fail: %s", name, esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "%s (%d/%d):", name, sda, scl);
    int n = i2c_scan_on(bus, found, max_n);
    i2c_del_master_bus(bus);
    return n;
}

/**
 * 定位 0x40：先扫 19/21，再扫 23/22。
 * 返回 true 表示找到并已创建 s_bus/s_hdc。
 */
static bool locate_hdc1080(void)
{
    uint8_t found[8];

    /* 优先原 ESPHome th_bus 引脚 */
    int n = scan_pins(I2C_SDA, I2C_SCL, "th_bus 19/21", found, 8);
    int hit = -1;
    for (int i = 0; i < n && i < 8; i++) if (found[i] == HDC1080_ADDR) hit = 0;
    int sda = I2C_SDA, scl = I2C_SCL;

    if (hit < 0) {
        /* 备选 pm_bus 引脚 —— 若板子布线与 yaml 相反会落在这里 */
        n = scan_pins(PM_SDA, PM_SCL, "pm_bus 23/22", found, 8);
        for (int i = 0; i < n && i < 8; i++) if (found[i] == HDC1080_ADDR) { hit = 1; break; }
        if (hit >= 0) { sda = PM_SDA; scl = PM_SCL; }
    }

    if (hit < 0) {
        ESP_LOGE(TAG, "HDC1080 0x40 NOT FOUND on 19/21 nor 23/22");
        return false;
    }
    ESP_LOGI(TAG, "HDC1080 located on %d/%d", sda, scl);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "bus create fail on %d/%d", sda, scl);
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HDC1080_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_hdc) != ESP_OK) {
        ESP_LOGE(TAG, "hdc1080 add fail");
        return false;
    }
    return true;
}

bool daikin_sensors_init(void)
{
    diag_bus_state();
    bb_scan_all();              /* 位敲先行：全地址扫描 + 锁定 0x40 所在总线 */
    /* 位敲能找到 0x40 就直接用位敲路径（硬件 I2C 外设当前不可靠） */
    if (s_bb_sda >= 0) {
        s_i2c_ok = true;
        ESP_LOGI(TAG, "TH path: bitbang on %d/%d (hw i2c skipped)", s_bb_sda, s_bb_scl);
    } else {
        s_i2c_ok = locate_hdc1080();   /* 位敲没找到才试外设（大概率也空） */
    }

    /* ---- CM1106 @ UART1 ---- */
    uart_config_t uart_cfg = {
        .baud_rate = CM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(CM_UART, 512, 512, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(CM_UART, &uart_cfg);
    if (err == ESP_OK) err = uart_set_pin(CM_UART, CM_TX_PIN, CM_RX_PIN,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_uart_ok = (err == ESP_OK);
    if (!s_uart_ok) ESP_LOGE(TAG, "cm1106 uart init failed: %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "init done: hdc1080=%d cm1106=%d", s_i2c_ok, s_uart_ok);
    return s_i2c_ok || s_uart_ok;
}

/** 读数连续失败时的运行期复诊（每 60s 最多一次） */
static void mayberediag(void)
{
    if (s_diag_running || !s_th_fail_streak) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_th_fail_streak < 3 && s_last_diag != 0) return;
    if (s_last_diag && (now - s_last_diag) < 60000) return;
    s_last_diag = now;
    s_diag_running = true;
    ESP_LOGW(TAG, "TH fail streak=%d -> re-diag", s_th_fail_streak);
    diag_bus_state();
    /* 在已建总线上复扫（若总线还活着） */
    if (s_i2c_ok && s_bus) {
        ESP_LOGI(TAG, "rescan on live bus:");
        i2c_scan_on(s_bus, NULL, 0);
    }
    s_diag_running = false;
}

static bool hdc1080_read(int16_t *temp_x10, uint8_t *hum_pct)
{
    /* 主路径：位敲（硬件外设当前全 timeout，位敲已被 0x58 ACK 证明可靠） */
    if (s_bb_sda >= 0) {
        if (bb_read_hdc1080(s_bb_sda, s_bb_scl, temp_x10, hum_pct)) {
            s_th_fail_streak = 0;
            return true;
        }
        s_th_fail_streak++;
        ESP_LOGW(TAG, "bb hdc1080 fail#%d", s_th_fail_streak);
        mayberediag();
        return false;
    }

    /* 后备：硬件 I2C 外设 */
    if (!s_i2c_ok || !s_hdc) return false;

    uint8_t reg = 0x00;
    esp_err_t err = i2c_master_transmit(s_hdc, &reg, 1, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        s_th_fail_streak++;
        ESP_LOGW(TAG, "hdc1080 tx: %s (fail#%d)", esp_err_to_name(err), s_th_fail_streak);
        mayberediag();
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(HDC1080_CONV_MS));

    uint8_t buf[4];
    err = i2c_master_receive(s_hdc, buf, 4, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        s_th_fail_streak++;
        ESP_LOGW(TAG, "hdc1080 rx: %s (fail#%d)", esp_err_to_name(err), s_th_fail_streak);
        mayberediag();
        return false;
    }

    uint16_t t_raw = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t h_raw = ((uint16_t)buf[2] << 8) | buf[3];
    float t = ((float)t_raw / 65536.0f) * 165.0f - 40.0f;
    float h = ((float)h_raw / 65536.0f) * 100.0f;

    if (t_raw == 0x0000 || t_raw == 0xFFFF) return false;
    if (t < -40.0f || t > 85.0f) return false;
    if (h < 0.0f) h = 0.0f;
    if (h > 100.0f) h = 100.0f;

    *temp_x10 = (int16_t)(t * 10.0f + (t >= 0 ? 0.5f : -0.5f));
    *hum_pct = (uint8_t)(h + 0.5f);
    s_th_fail_streak = 0;
    return true;
}

static bool cm1106_read(uint16_t *co2_ppm)
{
    if (!s_uart_ok) return false;

    uint8_t cmd[4] = { 0x11, 0x01, 0x01, 0x00 };
    cmd[3] = cm_crc(cmd, 3);

    uart_flush_input(CM_UART);
    if (uart_write_bytes(CM_UART, cmd, 4) != 4) return false;

    uint8_t resp[8];
    int got = uart_read_bytes(CM_UART, resp, 8, pdMS_TO_TICKS(CM_RESP_TIMEOUT_MS));
    if (got != 8) return false;
    if (resp[0] != 0x16 || resp[1] != 0x05 || resp[2] != 0x01) return false;
    if (resp[7] != cm_crc(resp, 7)) return false;

    *co2_ppm = ((uint16_t)resp[3] << 8) | resp[4];
    return true;
}

int daikin_sensors_read(int16_t *temp_x10, uint8_t *hum_pct, uint16_t *co2_ppm)
{
    int mask = 0;
    if (hdc1080_read(temp_x10, hum_pct)) mask |= DAIKIN_SENS_TH;
    if (cm1106_read(co2_ppm))            mask |= DAIKIN_SENS_CO2;
    return mask;
}
