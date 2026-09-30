# espressif/idf:v5.4 plus Espressif's QEMU esp_develop 9.2.2 (the 9.0.0 one in the
# image has no ESP32-S3 PSRAM). Build it yourself, then point the test scripts at it:
#   docker build -t xjit-idf docker/
#   export XJIT_IMAGE=xjit-idf
FROM espressif/idf:v5.4
RUN mkdir -p /q && cd /q \
 && curl -sL https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz | tar xJ \
 && /q/qemu/bin/qemu-system-xtensa --version
