"""One CC1101 hub with fixed, journal-backed shutter identities."""

from esphome import pins
import esphome.codegen as cg
from esphome.components import button, cover, esp32, ota, spi, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ALLOW_OTHER_USES,
    CONF_CLK_PIN,
    CONF_CS_PIN,
    CONF_ID,
    CONF_INVERTED,
    CONF_MISO_PIN,
    CONF_MOSI_PIN,
    CONF_NAME,
    CONF_NUMBER,
    CONF_SPI_ID,
    __version__ as ESPHOME_VERSION,
)
from esphome.core import CORE, CoroPriority, coroutine_with_priority
import esphome.final_validate as fv

AUTO_LOAD = ["cover", "button", "text_sensor"]
DEPENDENCIES = ["esp32", "spi", "api"]

# Generated main.cpp imports esphome; qualify our classes to distinguish ::x2d.
x2d_ns = cg.global_ns.namespace("esphome").namespace("x2d")
X2DComponent = x2d_ns.class_("X2DComponent", cg.Component, spi.SPIDevice)
X2DCover = x2d_ns.class_("X2DCover", cover.Cover)
X2DButton = x2d_ns.class_("X2DButton", button.Button)

CONF_DATA_PIN = "data_pin"
CONF_CHIP_NS = "chip_ns"
CONF_TRANSMIT_ENABLED = "transmit_enabled"
CONF_ENROLLMENT_ENABLED = "enrollment_enabled"
CONF_COVERS = "_covers"
CONF_BUTTONS = "_buttons"
CONF_STATUS = "_status"
X2D_CORE_REF = "85a05600f446d4030cbf8cfacc981cee67d07c96"


def _validate_chip_ns(value):
    value = cv.int_range(min=200, max=1000000)(value)
    if value % 100:
        raise cv.Invalid("chip_ns must be a multiple of 100ns for the 10MHz RMT clock")
    return value


def _validate_options(config):
    if not isinstance(config, dict):
        raise cv.Invalid("x2d requires one hub configuration")
    if ESPHOME_VERSION != "2026.9.1":
        raise cv.Invalid("x2d requires ESPHome 2026.9.1")
    for key in (CONF_COVERS, CONF_BUTTONS, CONF_STATUS):
        if key in config:
            raise cv.Invalid("x2d entities are generated; per-shutter configuration is unsupported")
    if config.get(CONF_ENROLLMENT_ENABLED, False) and not config.get(
        CONF_TRANSMIT_ENABLED, False
    ):
        raise cv.Invalid("enrollment_enabled requires transmit_enabled")
    return config


CONFIG_SCHEMA = cv.All(
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
    esp32.only_on_variant(supported=[esp32.VARIANT_ESP32S3]),
    _validate_options,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(X2DComponent),
            cv.GenerateID(CONF_SPI_ID): cv.use_id(spi.SPIComponent),
            cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_DATA_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_CHIP_NS, default=208500): _validate_chip_ns,
            cv.Optional(CONF_TRANSMIT_ENABLED, default=False): cv.boolean,
            cv.Optional(CONF_ENROLLMENT_ENABLED, default=False): cv.boolean,
            cv.Optional(
                CONF_COVERS,
                default=[
                    {CONF_ID: f"x2d_shutter_{slot}", CONF_NAME: f"Shutter{slot}"}
                    for slot in range(1, 17)
                ],
                visibility=cv.Visibility.YAML_ONLY,
            ): [cover.cover_schema(X2DCover, device_class="shutter")],
            cv.Optional(
                CONF_BUTTONS,
                default=[
                    {CONF_ID: "x2d_associate", CONF_NAME: "Associer un volet"},
                    {CONF_ID: "x2d_confirm", CONF_NAME: "Confirmer l’association"},
                ],
                visibility=cv.Visibility.YAML_ONLY,
            ): [button.button_schema(X2DButton, entity_category="config")],
            cv.Optional(
                CONF_STATUS,
                default={CONF_ID: "x2d_status", CONF_NAME: "Diagnostic X2D"},
                visibility=cv.Visibility.YAML_ONLY,
            ): text_sensor.text_sensor_schema(entity_category="diagnostic"),
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


def _spi_config(config, full_config):
    path = full_config.get_path_for_id(config[CONF_SPI_ID])[:-1]
    return full_config.get_config_for_path(path)


def _final_validate(config):
    full = fv.full_config.get()
    target = full["esp32"]
    if int(target["flash_size"].removesuffix("MB")) < 4:
        raise cv.Invalid("x2d requires at least 4MB flash")
    if target["framework"]["version"] != "5.5.5":
        raise cv.Invalid("x2d requires ESP-IDF 5.5.5")
    if "partitions" in target:
        raise cv.Invalid("x2d owns the partition table; custom partitions can relocate x2d_journal")
    pio = full["esphome"].get("platformio_options", {})
    if any("partition" in key for key in pio):
        raise cv.Invalid("x2d forbids PlatformIO partition overrides")
    sdkconfig = target["framework"].get("sdkconfig_options", {})
    if sdkconfig.get("CONFIG_RMT_TX_ISR_CACHE_SAFE", "y") != "y":
        raise cv.Invalid("x2d requires CONFIG_RMT_TX_ISR_CACHE_SAFE enabled")
    if any(key.startswith("CONFIG_PARTITION_TABLE") for key in sdkconfig):
        raise cv.Invalid("x2d forbids SDK partition table overrides")
    if "captive_portal" in full:
        raise cv.Invalid("x2d does not support captive_portal")
    if any(ota["platform"] != "esphome" for ota in full.get("ota", [])):
        raise cv.Invalid("x2d supports only native ESPHome app-only OTA")
    if any(ota.get("allow_partition_access", False) for ota in full.get("ota", [])):
        raise cv.Invalid("x2d forbids OTA allow_partition_access; use app-only OTA")
    if full.get("web_server", {}).get("ota", False):
        raise cv.Invalid("x2d does not support web OTA")
    if full["api"]["reboot_timeout"].total_milliseconds != 0:
        raise cv.Invalid("x2d requires api.reboot_timeout: 0s")
    if "cc1101" in full:
        raise cv.Invalid("x2d must own the CC1101; remove the stock cc1101 component")

    spi.final_validate_device_schema("x2d", require_mosi=True, require_miso=True)(config)
    bus = _spi_config(config, full)
    owned_pins = [config[CONF_CS_PIN], config[CONF_DATA_PIN]] + [
        bus[key] for key in (CONF_CLK_PIN, CONF_MOSI_PIN, CONF_MISO_PIN)
    ]
    numbers = [pin[CONF_NUMBER] for pin in owned_pins]
    if len(set(numbers)) != len(numbers):
        raise cv.Invalid("x2d CS, GDO0, CLK, MOSI and MISO must use distinct pins")
    for pin in owned_pins:
        if pin.get(CONF_ALLOW_OTHER_USES, False) or pin[CONF_INVERTED]:
            raise cv.Invalid("x2d radio pins must be non-inverted and exclusive")
        if pins.PIN_SCHEMA_REGISTRY.get_count("esp32", "esp32", pin[CONF_NUMBER]) != 1:
            raise cv.Invalid("x2d radio pins cannot be shared with another component")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.FINAL)
async def _check_partition_layout():
    if set(CORE.data[esp32.KEY_ESP32][esp32.KEY_CUSTOM_PARTITIONS]) != {"x2d_journal"}:
        raise cv.Invalid("x2d forbids other custom partitions that can relocate x2d_journal")


async def to_code(config):
    esp32.add_idf_sdkconfig_option("CONFIG_RMT_TX_ISR_CACHE_SAFE", True)
    hub = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(hub, config)
    await spi.register_spi_device(hub, config)
    cg.add(hub.set_data_pin(await cg.gpio_pin_expression(config[CONF_DATA_PIN])))
    bus = _spi_config(config, CORE.config)
    cg.add(hub.set_miso_pin(await cg.get_variable(bus[CONF_MISO_PIN][CONF_ID])))
    cg.add(hub.set_transmit_enabled(config[CONF_TRANSMIT_ENABLED]))
    cg.add(hub.set_enrollment_enabled(config[CONF_ENROLLMENT_ENABLED]))
    if config[CONF_ENROLLMENT_ENABLED]:
        cg.add_define("X2D_ENROLLMENT_ENABLED")
    cg.add(hub.set_chip_ns(config[CONF_CHIP_NS]))
    for slot, conf in enumerate(config[CONF_COVERS], start=1):
        entity = await cover.new_cover(conf)
        cg.add(entity.set_parent(hub))
        cg.add(hub.add_cover(entity, slot))
    for confirm, conf in enumerate(config[CONF_BUTTONS]):
        entity = await button.new_button(conf)
        cg.add(entity.set_parent(hub))
        cg.add(entity.set_confirm(bool(confirm)))
    cg.add(hub.set_status_sensor(await text_sensor.new_text_sensor(config[CONF_STATUS])))
    if "ota" in CORE.config:
        ota.request_ota_state_listeners()
    esp32.add_partition("x2d_journal", 0x40, 1, 65536)
    flash_bytes = int(CORE.config["esp32"]["flash_size"].removesuffix("MB")) * 1024 * 1024
    cg.add_define("X2D_JOURNAL_ADDRESS", flash_bytes - 0x20000)
    CORE.add_job(_check_partition_layout)
    cg.add_library("x2d-core", None, f"https://github.com/guilhem/x2d-core.git#{X2D_CORE_REF}")
