# 大金温湿度计3Pro 固件 —— ESP-IDF v6.0.1 / 经典 ESP32 编译
# 用法: .\build.ps1   （只编译不烧录）
$ErrorActionPreference = "Stop"

$env:IDF_TOOLS_PATH = "E:\ESP\.espressif\tools"
$env:IDF_PATH       = "E:\ESP\.espressif\v6.0.1\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "E:\ESP\.espressif\tools\python\v6.0.1\venv"
$env:ESP_ROM_ELF_DIR = "E:\ESP\.espressif\tools\esp-rom-elfs\20241011"
$env:ESP_IDF_VERSION = "6.0.1"
$env:PYTHONUTF8 = "1"
$env:IDF_CCACHE_ENABLE = "0"

$env:PATH = "E:\ESP\.espressif\tools\cmake\4.0.3\bin;" +
    "E:\ESP\.espressif\tools\ninja\1.12.1;" +
    "E:\ESP\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;" +
    "E:\ESP\.espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;" +
    "E:\ESP\.espressif\tools\python\v6.0.1\venv\Scripts;" +
    "$env:PATH"

$PY = "E:\ESP\.espressif\tools\python\v6.0.1\venv\Scripts\python.exe"
$IDF_PY = "E:\ESP\.espressif\v6.0.1\esp-idf\tools\idf.py"

Set-Location "E:\ESP\ESP32\miot-ble-sdk\esp32\t3pro_adv"
& $PY $IDF_PY @args
exit $LASTEXITCODE
