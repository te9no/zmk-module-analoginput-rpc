# zmk-module-analoginput-rpc

Runtime-configurable analog input driver for MeKaBu and DYA Studio.

This module is based on `cormoran/zmk-module-template-with-custom-studio-rpc` and the analog polling/reporting model used by `badjeff/zmk-analog-input-driver`.

## Features

- `dya,analog-input` devicetree driver for two-axis analog sticks
- ADC polling and Zephyr input reporting
- Deadzone, center voltage, travel range, scale, invert, event role
- Response curve for push-depth-sensitive output
  - `0`: linear
  - `1`: soft
  - `2`: aggressive
- Runtime custom Studio RPC subsystem (primary): `dya__studio`
- Backward-compatible subsystem aliases: `dya_analog_input`, `dya__analog_input`
- Web UI for full runtime tuning of all device/axis settings
- Web UI demo mode (works without device connection)

## west.yml

Use a ZMK branch/build that includes the custom Studio RPC feature used by DYA Studio.

```yaml
manifest:
  remotes:
    - name: yourname
      url-base: https://github.com/yourname
    - name: cormoran
      url-base: https://github.com/cormoran

  projects:
    - name: zmk-dya-analog-input
      remote: yourname
      revision: main
    - name: zmk
      remote: cormoran
      revision: v0.3+custom-studio-protocol
      import:
        file: app/west.yml
```

## Kconfig

```conf
CONFIG_ADC=y
CONFIG_INPUT=y
CONFIG_DYA_ANALOG_INPUT=y
CONFIG_ZMK_STUDIO=y
CONFIG_DYA_ANALOG_INPUT_STUDIO_RPC=y
```

## DYA Studio subsystem name

This module now registers as `dya__studio` (main identifier).

If your frontend or tooling still references older names, these aliases are still available:

- `dya_analog_input`
- `dya__analog_input`

Optional debug:

```conf
CONFIG_DYA_ANALOG_INPUT_LOG_DBG_RAW=y
CONFIG_DYA_ANALOG_INPUT_LOG_DBG_REPORT=y
CONFIG_DYA_ANALOG_INPUT_LOG_LEVEL_DBG=y
```

## Devicetree

```dts
#include <zephyr/dt-bindings/input/input-event-codes.h>

&adc {
    status = "okay";
};

/ {
    anin0: analog_input_0 {
        compatible = "dya,analog-input";
        sampling-hz = <100>;
        report-interval-ms = <8>;

        x-ch {
            label = "X Axis";
            io-channels = <&adc 2>;
            mv-mid = <1630>;
            mv-min-max = <1600>;
            mv-deadzone = <10>;
            scale-multiplier = <1>;
            scale-divisor = <1>;
            output-min = <0>;
            output-max = <24>;
            response-curve = <2>; /* aggressive */
            invert;
            evt-type = <INPUT_EV_REL>;
            input-code = <INPUT_REL_X>;
        };

        y-ch {
            label = "Y Axis";
            io-channels = <&adc 3>;
            mv-mid = <1630>;
            mv-min-max = <1600>;
            mv-deadzone = <10>;
            scale-multiplier = <1>;
            scale-divisor = <1>;
            output-min = <0>;
            output-max = <24>;
            response-curve = <2>; /* aggressive */
            invert;
            evt-type = <INPUT_EV_REL>;
            input-code = <INPUT_REL_Y>;
        };
    };
};
```

## Output model

The driver calculates output per axis as:

```text
normalized = clamp((abs(mv - mv_mid) - mv_deadzone) / (mv_min_max - mv_deadzone), 0..1)
curved = apply_curve(normalized)
output = output_min + (output_max - output_min) * curved
output = output * scale_multiplier / scale_divisor
```

`aggressive` is the recommended curve for mouse movement or scrolling when you want shallow pushes to move slowly and deep pushes to move quickly.

## Web UI

The Web UI lives under `web/` and supports:

- Listing analog devices
- Editing sampling/report interval
- Editing all axis runtime fields:
  - `name`, `enabled`, `adc_channel`, `mv_mid`, `mv_min_max`, `mv_deadzone`
  - `scale_multiplier`, `scale_divisor`, `invert`, `role`, `report_on_change_only`
  - `output_min`, `output_max`, `response_curve`
- Resetting device runtime settings
- Reading live values

Run locally:

```bash
cd web
npm install
npm run dev
```

Build:

```bash
cd web
npm run build
```

### Demo mode

You can use the UI without a connected device:

1. Open the Web UI
2. Click `Enable Demo Mode`
3. Tune all settings against local demo state

Demo mode is UI-only and does not write to firmware.

## Current limitation

Runtime settings are RAM-only. They apply immediately but are not persisted across reboot yet. Add Zephyr settings support if persistent tuning is required.
