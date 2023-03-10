## 大金空气传感器
下图为BRY88AB151K版，其右上为甲醛扩展侧盖，向后滑动打开；不带甲醛字样的为BRY88AA151K版，不支持甲醛扩展

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/BRY88AB151K.jpg" width="240"><img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/achievements.png" width="450">

## 拆机以及烧录
1.按照图片位置拆下带指示灯的一面，都是卡扣结构，拆开后取出PCB板。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/dismantle.jpg" width="500">

2.将header文件夹中的cm1106.h和pm2005.h(或pm2105.h，根据PM2.5传感器型号选择)放入ESPHome配置文件夹中，根据传感器型号使用相应yaml编译固件，若没有编译环境，可直接使用firmware文件夹中编译好的factory固件刷写。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/PM2.5.jpg" width="500">

3.BRY88AB151K版为短接图片中GPIO0两个针脚（其中一个为GND），短接后上电即可进入BOOT刷写模式，然后将图片中的RXD TXD GND连接到USB转TTL刷机板的TX RX GND上，USB插到电脑上，进入[WEB ESPHome](https://web.esphome.io/) 烧录固件；BRY88AA151K版刷机原理相同，但是PCB板上貌似没有专门刷机孔，需要短接GPIO0和GND进入BOOT后通过芯片U0 RXTX刷写；进入BOOT后PCB板上的LED灯不亮，若无法烧录可尝试对调RXD TXD针脚或一直短接GPIO0/GND并重新上电。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/replication.jpg" width="320"><img src="https://i0.wp.com/randomnerdtutorials.com/wp-content/uploads/2018/08/esp32-pinout-chip-ESP-WROOM-32.png" width="450">

4.刷写完成后重新上电，连接DAIKIN Fallback Hotspot热点进行配网，CO2传感器启动会需要一小段时间，启动期间数值为550ppm；在进行CO2校准时，请确保放入室外或此时CO2浓度足够低。
## 参考链接
- 感谢C佬的[视频教程](https://www.bilibili.com/video/BV1PY4y1y7sE)
- [CM1106二氧化碳传感器](http://www.gassensor.com.cn/CO2Sensor/info_itemid_94.html) [Header文件](https://github.com/LeoDJ/ESPHome_Nodes)
- 感谢[严泽远](https://github.com/nixieclock)大佬的[PM2.5传感器](http://www.gassensor.com.cn/ParticulateSensor/info_itemid_104.html) Header文件以及各种证件照
- 需要加装[ZE08-CH20传感器](http://style.winsensor.com/pro_pdf/ZE08-CH2O.pdf)的可以用MiWyf大佬的[Header文件](https://github.com/miwyf/ze08-ch2o-esphome)
## 交流
- QQ群：[198841186](https://jq.qq.com/?_wv=1027&k=lZAMn5Uo)
- 微信群：

  <img src="https://user-images.githubusercontent.com/4549099/161735971-0540ce1c-eb49-4aff-8cb3-3bdad15e22f7.png" alt="xiaomi miot weixin group" width="100">

## 赏赞
- [不想刷机的老板可以直接购买俺刷好的成品](https://m.tb.cn/h.UpkczuX?tk=OvhBdiaatqy)
- 如果教程帮助到了您，不妨请俺喝杯奶茶

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/wechat.jpg" width="200"><img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/alipay.jpg" width="200">
