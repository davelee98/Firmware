# Configs

Device configs exported from the web toolbox (**Export JSON** →
`oep_config.json`), kept here so factory builds can be reproduced.

## Naming

`config_[Manufacturer Name]_[Model Name].json`

Take both names from the config's `data_extended` packet (`manufacturer_name`,
`model_name`). Keep their capitalisation, and replace spaces with hyphens so
the underscore stays the only separator:

| `manufacturer_name` | `model_name` | File |
|---|---|---|
| Seeed Studio | reTerminal E1001 | `config_Seeed-Studio_reTerminal-E1001.json` |
| Seeed Studio | XIAO nRF54LM20A | `config_Seeed-Studio_XIAO-nRF54LM20A.json` |

## Firmware target: `firmware_targets.json`

Each config is built for exactly one firmware target. `firmware_targets.json`
maps every `config_*.json` here to its PlatformIO env:

```json
"targets": {
  "config_Seeed-Studio_reTerminal-E1004.json": "esp32-s3-N32R8-extuart"
}
```

Add the entry in the same change as the config. The env must be a released
target in `.github/firmware-targets.json`, and its chip must match the
config's `system_config.ic_type`. Matching chip is not enough on its own: the
reTerminal E1004 is an ESP32-S3 but needs `esp32-s3-N32R8-extuart`, not
`esp32-s3-N16R8` (see the note in `platformio.ini`). Validate the whole
directory with:

```sh
./tools/build_factory_image.py --check
```

## Building a factory image

```sh
./tools/build_factory_image.py configs/config_Seeed-Studio_reTerminal-E1004.json
```

The env comes from `firmware_targets.json`; the image lands in `factory_images/`.
See `tools/README.md`.

**Do not commit secrets.** Toolbox exports can include `wifi_config` (Wi-Fi
SSID and password) and `security_config` (the BLE encryption key). Delete those
packets, or blank the password / zero the key and set `encryption_enabled` to
`0x0`, before adding a config here — this directory is public.
`build_factory_image.py` refuses a config that still carries either.

## Schema: `config.yaml`

`config.yaml` is a copy of the web toolbox's config schema, which
`tools/json2config.py` uses for every field's size and type. It must match the
firmware being built. When the protocol gains fields, refresh it from
`https://opendisplay.org/firmware/toolbox/config.yaml` in the same change.
Its `minor_version` is the schema version; the tool warns when a config's
`minor_version` differs (missing fields encode as 0).
