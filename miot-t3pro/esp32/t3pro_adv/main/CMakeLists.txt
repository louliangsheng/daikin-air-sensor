# 本文件在 <repo>/esp32/t3pro_adv/main/，仓库根要往上三级
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

idf_component_register(
    SRCS "app_main.c"
         "gatt_miot.c"
         "sensors.c"
         "${SDK_DIR}/src/miot_beacon.c"
         "${SDK_DIR}/src/miot_crypto.c"
         "${SDK_DIR}/src/miot_auth.c"
         "${SDK_DIR}/src/miot_device.c"
         "${SDK_DIR}/src/miot_ag2.c"
         "${SDK_DIR}/src/miot_spec.c"
         "${SDK_DIR}/src/miot_props.c"
         "${SDK_DIR}/src/miot_profiles.c"
         "${SDK_DIR}/port/esp32/miot_port_esp32.c"
    INCLUDE_DIRS "." "${SDK_DIR}/include"
    PRIV_REQUIRES bt nvs_flash mbedtls esp_driver_gpio esp_driver_i2c esp_driver_uart
)

# IDF v6.0 (mbedTLS 4.x)：CCM/ECP/ECDH 的 legacy 函数声明被包在
# MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS 里，不定义就只有上下文类型、没有函数原型。
target_compile_definitions(${COMPONENT_LIB} PRIVATE MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)
