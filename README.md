## [大金空气传感器]([http://daikin.shuishushi.com/product/237.html](https://mp.weixin.qq.com/s/d5tm4ObWZfIJkB5_18t_uA))
<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/BRY88AB151K.png" width="350">

## 拆机以及烧录
1.按照图片位置拆下带指示灯的一面，都是卡扣结构，拆开后取出PCB板。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/%E6%8B%86%E6%9C%BA.jpg" width="450">

2.将cm1106.h和pm2005.h(或pm2105.h，根据PM2.5传感器型号选择)放入ESPHome配置文件夹中，使用daikin-air-sensor.yaml编译固件。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/PM2.5.jpg" width="450">

3.短接图片中GPIO0两个针脚（其中一个为GND），短接后上电即可进入BOOT刷写模式，然后将图片中的RXD TXD GND连接到USB转TTL刷机板的TX RX GND上，USB插到电脑上，进入[WEB ESPHome](https://web.esphome.io/) 烧录固件。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/%E7%83%A7%E5%BD%95%E6%8E%A5%E5%8F%A3.jpg" width="450">

注意:
- 
- 注意侧面USB扩展侧盖不能硬撬，是侧滑打开的，且其RX TX不能用于刷机
- 进入BOOT后PCB板上的LED灯不亮
- 无法烧录可尝试一直短接GPIO0或对调RXD TXD针脚，或重新插拔USB转TTL刷机板
- 若没有编译环境，可直接使用编译好的固件daikin-air-sensor-factory.bin，刷写完成后上电连接DAIKIN Fallback Hotspot热点进行配网
- CO2传感器启动会需要一小段时间，启动期间数值为550ppm
- 进行CO2校准时，请确保放入室外或此时CO2浓度足够低

## 成果展示

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/Achievements.png" width="450">

## 参考链接
- [CO2 cm1106库文件](https://github.com/LeoDJ/ESPHome_Nodes)
- 感谢[奋斗大佬](https://github.com/nixieclock)的PM2005(PM2105)库文件，以及各种证件照，还有[更详细的刷机教程](https://hasstech.cn/?p=1)
- [需要污染等级以及PM1.0和PM10数据的可以用MiWyf大佬的PM2005(PM2105)库文件](https://github.com/miwyf/pm2105)

## 交流
- QQ群：[198841186](https://jq.qq.com/?_wv=1027&k=lZAMn5Uo)
- 微信群：

  <img src="https://user-images.githubusercontent.com/4549099/161735971-0540ce1c-eb49-4aff-8cb3-3bdad15e22f7.png" alt="xiaomi miot weixin group" width="100">


## 赏赞
- [对刷机不了解的也可以直接购买成品](https://m.tb.cn/h.UPvmbi9?tk=PwFddUMFVw2)
- 如果教程帮助到了您，不妨请俺喝杯奶茶。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/wechat.jpg" width="200"><img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/alipay.jpg" width="200">
