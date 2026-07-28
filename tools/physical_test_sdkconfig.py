"""Ensure only the physical-test profile starts with its UART console config."""

from pathlib import Path
import shutil

Import("env")

if env.subst("$PIOENV") == "esp32-s3-dev-ble-wifi-physical-test":
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
        print("Physical-test SDK config initialized with UART console on COM16.")
