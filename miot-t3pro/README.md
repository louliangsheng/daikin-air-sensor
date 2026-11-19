# miot-t3pro —— 米家温湿度计 3 Pro 替代固件

给本仓库的硬件提供**第二条固件路线**：不走 ESPHome / Home Assistant，而是把大金
BRY88AA151K / BRY88AB151K 伪装成**米家智能温湿度计 3 Pro**（`MJWSD07MMC`，
PID `0x9AB9`），用**官方原版米家 App** 直接添加、绑定、看数，数据经蓝牙网关
上云，小爱同学可语音查询。

| | ESPHome 固件（仓库根目录） | 本目录 miot-t3pro |
|---|---|---|
| 平台 | Home Assistant / ESPHome | 米家 App（官方原版） |
| 协议 | ESPHome native API | MiBeacon V5 + 0xFE95 GATT（miot-ble-sdk） |
| 传感器 | 温湿度/CO₂/PM2.5/TVOC/CH₂O 全量 | 温湿度 + CO₂ + 电量（3 Pro spec 槽位） |
| 依赖 | 需要 HA 或 ESPHome 热点配网 | 只要一台米家网关（或手机蓝牙） |

## 硬件与引脚（与 ESPHome 配置一致）

| 传感器 | 接口 | 引脚 |
|--------|------|------|
| HDC1080 温湿度 | I2C | SDA=19 SCL=21 |
| CM1106 CO₂ | UART1 9600 | TX=17 RX=16 |
| SW1 面板键 | GPIO | 34（短按=配对唤醒，长按 5s=恢复出厂） |

- 目标芯片：**经典 ESP32**（非 C3）
- PM2.5 / TVOC / CH₂O 本固件**不读取**——米家 3 Pro 的 spec 只有温湿度/CO₂/电量四个槽位

## 编译

需要 **ESP-IDF v6.0+**（mbedTLS 4.x）。

```bash
cd miot-t3pro/esp32/t3pro_adv
idf.py set-target esp32
idf.py build
```

Windows 也可以用 `build.ps1`（内部调用 idf.py）。

## 烧录

**首次必须整片擦除**，否则 NVS 里的旧身份会让米家添加列表过滤掉设备：

```bash
idf.py -p COMxx erase-flash
idf.py -p COMxx flash monitor
```

进 BOOT 模式：短接板上 **GPIO0-GND** 焊盘后上电（LED 不亮即就绪）。
串口接 RXD/TXD/GND，波特率 115200。

## 米家添加

1. 米家 App → 右上角 **+** → 添加设备，等待 **MJWSD07MMC**
2. 点它 → 短接一下 **GPIO0-GND**（或按 SW1）等价真机按键
3. 绑定完成后串口应出现 `REGISTER: did stored` → `bound=1` → `session established`
4. 设备页顶部温度/湿度/CO₂ 出数；关掉手机蓝牙后数值仍更新 = 走的网关广播

## 与上游 miot-ble-sdk 的关系

代码基于 [haohaozi328-arch/miot-ble-sdk](https://github.com/haohaozi328-arch/miot-ble-sdk)
的 `t3pro_adv` 示例移植到大金板，并新增：

- `main/sensors.c` —— HDC1080 / CM1106 真实驱动 + I2C 位敲（bit-bang）扫描与读数
- 位敲读 HDC1080 时读地址必须带 R/W=1（写指针用写地址、读数据用读地址）
- SW1 用 GPIO34（本板面板键，非 C3 的 GPIO9）
- `sdkconfig.defaults` 目标改 `esp32`，控制台走 UART0

## 已知限制

- 米家设置里「连接模式」切换（按键连接/常广播）尚未实现 spec set
- CO₂ 上电约 3 分钟预热，期间固定 550 ppm
- 历史曲线需跨整点后才出现（云端按小时聚合）
- 仅用于学习研究，不得用于冒充真实商品设备（见上游 NOTICE / 免责声明）

## 致谢

- [haohaozi328-arch/miot-ble-sdk](https://github.com/haohaozi328-arch/miot-ble-sdk) —— 认证/广播/spec 全套协议栈
- 本仓库原作者 —— 大金拆机、引脚与 ESPHome 方案
