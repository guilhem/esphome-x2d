# X2D shutters for ESPHome

An external ESPHome component for an **ESP32-S3 and a CC1101**, exposing shutter controls through Home Assistant's native ESPHome integration. The node owns the associations and durable rolling counters. No per-shutter YAML is needed.

**Experimental: RF is disabled by default.** The ESP32 transmitter and new-controller association still need physical qualification. This repository provides the implementation and software checks, not a claim of motor compatibility.

## Build the reference node

Use ESPHome **2026.9.1**, ESP-IDF **5.5.5**, an ESP32-S3 with at least **4 MB flash**, and an **868 MHz CC1101 module**. Other ESP32 variants and Arduino builds are rejected.

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r requirements-dev.txt
esphome config examples/esp32-s3.yaml
esphome compile examples/esp32-s3.yaml
```

Copy [the example](examples/esp32-s3.yaml), enter your Wi-Fi credentials, and use a serial connection for the first installation so the dedicated journal partition is installed. Add the node through Home Assistant's ESPHome integration. With RF disabled, the diagnostic reports `transmission_disabled` and the association buttons refuse transmission.

For a configuration outside this checkout, replace the example's local source with:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/guilhem/esphome-x2d
      ref: v0.1.0-alpha.1
    components: [x2d]

x2d:
  cs_pin: GPIO10
  data_pin: GPIO9
  chip_ns: 208500
  transmit_enabled: false
  enrollment_enabled: false
```

The component pins its `x2d-core` dependency to a commit. Keep the node name stable; use Home Assistant to rename shutters.

## Wiring

The example uses this pinout; adapt the SPI bus and the two component pins to your board.

| CC1101 | ESP32-S3 example |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SCLK | GPIO12 |
| SI / MOSI | GPIO11 |
| SO / MISO | GPIO13 |
| CSn | GPIO10 |
| GDO0 | GPIO9, with an external 10 kΩ pull-down to GND |

GDO0 must remain low when the MCU resets while the CC1101 is transmitting. The pull-down and reset behavior require a bench capture before enabling RF. The component verifies the CC1101 asynchronous input configuration before driving that pin. Radio pins must be non-inverted and exclusive.

## Associate and control a shutter

Association currently requires a **private, supervised trial build** described in [qualification](docs/qualification.md). Simply setting `enrollment_enabled: true` does not authorize an unqualified identity profile; compilation requires the explicit trial parameters. Each trial authorizes one slot and one counter pair. Advancing to another trial slot requires an updated private build until the profile is physically qualified.

1. Put the target motor into its documented association mode.
2. Press **Associer un volet**. The node reuses its pending slot, or chooses the lowest unused slot, and durably reserves the new identity and counters before transmitting.
3. Observe the motor. Press **Confirmer l’association** only if it responded as expected. A successful radio emission alone is not confirmation.
4. The node saves the association and restarts. Home Assistant discovers the new `Shutter1` … `Shutter16` entity; unused slots remain hidden.

After a power cut, the pending identity and consumed counters remain stored. Confirmation can finish without retransmitting. A retry always needs an explicit button press and a matching trial authorization. No startup, reconnect or OTA recovery resends a command.

Covers provide **open, close and STOP**, with native **assumed state** and no position percentage. An open/close estimate is published only after a complete emission. STOP, restart and uncertain emissions invalidate it. Home Assistant's native binary-cover rendering can show “open” for that unknown estimate; this is not motor feedback. The node never reports measured movement. An accepted command continues if Home Assistant disconnects.

The global diagnostic identifies the affected slot and outcomes such as `awaiting_confirmation`, `association_counter_mismatch`, `radio_busy`, `counter_exhausted` or `storage_corrupt`. RF identities and suffixes are not exposed. There is no erase, forget or counter-reset button.

## Storage and updates

The journal is a dedicated, unencrypted **64 KiB raw flash partition**, `x2d_journal`, type `0x40`, subtype `0x01`. The core owns its two-bank recovery and wear management. Counters are reserved before RF and never rolled back; a failed or cancelled attempt still consumes them. The final counter and journal reserve remain available for STOP.

Only native ESPHome **application OTA** is supported. Before its first flash write, the component rejects new commands, cancels queued work and drains the current frame boundary. A 250 ms timeout forces the carrier low and marks the result uncertain. OTA errors never replay cancelled work. Configure API encryption and OTA authentication in your local configuration before enabling RF.

Keep the flash size and partition layout unchanged across updates. Custom partitions, partition-table OTA, web/HTTP OTA and captive portal are rejected. The component validates the actual journal address and geometry at boot. A corrupt or missing journal disables RF and the native API, retaining previously discovered Home Assistant entities as unavailable instead of advertising an empty inventory. It never reformats corrupt storage. A full-chip erase destroys the identities and counters and requires a new supervised association; do not restore an old journal snapshot or copy it to another transmitter.

## Checks and limits

```sh
git clone https://github.com/guilhem/x2d-core .core
# Check out X2D_CORE_REF from components/x2d/__init__.py in .core first.
cmake -S . -B build -DX2D_CORE_DIR="$PWD/.core"
cmake --build build
ctest --test-dir build --output-on-failure
python -m unittest discover -s tests -p 'test_*.py' -v
```

The association controller is shared with the RP2040 MySensors adapter in x2d-core;
this ESPHome adapter alone pauses and restarts after confirmation.

CI runs the host controller/encoder checks, schema/code-generation checks and the reference ESP32-S3 compilation. It checks out the exact core revision automatically. Software tests cover reboot recovery, authorization, STOP priority, counter exhaustion, corrupted storage, waveform equivalence and injected timing faults.

The RMT backend uses one continuous transaction at 10 MHz with 96 symbols of prefetch memory. STOP ends at the next complete frame not yet prefetched. A delayed interrupt can cause RMT to repeat stale data: the component detects lateness and reports `unknown`, but cannot prevent already-emitted corruption. Timing faults disable further RF until reboot. The IDF pin is intentional, including the internal clock-inspection API used to reject a rounded RMT clock.

See [the qualification procedure](docs/qualification.md) for the required captures and motor tests. Physical-remote reception, measured position, USB identity migration and general support for other X2D/X3D motor families are outside this version.

Apache-2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
