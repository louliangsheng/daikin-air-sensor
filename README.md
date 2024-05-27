## 大金空气传感器
下图为BRY88AB151K版，其右上为甲醛扩展侧盖，向后滑动打开；不带甲醛字样的为BRY88AA151K版，不支持甲醛扩展

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/image/BRY88AB151K.jpg" width="300">

## 编译以及烧录
1.按照参考链接[视频教程](https://www.bilibili.com/video/BV1PY4y1y7sE)进行拆解。

2.根据需要将header文件夹中的pm2005.h、pm2105.h、cm1106.h放入ESPHome配置文件夹中，然后根据传感器选择相应的yaml文件编译固件，其中PM2.5传感器银色金属外壳是pm2005，黑色塑料外壳的是pm2105，若没有编译环境，可直接使用firmware文件夹中编译好的factory固件刷写。

3.将Power&IIC排线插到PCB板上，BRY88AB151K版为短接图片中GPIO0两个针脚（其中一个为GND），短接后上电即可进入BOOT刷写模式，然后将图片中的RXD TXD GND连接到USB转TTL刷机板的TX RX GND上，USB插到电脑上，进入[WEB ESPHome](https://web.esphome.io/) 烧录固件；BRY88AA151K版刷机原理相同，需要短接GPIO0和GND进入BOOT后通过芯片U0 RXTX刷写；进入BOOT后PCB板上的LED灯不亮，若无法烧录可尝试对调RXD TXD针脚或一直短接GPIO0/GND并重新上电。

<img src="https://hasstech.cn/wp-content/uploads/2023/01/mainboard.jpg" width="350"><img src="https://i0.wp.com/randomnerdtutorials.com/wp-content/uploads/2018/08/esp32-pinout-chip-ESP-WROOM-32.png" width="430">

4.刷写完成后重新上电，连接DAIKIN Fallback Hotspot热点进行配网，BRY88AB151K版可安装[甲醛模块BRY88HA111](https://mp.weixin.qq.com/s?search_click_id=7568137682827411716-1679579213259-0915290085&__biz=MzI2MTQ1Mzk5NA==&mid=2247507885&idx=1&sn=4e776305cd6160c879ebb7c7b14114b3&chksm=ea58be68dd2f377e69ff886ba1ef2e415e8a3beee49b2b690f129c9dc010533a1c179c52a617&scene=7&clicktime=1679579213&enterid=1679579213&subscene=0&ascene=65&devicetype=iOS16.3.1&version=18002225&nettype=WIFI&lang=zh_CN&countrycode=CN&fontScale=100&exportkey=n_ChQIAhIQqp9dEP%2FcvKNVHjT9MLSoURLbAQIE97dBBAEAAAAAALfEBAuiQiEAAAAOpnltbLcz9gKNyK89dVj0tP45T%2Fk5VwDaZiPSX7nxBsvQzJfAO87KKKXNywrNXQSAgmyxqgaAGN37ms8QtxnxcvyjU5Ec%2FVkE3UVETqpzCG7kW9sFuH2XkRVj5qAT06RIDakT%2BXpfhqK24j9zp3%2Bxuq2SrOr3gwBi3sqsPCcmRKwUZ3zEUPUGofxD%2B4HrNPWxEKcwCH48Rzs49u%2FqjaAQfOjcn9rVs2X2sWnFcLAP3%2BlwZONDO423YffTUZW4811HiZ755Q%3D%3D&pass_ticket=VG%2B%2FH3lCltnJWspHiUjdHQtR83Q%2BxCGYBd2vxTKXFSsgfMHaA28Ud%2BsfO15hWyOYtfamz1JiG7DhYWErlwaccA%3D%3D&wx_header=3)；配网完成后下载firmware/ota文件夹内设备对应固件进行升级，升级后需要重新配网；CO2传感器启动需要大概3分钟，启动期间数值为550ppm；在进行CO2校准时，请确保放入室外或此时CO2浓度足够低。

<img src="https://mmbiz.qpic.cn/mmbiz_png/HhGBahfz4QZ6IMPibRCbowicxMjSgsZNiaCX7C6A6RgkyOuGIbDl0eCWA5cjsypn0RPjo7rjbAhqvtTxANthGQZmw/640?wx_fmt=png&wxfrom=5&wx_lazy=1&wx_co=1" width="450">

## 参考链接
- 感谢C佬的[视频教程](https://www.bilibili.com/video/BV1PY4y1y7sE)
- 感谢[严泽远](https://github.com/nixieclock)大佬的PM2.5传感器[PM2105](http://www.gassensor.com.cn/ParticulateSensor/info_itemid_104.html) Header文件以及各种证件照
- 感谢MiWyf大佬的甲醛传感器[ZE08-CH20](http://style.winsensor.com/pro_pdf/ZE08-CH2O.pdf) [Header文件](https://github.com/miwyf/ze08-ch2o-esphome)
- [CM1106二氧化碳传感器](http://www.gassensor.com.cn/CO2Sensor/info_itemid_94.html) [Header文件](https://github.com/LeoDJ/ESPHome_Nodes)

## 购买链接
不想刷机的老板可以咸鱼搜索[大金空气传感器 ESPHome](https://m.tb.cn/h.gWw3A1P?tk=Ri44WEw83Ko)，直接购买俺刷好的成品，包含甲醛模块，同时也有兼容原大金APP的甲醛模块单独出售。