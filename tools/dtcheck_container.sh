set -e
cd /tmp/ct
export PATH=/opt/dtv/bin:$PATH
for y in input/trimui,smart-pro-pad.yaml leds/allwinner,sun50i-a100-ledc.yaml serial/snps-dw-apb-uart.yaml; do
  echo "== $y"; make ARCH=arm64 dt_binding_check DT_SCHEMA_FILES=$y 2>&1 | grep -vE "^\s+(CHKDT|LINT|DTEX|DTC|SCHEMA|HOSTCC|HOSTLD)|^make" | tail -5
done
echo "== dtb"; make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CHECK_DTBS=y allwinner/sun50i-a133-trimui-smartpro.dtb 2>&1 | grep -iE "ledc|led-controller|mcu|gamepad|vcc-pad|serial@500" | head -12
