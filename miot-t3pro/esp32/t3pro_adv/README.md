# 米家智能温湿度计 3 Pro（MJWSD07MMC）可绑定固件 —— ESP32-C3

**这是一个可以直接编译烧录、被米家 App 正常添加、数据能上报到云端的完整固件工程。**
2026-11-13 实测（同一天跑通两次，一次在 multi_dev 档案、一次在本工程）：
米家添加成功 → **温度 / 湿度 / CO₂ 三项同时出数** → 小爱同学能语音查询 → 网关解密广播上传云端。

- 目标芯片：ESP32-C3（USB-Serial/JTAG 直连，一根 Type-C 线即可）
- 模拟型号：米家智能温湿度计 3 Pro `MJWSD07MMC`，PID **0x9AB9**（国区 pdid 39609）
- 上报内容：温度 / 湿度 / CO₂ / 电量（四个对象，三帧轮换）
- 通道：MiBeacon V5 加密广播（网关用）+ 0xFE95 GATT（手机用，spec 7.1001/7.1002/8.1029/4.1003）

> 只想看懂原理、不想烧板子？看 [`esp32/thermo_gatt`](../thermo_gatt) 的 README，链路图更完整。
> 本工程与它骨架相同，差别只有三个（PID、对象号、分帧策略），代码里都标了 ★。

## 编译

需要 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.0/esp32c3/get-started/) **v6.0**（其他版本未验证）。

```bash
cd esp32/t3pro_adv
idf.py build
```

没装 IDF 就用 Docker（不用配环境）：

```bash
docker run --rm -v "D:/workspace/temp/miot-ble-sdk:/project" -w /project/esp32/t3pro_adv \
  espressif/idf:v6.0 bash -c "source /opt/esp/idf/export.sh && idf.py build"
```

## 烧录（首次必须整片擦除）

**首次烧录一定要 `erase-flash`**，否则 NVS 里残留的旧身份会让设备以"已绑定"状态广播，
米家添加列表会直接过滤掉它（表现就是「扫描不到」）：

```bash
idf.py -p COM6 erase-flash
idf.py -p COM6 flash monitor
```

`monitor` 里应该看到：

```
I (166) T3PRO: spec table: 4 props (7.1001 / 7.1002 / 8.1029 / 4.1003)
I (456) T3PRO: host synced, MAC xx:xx:xx:xx:xx:xx  bound=0     ← 全新待绑定
I (909) T3PRO: ECDH prepare 390ms
```

## 米家添加

1. 米家 App → 右上角 **+** → **添加设备**，等「附近设备」出现 **MJWSD07MMC**
2. 点它 → App 提示按设备按键 → **短按一下板子的 BOOT 键**（等价真机按键）
3. 等绑定完成，串口依次出现：

```
wr 0010 #1 len=4 cmd=A2                          ← App 发起注册
REGISTER: did stored (utc=...) bound=1           ← 云端下发 did，绑定成功
*** bind state -> 1 (did stored, adv switches to registered) ***
*** session established: spec channel ready ***  ← LOGIN 成功
spec pushed (26.1C / 58% / 480ppm / batt 90%)
```

4. 设备页顶部温度/湿度/CO₂ 出数，小爱同学说「查一下温湿度计的温度」也能答

## 改参数

全部集中在 `main/app_main.c` 顶部：

| 想改什么 | 位置 |
|---|---|
| PID / 设备名 | `T3PRO_PID` / `T3PRO_NAME` |
| 温湿度、CO₂、电量读数 | `v_temp / v_hum / v_co2 / v_batt` 与 `sim_tick()` |
| 广播间隔 | `ADV_PERIOD_MS`（默认 1500ms，一轮 3 帧约 4.5s） |
| 平时是否可连接 | `ADV_CONN_ALWAYS`（1=随时可连，方便添加；0=还原真机"按键才可连"） |
| 恢复出厂 | 长按 BOOT 5 秒（擦 NVS，广播回到未注册态） |

**接真实传感器**：把 `sim_tick()` 换成你的读数来源即可，注意两点：
温度单位是 ℃ 的浮点、湿度是整数百分比、CO₂ 是 uint16 ppm、电量是 uint8 百分比。

## 验收清单

> 2026-11-13 23:0x 实机记录（本工程，芯片物理 MAC `…4D:02`）：
> `connected mtu=23` → `subscribe 0x0010/0x0019 notify=1` → `cmd=A2/15/13` →
> `did stored bound=1` → `REGISTER COMPLETE (0x11)` → App 退订断开（正常收尾）
> → 广播转已绑定帧 `48 59 b9 9a …`，设备页三项数值全部出数，全程 0 崩溃。

| 步骤 | 你应该看到 | 判据 |
|---|---|---|
| ① 烧完复位 | `host synced … bound=0` | 未绑定态 |
| ② 空口广播 | service data 以 `95 FE` 开头，紧接 `30/48/59` | 未绑定帧 FC=0x30 |
| ③ 米家添加 | 附近设备出现 `MJWSD07MMC` | 名字来自扫描响应 |
| ④ 绑定 | `did stored` → `bound=1` | 云端下发 did 才算成 |
| ⑤ 登录 | `LOGIN COMPLETE (0x21)` + `session established` | 0x21 = 成功 |
| ⑥ 手机读数 | `spec pushed (…)` 每 5 秒一条 | 设备页顶部同步变化 |
| ⑦ 网关上报 | 设备页显示「已连接蓝牙网关」，拔掉手机蓝牙后数值仍更新 | 说明走的是广播不是 GATT |
| ⑧ 历史曲线 | 跨过整点后才出现 | 见下方 FAQ，别绑完就下结论 |

## FAQ

**Q：设备页一直显示「未连接手机」，是坏了吗？**
不是。真机 3 Pro 是**按键连接模式**：平时广播不带可连接位，只有按一下按键才开一段唤醒窗口。
数据走「广播 → 网关 → 云端」，完全不依赖手机连接，这个提示是常态。

**Q：顶部有实时读数，历史曲线一直是空的？**
云端按小时聚合，绑完只等十几分钟是判早了，**至少要跨过一次整点**（约 40~60 分钟）。
另外手机蓝牙一直连着设备时设备会停播广播、网关就收不到数据 —— 观察期间把手机蓝牙关掉。

**Q：为什么温度湿度能出，CO₂ 一会儿有一会儿没有？**
一帧广播装不下四个对象（23B 上限），第三帧在 CO₂ 和电量之间交替，所以 CO₂ 是隔帧出现的。
云端会保留最近一次有效值，页面不会因此空掉；真机也是这么分帧发的。

**Q：删除设备后再也扫不到了？**
先 `idf.py -p COM6 erase-flash`（或长按 BOOT 5 秒）清掉绑定身份。设备带着旧 token 时
会以「已注册」形态广播，添加列表按 registered 位过滤，直接看不到它。

**Q：CO₂ 数值为什么不跟着我的呼吸变？**
默认是模拟值（`sim_tick()` 里 420~710 ppm 循环）。接真实传感器见上面「改参数」。

## 踩过的坑（改代码前先读这几条）

1. **对象号按代际换，不能沿用上一代**。3 mini 是 `0x4C01` 温度 / `0x4C02` 湿度，
   3 Pro 换成了 **`0x5C01` / `0x5C02`**（编码相同，都是 float32 / uint8）。
   沿用旧号的后果特别隐蔽：帧、加密、MAC 全对，设备页也显示在线，但**数值恒为 `--`**
   —— 云端按 PID 查产品定义，找不到对应对象就整帧丢掉。
2. **广播必须用无 MAC 形态（`miot_beacon_frame_v5_nomac`）**。带 MAC 的 v5 帧光帧头+MIC
   就 18B，加一个 float32 对象 7B → 25B，超过 BLE 的 23B service data 上限，
   `ble_gap_adv_set_fields` 直接返回 `rc=4`（EMSGSIZE），表现为广播发不出去、设备扫不到。
3. **广播里的 MAC 字段是 LSB 序**。网关用扫描到的 BLE 地址按 LSB 拼 nonce 解密，
   写成正序则永远解不开（`on_sync()` 里有注释）。
4. **spec 属性编号必须是 3 Pro 的**：`7.1001` 温度 / `7.1002` 湿度 / `8.1029` CO₂ /
   `4.1003` 电量。套用 2 代的 `2.1/2.2/2.3` 页面读不到任何值。
5. **模拟值要缓慢波动，别写死**。网关对"值没变"的属性只上报一次，写死会导致
   顶部有读数、历史曲线永远空。
6. IDF v6.0 / mbedTLS 4.x 需要 `MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` 和显式开启
   `CONFIG_MBEDTLS_*_C`（`main/CMakeLists.txt` 与 `sdkconfig.defaults` 已配好）。
7. **未绑定帧必须带 bit9（solicited）**，即 FC 高字节发 `0x2A` 而不是库默认的 `0x28`。
   这一位是「设备请求被连接」：App 点击添加后会一直等它才发起连接，等不到就干等 30 秒
   报「连接超时」。判断依据很干脆——串口侧**一个 `phone connected` 都没有**，
   说明连接根本没建立（不是握手失败）。
8. **BOOT 键要在运行期监听**，不能只在 `app_main()` 里读一次电平。写成一次性判断时，
   用户在配对过程中按 BOOT 完全没反应（现象和上一条一样：连接超时）。现在由 `btn_task`
   持续轮询：短按开 120s 唤醒窗口（`solicited` + 快广播 30~50ms），长按 5s 擦 NVS。

## 免责声明

见仓库根目录 [NOTICE.md](../../NOTICE.md)。本项目仅用于学习研究，设备读数为模拟数据；
不得用于冒充真实商品设备、绕过米家平台安全机制或其他违反平台协议的用途。
