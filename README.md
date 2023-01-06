## 大金空气传感器
<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/BRY88AB151K.png" width="300">

## 拆机以及烧录
1.按照图片位置拆下带指示灯的一面，都是卡扣结构；注意侧面USB甲醛扩展位置不能硬撬，是滑开的。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/%E6%8B%86%E6%9C%BA.jpg" width="400">

2.短接图片中GPIO0两个针脚（其中一个为GND），短接后上电即可进入BOOT刷写模式（进入BOOT后PCB板上的LED灯不亮），然后将图片中的RX TX GND连接到USB转TTL刷机板的TX RX GND上(侧面USB上的RX TX不能用于刷机)，USB插到电脑上。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/%E7%83%A7%E5%BD%95%E6%8E%A5%E5%8F%A3.jpg" width="400">

3.将cm1106.h和pm2005.h(或者pm2105.h,根据自己PM2.5传感器型号决定)放入ESPHome配置文件夹中，使用daikin-air-sensor.yaml编译固件（没有编译环境可直接使用编译好的固件daikin-air-sensor-factory.bin）。

4.进入[WEB ESPHome](https://web.esphome.io/) 烧录编译好的固件(任意烧录工具均可，无法连接对调USB转TTL刷机板与传感器的RX TX针脚)。

## 参考链接
- [CO2库文件](https://github.com/LeoDJ/ESPHome_Nodes)
- [感谢奋斗大佬的PM2005(PM2105)库文件](https://github.com/nixieclock)

## 交流
- QQ群：[198841186](https://jq.qq.com/?_wv=1027&k=lZAMn5Uo)
- 微信群：

  <img src="https://user-images.githubusercontent.com/4549099/161735971-0540ce1c-eb49-4aff-8cb3-3bdad15e22f7.png" alt="xiaomi miot weixin group" width="100">


## 赏赞
如果帮助到了您，不妨请俺喝杯奶茶。

<img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/wechat.jpg" width="200"><img src="https://raw.githubusercontent.com/louliangsheng/daikin-air-sensor/main/alipay.jpg" width="200">
