# Tools

Helper scripts for building, provisioning, and talking to OpenDisplay devices.
These are advanced tools. They assume you know the wire protocol and the
firmware's config layout.

- `od-device-cli.py` — read, edit, and write a device's config over BLE.
- `provision_firmware.py` — build firmware with a config baked in, or flash a
  config-clearing image.
- `build_factory_image.py` — one step from a config JSON to a flashable
  factory image (encode, build with the config embedded, verify, copy out).
- `json2config.py` — encode a toolbox JSON export (`oep_config.json`) into a
  raw config packet or hex string, using the toolbox's `config.yaml` schema.
- `convert_logo.py` — convert `od_logo.svg` into a packed 1-bit C header for
  the boot screen.
- `uf2conv.py` / `uf2families.json` — convert `.bin`/`.hex` images to UF2 for
  boards that flash via USB mass storage.
- `test_zlib_stream.c` — standalone test harness for the firmware's streaming
  zlib/uzlib decoder (`lib/uzlib`). Not part of the PlatformIO build.
- `test_sensor_axp2101.cpp` — standalone host test for the AXP2101 battery-
  voltage helpers in `src/sensor_axp2101.h` (VBAT decoding, targeted ADC-enable,
  address defaulting, and I2C-failure fall-through). Not part of the PlatformIO
  build. See the header comment for the exact build line.
- `ble_crypto.py` / `config_packet.py` — shared helpers. `ble_crypto.py` is
  inlined into `od-device-cli.py` and kept here for reference. `config_packet.py`
  is imported by `provision_firmware.py`.

## od-device-cli.py

Reads, edits, and writes an OpenDisplay device's config packet.

The config is a small binary blob. It holds display pins, sensor wiring,
Wi-Fi credentials, and similar settings. This tool decodes it to YAML so you
can read and edit it by hand, then encodes it back to binary.

It's self-contained. Run it with `uv run`, or execute it directly if `uv` is
on your `PATH` — the shebang invokes `uv run --script` and installs
dependencies (`bleak`, `pyyaml`, `cryptography`) automatically.

```sh
uv run tools/od-device-cli.py --help
# or, if executable:
./tools/od-device-cli.py --help
```

Commands: `read-config`, `write-config`, `decode-config`, `encode-config`,
`add-sensor`, `read-msd`. Run any command with `--help` for its full options.

### Typical workflow: read, edit, write

Read the config from a device and save it as YAML:

```sh
./tools/od-device-cli.py read-config --addr AA:BB:CC:DD:EE:FF -o config.yaml
```

Edit `config.yaml` in any text editor — change a pin number, add a sensor
block, whatever you need.

Write the edited YAML back to the device:

```sh
./tools/od-device-cli.py write-config --addr AA:BB:CC:DD:EE:FF -i config.yaml
```

### Offline decode/encode

No device needed. Useful for inspecting a saved hex dump, or building a
config to embed at factory-flash time.

```sh
./tools/od-device-cli.py decode-config --config-hex "1D 00 01 ... EC 58"
./tools/od-device-cli.py encode-config --input config.yaml
```

### Other commands

Add a sensor block without hand-editing YAML:

```sh
./tools/od-device-cli.py add-sensor --addr AA:BB:CC:DD:EE:FF --sensor-type 0x0003 --bus-id 0
```

Read live battery voltage and chip temperature (no config change):

```sh
./tools/od-device-cli.py read-msd --addr AA:BB:CC:DD:EE:FF
```

### Encrypted devices

If `security_config.encryption_enabled` is set, pass `--key` with the
device's 16-byte master key (32 hex chars):

```sh
./tools/od-device-cli.py read-config --addr AA:BB:CC:DD:EE:FF --key 1e6d01ca00803339d31ee98ca052da71
```

## json2config.py

Encodes a config exported from the web toolbox (**Export JSON** →
`oep_config.json`) into the binary config packet the firmware stores. It is a
port of the toolbox's own encoder, so the bytes match what the toolbox would
write over BLE.

Field sizes come from the toolbox schema, `configs/config.yaml`; pass
`--schema` to use another copy. Configs kept for factory builds live in
`configs/` too — see `configs/README.md` for naming and for refreshing the
schema.

```sh
./tools/json2config.py configs/config_Seeed-Studio_reTerminal-E1004.json -o config.bin
./tools/json2config.py configs/config_Seeed-Studio_reTerminal-E1004.json --hex

```

For a factory image, use `build_factory_image.py` below — it runs this
encoder, picks the right firmware target, and verifies the result.

Exports can include `wifi_config` and `security_config` blocks. Delete them
from the JSON first unless you want the Wi-Fi password and encryption key
embedded in the packet (and, for a factory build, in plain text in the
firmware image).

## build_factory_image.py

One command from a config JSON (see `configs/`) to a flashable factory image.
It encodes the JSON with `json2config.py`, builds the config's firmware target
with the config embedded, checks the config packet is really inside the image,
and copies the image to `factory_images/` (ESP32 `.bin` images there are
git-ignored; rebuild them from the config):

```sh
./tools/build_factory_image.py configs/config_Seeed-Studio_reTerminal-E1004.json
./tools/build_factory_image.py --check    # validate configs/firmware_targets.json
```

The env comes from `configs/firmware_targets.json`. The build is refused if the
env is not a released target (`.github/firmware-targets.json`) or its chip does
not match the config's `ic_type`. `-e` is only needed for a config that is not
in the mapping (e.g. a one-off export), and must agree with the mapping
otherwise.

| Target | Output | Flash |
|---|---|---|
| ESP32 | `factory_<Manufacturer>_<Model>_<version>_full.bin` — bootloader, partitions and app merged | `esptool.py write-flash 0x0 <file>` |
| nRF52840 | `factory_<Manufacturer>_<Model>_<version>.uf2` (+ `.hex`) | copy the `.uf2` onto the bootloader drive |

`--version` defaults to the latest git tag. The build refuses a JSON that
carries a Wi-Fi password or an enabled/non-zero encryption key unless you pass
`--allow-secrets`. The embedded config is applied only when the device has no
valid stored config, so erase flash first on a device that has been used.
