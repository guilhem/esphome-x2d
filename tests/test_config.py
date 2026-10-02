"""Config/codegen checks only; no physical RF or hardware qualification.

Run with python -m unittest discover -s tests -p 'test_*.py'.
"""

from copy import deepcopy
import logging
from pathlib import Path
import tempfile
import unittest

import yaml

from esphome.__main__ import generate_cpp_contents
from esphome.config import validate_config
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.core import CORE

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples" / "esp32-s3.yaml"


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.raw = yaml.safe_load(EXAMPLE.read_text())
        logging.getLogger("esphome").setLevel(logging.ERROR)

    def validate(self, raw=None):
        CORE.reset()
        CORE.config_path = EXAMPLE
        result = validate_config(deepcopy(self.raw if raw is None else raw), None)
        CORE.config = result
        return result

    def valid(self, raw=None):
        result = self.validate(raw)
        self.assertFalse(result.errors, "\n".join(str(e) for e in result.errors))
        return result

    def invalid(self, message, raw=None):
        result = self.validate(raw)
        self.assertTrue(result.errors)
        self.assertIn(message.lower(), "\n".join(str(e) for e in result.errors).lower())

    def test_generated_entities_are_fixed_and_not_internal(self):
        self.assertNotIn("cover", self.raw)
        first = self.valid()["x2d"]
        self.assertEqual([c["name"] for c in first["_covers"]], [f"Shutter{i}" for i in range(1, 17)])
        ids = [str(c["id"]) for c in first["_covers"]]
        self.assertEqual(ids, [f"x2d_shutter_{i}" for i in range(1, 17)])
        self.assertTrue(all(not c.get("internal", False) for c in first["_covers"]))
        self.assertEqual([c["name"] for c in first["_buttons"]], ["Associer un volet", "Confirmer l’association"])
        self.assertEqual(first["_status"]["entity_category"], "diagnostic")
        self.assertFalse(first["transmit_enabled"])
        self.assertFalse(first["enrollment_enabled"])
        self.assertEqual(ids, [str(c["id"]) for c in self.valid()["x2d"]["_covers"]])

    def test_radio_gates_default_off(self):
        self.raw["x2d"].pop("transmit_enabled")
        self.raw["x2d"].pop("enrollment_enabled")
        config = self.valid()["x2d"]
        self.assertFalse(config["transmit_enabled"])
        self.assertFalse(config["enrollment_enabled"])

    def test_entity_names_still_participate_in_duplicate_validation(self):
        self.raw["cover"] = [{"platform": "template", "name": "Shutter1"}]
        self.invalid("duplicate cover")

    def test_entity_ids_still_participate_in_duplicate_validation(self):
        self.raw["cover"] = [{"platform": "template", "name": "Other shutter", "id": "x2d_shutter_1"}]
        self.invalid("redefined")

    def test_per_shutter_configuration_is_rejected(self):
        for key in ("covers", "_covers", "_buttons", "_status"):
            with self.subTest(key=key):
                raw = deepcopy(self.raw)
                raw["x2d"][key] = []
                self.assertTrue(self.validate(raw).errors)

    def test_single_hub_only(self):
        self.raw["x2d"] = [self.raw["x2d"], deepcopy(self.raw["x2d"])]
        self.assertTrue(self.validate().errors)

    def test_requires_api(self):
        del self.raw["api"]
        self.invalid("requires component api")

    def test_requires_zero_api_reboot_timeout(self):
        for api in ({}, {"reboot_timeout": "15min"}):
            with self.subTest(api=api):
                self.raw["api"] = api
                self.invalid("api.reboot_timeout")

    def test_requires_s3(self):
        self.raw["esp32"].update(board="esp32-s2-saola-1", variant="esp32s2")
        self.invalid("only available on ESP32S3")

    def test_requires_idf(self):
        self.raw["esp32"]["framework"] = {"type": "arduino", "version": "3.3.11"}
        self.invalid("esp-idf")

    def test_requires_exact_idf_version(self):
        self.raw["esp32"]["framework"]["version"] = "5.5.4"
        self.invalid("5.5.5")

    def test_requires_at_least_4mb(self):
        self.raw["esp32"]["flash_size"] = "2MB"
        self.invalid("at least 4MB")
        self.raw["esp32"]["flash_size"] = "8MB"
        self.valid()

    def test_custom_partitions_are_rejected(self):
        self.raw["esp32"]["partitions"] = [{"name": "other", "type": 0x40, "subtype": 2, "size": 4096}]
        self.invalid("custom partitions")
        with tempfile.NamedTemporaryFile(suffix=".csv") as partition:
            self.raw["esp32"]["partitions"] = partition.name
            self.invalid("custom partitions")

    def test_platformio_partition_override_is_rejected(self):
        with tempfile.NamedTemporaryFile(suffix=".csv") as partition:
            self.raw["esphome"]["platformio_options"] = {"board_build.partitions": partition.name}
            self.invalid("partition")

    def test_sdk_partition_override_is_rejected(self):
        self.raw["esp32"]["framework"]["sdkconfig_options"] = {"CONFIG_PARTITION_TABLE_OFFSET": "0x9000"}
        self.invalid("partition table overrides")

    def test_rmt_cache_safe_cannot_be_disabled(self):
        for value in (False, "n", "false", "0"):
            with self.subTest(value=value):
                self.raw["esp32"]["framework"]["sdkconfig_options"] = {"CONFIG_RMT_TX_ISR_CACHE_SAFE": value}
                self.invalid("CONFIG_RMT_TX_ISR_CACHE_SAFE")
        self.raw["esp32"]["framework"]["sdkconfig_options"] = {"CONFIG_RMT_TX_ISR_CACHE_SAFE": "y"}
        generate_cpp_contents(self.valid())
        self.assertTrue(esp32.is_idf_sdkconfig_option_enabled("CONFIG_RMT_TX_ISR_CACHE_SAFE"))

    def test_only_native_app_ota_is_allowed(self):
        self.raw["ota"] = [{"platform": "web_server"}]
        self.raw["web_server"] = {}
        self.invalid("app-only OTA")
        self.raw["ota"] = [{"platform": "http_request"}]
        self.raw["http_request"] = {}
        self.invalid("app-only OTA")
        self.raw["ota"] = [{"platform": "esphome", "allow_partition_access": True}]
        self.invalid("allow_partition_access")
        self.raw.pop("ota")
        self.valid()

    def test_captive_portal_is_rejected(self):
        self.raw["captive_portal"] = {}
        self.invalid("captive_portal")

    def test_enrollment_requires_transmission(self):
        self.raw["x2d"]["enrollment_enabled"] = True
        self.invalid("requires transmit_enabled")
        self.raw["x2d"]["transmit_enabled"] = True
        self.valid()

    def test_invalid_chip_period_is_rejected(self):
        for period in (0, -1, 99, 100, 101, 208501, 1000001, 1638300, 0x100000000):
            with self.subTest(period=period):
                self.raw["x2d"]["chip_ns"] = period
                self.assertTrue(self.validate().errors)

    def test_representable_chip_period_boundaries_are_allowed(self):
        for period in (200, 208500, 1000000):
            with self.subTest(period=period):
                self.raw["x2d"]["chip_ns"] = period
                self.assertEqual(self.valid()["x2d"]["chip_ns"], period)

    def test_enrollment_codegen_requires_private_compile_authorization(self):
        self.raw["x2d"].update(transmit_enabled=True, enrollment_enabled=True)
        config = self.valid()
        generate_cpp_contents(config)
        defines = {define.name for define in CORE.defines}
        self.assertIn("X2D_ENROLLMENT_ENABLED", defines)
        self.assertTrue(esp32.is_idf_sdkconfig_option_enabled("CONFIG_RMT_TX_ISR_CACHE_SAFE"))
        # C++ rejects compilation without explicit authorization for a trial slot.
        self.assertTrue(defines.isdisjoint({
            "X2D_TRIAL_SLOT", "X2D_TRIAL_IDENTITY_SUFFIX", "X2D_TRIAL_EXPECTED_NEXT_COUNTER"
        }))

    def test_spi_requires_mosi_and_miso(self):
        for key in ("miso_pin", "mosi_pin"):
            with self.subTest(key=key):
                raw = deepcopy(self.raw)
                del raw["spi"][key]
                self.invalid(key, raw)

    def test_radio_pins_are_distinct_and_exclusive(self):
        self.raw["x2d"]["data_pin"] = "GPIO10"
        self.invalid("distinct pins")
        self.raw["x2d"]["data_pin"] = {"number": 9, "allow_other_uses": True}
        self.invalid("exclusive")
        self.raw["x2d"]["data_pin"] = {"number": 9, "inverted": True}
        self.invalid("non-inverted")
        self.raw["x2d"]["data_pin"] = "GPIO9"
        self.raw["output"] = [{"platform": "gpio", "pin": {"number": 9, "allow_other_uses": True}, "id": "other_output"}]
        self.invalid("cannot be shared")

    def test_stock_cc1101_ownership_is_rejected(self):
        self.raw["cc1101"] = {"cs_pin": "GPIO8"}
        self.invalid("must own the CC1101")

    def test_codegen_registers_entities_partition_and_ota_listener(self):
        config = self.valid()
        generate_cpp_contents(config)
        cpp = CORE.cpp_main_section
        self.assertEqual(cpp.count("->add_cover("), 16)
        self.assertIn("->set_confirm(false)", cpp)
        self.assertIn("->set_confirm(true)", cpp)
        self.assertIn("->set_data_pin(", cpp)
        self.assertIn("->set_miso_pin(", cpp)
        self.assertIn("->set_status_sensor(", cpp)
        defines = {define.name: define.value for define in CORE.defines}
        self.assertNotIn("X2D_ENROLLMENT_ENABLED", defines)
        self.assertTrue(esp32.is_idf_sdkconfig_option_enabled("CONFIG_RMT_TX_ISR_CACHE_SAFE"))
        self.assertIn("USE_OTA_STATE_LISTENER", defines)
        self.assertEqual(int(str(defines["X2D_JOURNAL_ADDRESS"])), 0x3E0000)
        partition = CORE.data[esp32.KEY_ESP32][esp32.KEY_CUSTOM_PARTITIONS]["x2d_journal"]
        self.assertEqual((partition.type, partition.subtype, partition.size), ("0x40", "0x1", 65536))
        self.assertIn("x2d_journal, 0x40, 0x1, , 0x10000", esp32.get_partition_csv("4MB"))

    def test_journal_address_matches_auto_placed_partition_table(self):
        for flash_mb in (4, 8, 16, 32):
            with self.subTest(flash_mb=flash_mb):
                self.raw["esp32"]["flash_size"] = f"{flash_mb}MB"
                if flash_mb == 32:
                    self.raw["esp32"]["framework"]["advanced"] = {"enable_idf_experimental_features": True}
                config = self.valid()
                generate_cpp_contents(config)
                offset = 0x9000  # partition table ends here on the pinned S3 IDF build
                for row in esp32.get_partition_csv(f"{flash_mb}MB").splitlines():
                    name, kind, _, _, size, _ = [field.strip() for field in row.split(",")]
                    alignment = 0x10000 if kind == "app" else 0x1000
                    offset = (offset + alignment - 1) // alignment * alignment
                    if name == "x2d_journal":
                        break
                    offset += int(size, 0)
                self.assertEqual(name, "x2d_journal")
                self.assertEqual(offset, flash_mb * 1024 * 1024 - 0x20000)
                defines = {define.name: define.value for define in CORE.defines}
                self.assertEqual(offset, int(str(defines["X2D_JOURNAL_ADDRESS"])))

    def test_component_added_partitions_are_rejected_at_codegen(self):
        config = self.valid()
        esp32.add_partition("other_component", 0x40, 2, 4096)
        with self.assertRaisesRegex(cv.Invalid, "other custom partitions"):
            generate_cpp_contents(config)
        CORE.flush_tasks()  # Finish ESPHome finalizers after the expected rejection.


if __name__ == "__main__":
    unittest.main()
