import configparser
import json
import sys
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from validate_partitions import Partition, load_partitions, validate_partitions


class PartitionValidationTest(unittest.TestCase):
    def test_n16r8_board_profile_is_explicit_and_espidf_only(self) -> None:
        board = json.loads(
            (
                PROJECT_ROOT
                / "boards"
                / "algaguard-esp32-s3-devkitc-1-n16r8.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(board["build"]["mcu"], "esp32s3")
        self.assertEqual(board["build"]["flash_mode"], "qio")
        self.assertEqual(board["build"]["f_flash"], "80000000L")
        self.assertEqual(board["frameworks"], ["espidf"])
        self.assertEqual(board["upload"]["flash_size"], "16MB")
        self.assertEqual(board["upload"]["maximum_size"], 6 * 1024 * 1024)
        self.assertIn("8 MB Octal PSRAM", board["name"])

    def test_platformio_environment_selects_n16r8_profile(self) -> None:
        parser = configparser.ConfigParser()
        parser.read(PROJECT_ROOT / "platformio.ini", encoding="utf-8")
        environment = parser["env:esp32-s3-devkitc-1"]
        self.assertEqual(
            environment["board"],
            "algaguard-esp32-s3-devkitc-1-n16r8",
        )
        self.assertEqual(environment["framework"], "espidf")
        self.assertEqual(environment["board_upload.flash_size"], "16MB")
        self.assertIn("partitions.csv", environment["board_build.partitions"])

    def test_sdk_defaults_enable_16_mb_flash_and_octal_psram(self) -> None:
        defaults = (PROJECT_ROOT / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )
        required = (
            "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
            "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y",
            "CONFIG_SPIRAM=y",
            "CONFIG_SPIRAM_MODE_OCT=y",
            "CONFIG_SPIRAM_SPEED_80M=y",
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"',
            "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
        )
        for setting in required:
            self.assertIn(setting, defaults)

    def test_target_images_encode_safe_boot_header_80_mhz_and_16_mb(self) -> None:
        build = PROJECT_ROOT / ".pio" / "build" / "esp32-s3-devkitc-1"
        if not build.exists():
            self.skipTest("target artifacts are not present in this test job")
        for name in ("bootloader.bin", "firmware.bin"):
            header = (build / name).read_bytes()[:4]
            self.assertEqual(header[0], 0xE9, name)
            # ESP-IDF deliberately boots QIO/QOUT selections through a DIO
            # image header, then the bootloader enables quad mode.
            self.assertEqual(header[2], 0x02, f"{name} has unsafe boot mode")
            self.assertEqual(header[3] & 0x0F, 0x0F, f"{name} is not 80 MHz")
            self.assertEqual(header[3] >> 4, 0x04, f"{name} is not 16 MB")

    def test_repository_layout_is_dual_slot_and_within_16_mb(self) -> None:
        partitions = load_partitions(PROJECT_ROOT / "partitions.csv")
        validate_partitions(
            partitions,
            flash_size=16 * 1024 * 1024,
            firmware_size=1024 * 1024,
            safety_margin=1024 * 1024,
        )

    def test_development_identity_nvs_has_two_slot_capacity_margin(self) -> None:
        partitions = {item.name: item for item in load_partitions(PROJECT_ROOT / "partitions.csv")}
        self.assertEqual(partitions["nvs"].size, 0x10000)
        # Pinned PSA bound: RSA-3072 DER (1,787), certificate (4 KiB), CA chain
        # (12 KiB), metadata (512), plus 25% NVS entry/page overhead.
        one_slot = 1787 + 4096 + 12288 + 512
        required = (2 * one_slot * 125 + 99) // 100
        self.assertLessEqual(required, partitions["nvs"].size)

    def test_missing_otadata_is_rejected(self) -> None:
        partitions = [
            Partition("nvs", "data", "nvs", 0x9000, 0x6000),
            Partition("ota_0", "app", "ota_0", 0x20000, 0x200000),
            Partition("ota_1", "app", "ota_1", 0x220000, 0x200000),
        ]
        with self.assertRaisesRegex(ValueError, "OTA data"):
            validate_partitions(partitions, 0x1000000, 0, 0x100000)

    def test_single_slot_is_rejected(self) -> None:
        partitions = [
            Partition("otadata", "data", "ota", 0xF000, 0x2000),
            Partition("ota_0", "app", "ota_0", 0x20000, 0x200000),
        ]
        with self.assertRaisesRegex(ValueError, "two OTA"):
            validate_partitions(partitions, 0x1000000, 0, 0x100000)

    def test_undersized_slot_is_rejected(self) -> None:
        partitions = [
            Partition("otadata", "data", "ota", 0xF000, 0x2000),
            Partition("ota_0", "app", "ota_0", 0x20000, 0x100000),
            Partition("ota_1", "app", "ota_1", 0x120000, 0x100000),
        ]
        with self.assertRaisesRegex(ValueError, "too small"):
            validate_partitions(partitions, 0x1000000, 0x90000, 0x90000)

    def test_wrong_flash_size_and_overflow_are_rejected(self) -> None:
        partitions = load_partitions(PROJECT_ROOT / "partitions.csv")
        with self.assertRaisesRegex(ValueError, "exactly 16 MB"):
            validate_partitions(partitions, 0x400000, 0, 0x100000)
        overflowing = [
            Partition("otadata", "data", "ota", 0xF000, 0x2000),
            Partition("ota_0", "app", "ota_0", 0x20000, 0x700000),
            Partition("ota_1", "app", "ota_1", 0x720000, 0x900000),
        ]
        with self.assertRaisesRegex(ValueError, "exceeds"):
            validate_partitions(overflowing, 0x1000000, 0, 0x100000)


if __name__ == "__main__":
    unittest.main()
