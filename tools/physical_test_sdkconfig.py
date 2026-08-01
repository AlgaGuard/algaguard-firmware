"""Ensure physical hardware profiles start with their COM16 UART console."""

from pathlib import Path
import shutil

Import("env")

uart_console_profiles = {
    "esp32-s3-dev-ble-wifi-physical-test",
    "esp32-s3-dev-qr-onboarding-demo",
}

if env.subst("$PIOENV") in uart_console_profiles:
    root = Path(env.subst("$PROJECT_DIR"))
    defaults = root / "sdkconfig.physical-test.defaults"
    generated = root / f"sdkconfig.{env.subst('$PIOENV')}"
    required = (
        "CONFIG_ESP_CONSOLE_UART_DEFAULT=y",
        "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set",
    )
    current = generated.read_text(encoding="utf-8") if generated.exists() else ""
    if not all(entry in current for entry in required):
        shutil.copyfile(defaults, generated)
        print("Physical hardware SDK config initialized with UART console on COM16.")
