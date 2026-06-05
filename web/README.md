# DYA Analog Input Web UI

Web UI for the `dya__studio` analog input custom Studio subsystem.

This UI is for runtime-tunable analog input devices such as joystick-like ADC
modules. It lists firmware-exposed devices, edits device timing, and applies per
axis configuration through ZMK Studio custom RPC.

## Runtime Behavior

- The UI searches for `dya__studio`, `dya_analog_input`, `dya__analog_input`,
  `analog_input`, then `analoginput`.
- Refresh and write operations are serialized so repeated clicks do not start
  overlapping RPC transactions.
- RPC calls use a bounded timeout. If the subsystem is missing or the device is
  not responding, the UI returns to an operable state and shows an error.
- `sampling_hz` and `report_interval_ms` edits are local until
  `Apply Device Settings` is pressed. This avoids writing firmware settings on
  every keypress.
- Axis changes are edited locally in each axis card and written with
  `Apply Axis`.
- Demo mode never writes to firmware.

## Usage

```sh
npm ci
npm run dev -- --host 127.0.0.1 --port 5173
```

## Build

```sh
npm run build
```

Run `npm run generate` only after changing
`../proto/dya/analog_input/analog_input.proto`.

## Deployment

GitHub Pages deployment is handled by `.github/workflows/web-ui-pages.yml`.
The workflow builds `web/` with Node.js 24 and publishes `web/dist`.
