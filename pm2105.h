/******************************************************************************
*    _    _   ______   ______   ______  _______  ______  ______  _    _       *
*   | |  | | | |  | | / |      / |        | |   | |     | |     | |  | |      *
*   | |--| | | |__| | '------. '------.   | |   | |---- | |     | |--| |      *
*   |_|  |_| |_|  |_|  ____|_/  ____|_/   |_|   |_|____ |_|____ |_|  |_|      *
*                                                                             *
*******************************************************************************
*   Copyright © HASSTECH® - All Rights Reserved - www.hasstech.cn QQ:6626209  *
*******************************************************************************
*   E-mail      :   yanzeyuan@qq.com
*   QQ          :   6626209
*   微信公众号  :   HASSTECH 智能家居
*   创建者      ：  严泽远
*******************************************************************************
*   ESPHome集成PM2005粉尘传感器协议分析
******************************************************************************/

#include "esphome.h"

uint8_t tmp;					//Temp
uint16_t pm25;				//PM2.5数据
uint16_t pm10;				//PM10数据
uint8_t Sensor_Situation = 0;				//当前传感器状态 Close: 1 Malfunction : 2 Under detecting : 3 Detecting completed: 0x80; other data is invalid.
uint16_t Sensor_MeasuringMode = 0;	//当前传感器测量类型 Single measuring mode: 2 Continuous measuring mode: 3 Dynamic measuring mode: 5 Timing measuring mode: >= 300 (means measuring time)
bool Updated = 0;			//是否有数据更新
    
class pm2005 : public PollingComponent, public Sensor {
 public:
  pm2005() : PollingComponent(1000) {}

  void setup() override {
    // This will be called by App.setup()
    //Wire.begin();
    //delay(100);
	  //Wire.beginTransmission(0x28);
	  //Wire.write(0x16);        	//Frame header
	  //Wire.write(0x01);        	//Number of byte, not including length of device address (From P1 to P7, 7 bytes in total)
	  //Wire.write(0x05);         //Data 1
	  //Wire.write(0x00);         //Data 2, high byte
	  //Wire.write(0x24);         //Data 2 , low byte
	  //Wire.write(0x00);         //Reserved
	  //Wire.write(0x16^0x01^0x03^0x24);         //Data check code
	  //Wire.endTransmission(true);
  }
  
  void update() override {
    // This will be called every "update_interval" milliseconds.
    
  	Wire.requestFrom(0x28, 12);
  	//tmp = Wire.read();	
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P1 0x16                : %02X", tmp);
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P2 Frame length        : %02X", tmp);
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P3 Sensor situation    : %02X", tmp);
  	
  	if(Sensor_Situation != tmp)
  	{
	  	Sensor_Situation = tmp;
	  	if(Sensor_Situation==1)	ESP_LOGD("custom", "Sensor situation: Close.");
	  	else if(Sensor_Situation==2)	ESP_LOGD("custom", "Sensor situation: Malfunction.");
	  	else if(Sensor_Situation==3)	ESP_LOGD("custom", "Sensor situation: Under detecting.");
	  	else if(Sensor_Situation==0x80)
	  	{
	  		Updated = 1;	
	  		ESP_LOGD("custom", "Sensor situation: Detecting completed.");
	  	}	
	  }
  		
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P4 high byte Reserved  : %02X", tmp);
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P5 low byte Reserved   : %02X", tmp);
  	
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P6 high byte PM2.5     : %02X", tmp);
  	pm25 = tmp;
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P7 low byte PM2.5      : %02X", tmp);
  	pm25 = pm25*0x100 + tmp;
  
   	tmp = Wire.read();	//	ESP_LOGD("custom", "P8 high byte PM10      : %02X", tmp);  	
  	pm10 = tmp;
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P9 low byte PM10       : %02X", tmp);
  	pm10 = pm10*0x100 + tmp;
  	
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P10 high byte M mode   : %02X", tmp);
  	Sensor_MeasuringMode = tmp;
  	tmp = Wire.read();	//	ESP_LOGD("custom", "P11 low byte M mode    : %02X", tmp);
  	Sensor_MeasuringMode = Sensor_MeasuringMode*0x100 + tmp;

		if(Updated)
	  {
	 		ESP_LOGD("custom", "PM2.5: %d", pm25);  		
	   	publish_state(pm25);	//输出PM2.5更新
	   	
	  	if(Sensor_MeasuringMode==2)	ESP_LOGD("custom", "The measuring mode of sensor: Single measuring mode.");
	  	else if(Sensor_MeasuringMode==3)	ESP_LOGD("custom", "The measuring mode of sensor: Continuous measuring mode.");
	  	else if(Sensor_MeasuringMode==5)	ESP_LOGD("custom", "The measuring mode of sensor: Dynamic measuring mode.");
	  		
	   	Updated = 0;
	  }
	
  }
};

/******************************************************************************
*   Copyright © HASSTECH® - All Rights Reserved - www.hasstech.cn QQ:6626209  *
*******************************************************************************
 * ┌───┐   ┌───┬───┬───┬───┐ ┌───┬───┬───┬───┐ ┌───┬───┬───┬───┐ ┌───┬───┬───┐
 * │Esc│   │ F1│ F2│ F3│ F4│ │ F5│ F6│ F7│ F8│ │ F9│F10│F11│F12│ │P/S│S L│P/B│ 
 * └───┘   └───┴───┴───┴───┘ └───┴───┴───┴───┘ └───┴───┴───┴───┘ └───┴───┴───┘ 
 * ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───────┐ ┌───┬───┬───┐ 
 * │~ `│! 1│@ 2│# 3│$ 4│% 5│^ 6│& 7│* 8│( 9│) 0│_ -│+ =│ BacSp │ │Ins│Hom│PUp│ 
 * ├───┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─────┤ ├───┼───┼───┤ 
 * │ Tab │ Q │ W │ E │ R │ T │ Y │ U │ I │ O │ P │{ [│} ]│ | \ │ │Del│End│PDn│ 
 * ├─────┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴─────┤ └───┴───┴───┘ 
 * │ Caps │ A │ S │ D │ F │ G │ H │ J │ K │ L │: ;│" '│ Enter  │               
 * ├──────┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴────────┤     ┌───┐     
 * │ Shift  │ Z │ X │ C │ V │ B │ N │ M │< ,│> .│? /│  Shift   │     │ ↑ │     
 * ├─────┬──┴─┬─┴──┬┴───┴───┴───┴───┴───┴──┬┴───┼───┴┬────┬────┤ ┌───┼───┼───┐ 
 * │ Ctrl│    │Alt │         Space         │ Alt│    │    │Ctrl│ │ ← │ ↓ │ → │ 
 * └─────┴────┴────┴───────────────────────┴────┴────┴────┴────┘ └───┴───┴───┘ 
******************************************************************************/