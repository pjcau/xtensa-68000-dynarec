# espressif/idf:v5.4 plus Espressif's QEMU esp_develop 9.2.2 (the 9.0.0 one in the
# image has no ESP32-S3 PSRAM). Build it yourself, then point the test scripts at it:
#   docker build -t xjit-idf docker/
#   export XJIT_IMAGE=xjit-idf
FROM espressif/idf:v5.4
# x86_64 or aarch64 (an Apple-silicon Mac runs the arm64 IDF image): the same
# release ships both
RUN mkdir -p /q && cd /q \
 && curl -sL https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-$(uname -m)-linux-gnu.tar.xz | tar xJ \
 && /q/qemu/bin/qemu-system-xtensa --version
