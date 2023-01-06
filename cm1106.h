// put this file in your esphome folder
// protocol implemented as described in https://en.gassensor.com.cn/Product_files/Specifications/CM1106-C%20Single%20Beam%20NDIR%20CO2%20Sensor%20Module%20Specification.pdf

#include "esphome.h"

class CM1106 : public UARTDevice {
  public:
    CM1106(UARTComponent *parent) : UARTDevice(parent) {}

    void setCo2CalibValue(uint16_t ppm = 400) {
        uint8_t cmd[6];
        memcpy(cmd, CM1106_CMD_SET_CO2_CALIB, sizeof(cmd));
        cmd[3] = ppm >> 8;
        cmd[4] = ppm & 0xFF;
        uint8_t response[4] = {0};
        bool success = sendUartCommand(cmd, sizeof(cmd), response, sizeof(response));

        if(!success) {
            ESP_LOGW(TAG, "Reading data from CM1106 failed!");
            return;
        }

        // check if correct response received
        if(memcmp(response, CM1106_CMD_SET_CO2_CALIB_RESPONSE, sizeof(response)) != 0) {
            ESP_LOGW(TAG, "Got wrong UART response: %02X %02X %02X %02X", response[0], response[1], response[2], response[3]);
            return;
        }

        ESP_LOGD(TAG, "CM1106 Successfully calibrated sensor to %uppm", ppm);

    }

    int16_t getCo2PPM() {
        uint8_t response[8] = {0}; // response: 0x16, 0x05, 0x01, DF1, DF2, DF3, DF4, CRC. PPM: DF1*256+DF2
        bool success = sendUartCommand(CM1106_CMD_GET_CO2, sizeof(CM1106_CMD_GET_CO2), response, sizeof(response));
        
        if(!success) {
            ESP_LOGW(TAG, "Reading data from CM1106 failed!");
            return -1;
        }

        if(!(response[0] == 0x16 && response[1] == 0x05 && response[2] == 0x01)) {
            ESP_LOGW(TAG, "Got wrong UART response: %02X %02X %02X %02X...", response[0], response[1], response[2], response[3]);
            return -1;
        }

        uint8_t checksum = calcCRC(response, sizeof(response));
        if(response[7] != checksum) {
            ESP_LOGW(TAG, "Got wrong UART checksum: 0x%02X - Calculated: 0x%02X", response[7], checksum);
            return -1;
        }

        int16_t ppm = response[3] << 8 | response[4];
        ESP_LOGD(TAG, "CM1106 Received CO₂=%uppm DF3=%02X DF4=%02X", ppm, response[5], response[6]);
        return ppm;
    }

  private:
    const char *TAG = "cm1106";
    uint8_t CM1106_CMD_GET_CO2[4] = {0x11, 0x01, 0x01, 0xED}; // head, len, cmd, [data], crc
    uint8_t CM1106_CMD_SET_CO2_CALIB[6] = {0x11, 0x03, 0x03, 0x00, 0x00, 0x00};
    uint8_t CM1106_CMD_SET_CO2_CALIB_RESPONSE[4] = {0x16, 0x01, 0x03, 0xE6};
    
    // Checksum: 256-(HEAD+LEN+CMD+DATA)%256
    uint8_t calcCRC(uint8_t* response, size_t len) {
        uint8_t crc = 0;
        // last byte of response is checksum, don't calculate it
        for(int i = 0; i < len - 1; i++) {
            crc -= response[i];
        }
        return crc;
    }

    bool sendUartCommand(uint8_t *command, size_t commandLen, uint8_t *response = nullptr, size_t responseLen = 0) {
        // Empty RX Buffer
        while (available()) {
            read();
        }

        // calculate CRC
        command[commandLen - 1] = calcCRC(command, commandLen);

        write_array(command, commandLen);
        flush();

        if(response == nullptr) {
            return true;
        }

        return read_array(response, responseLen);
    }
};

class CM1106Sensor : public PollingComponent, public Sensor {
  private:
    CM1106 *cm1106;

  public:
    CM1106Sensor(UARTComponent *parent, uint32_t update_interval) : PollingComponent(update_interval) { 
        cm1106 = new CM1106(parent);
    }

    float get_setup_priority() const { return setup_priority::DATA; }

    void setup() override {
    }

    void update() override {
        int16_t ppm = cm1106->getCo2PPM();
        if(ppm > -1) {
            publish_state(ppm);
        }
    }
};

class CM1106CalibrateSwitch : public Component, public Switch {
  private:
    CM1106 *cm1106;

  public:
    CM1106CalibrateSwitch(UARTComponent *parent) { 
        cm1106 = new CM1106(parent);
    }

    void write_state(bool state) override {
        if(state) {
            publish_state(state);
            cm1106->setCo2CalibValue();
            turn_off();
        }
        else {
            publish_state(state);
        }
    }
};