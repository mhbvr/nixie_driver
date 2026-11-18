# Nixie driver board tester (Raspberry Pi Pico 2, C / pico-sdk)

Firmware for testing the driver board: it switches the cathode transistors
(MPSA42) and the anode optocouplers from a command line over USB (the Pico
shows up as a virtual serial port).

* At power-up **every output is OFF**. Outputs go to their inactive level before
  anything else runs.
* Static on/off/toggle for each pin, or several at once
* Turn everything off (`alloff`, or **Ctrl-C** at any time)
* Hardware PWM with a frequency and duty you choose, on any pin
* Round robin over a list of pins, with an on time, an off (blanking) time and a
  cycle count you choose. It runs from a hardware alarm, so the CLI keeps working while it runs.

## Pin map

Edit `src/pins_config.h` to match the schematic. Only the pins listed there are
ever driven. Current board:

| Name    | GPIO      | Function                               |
|---------|-----------|----------------------------------------|
| K0..K9  | GP6..GP15 | cathode transistors (MPSA42), digits 0..9 |
| A0      | GP18      | anode optocoupler                      |

Each pin has an `active_low` flag. `false` (the default) means GPIO high
switches the element ON, which matches GPIO → resistor → MPSA42 base, or GPIO
→ resistor → opto LED → GND. Set it to `true` for an opto LED wired from 3V3 to
the GPIO.

> Until the firmware starts, the GPIOs are inputs and are not driven. Put an
> external 10k–100k pull-down on each transistor base / opto input so everything
> stays off while the Pico resets or is being flashed. On the Pico 2 (RP2350)
> this matters even more: because of erratum RP2350-E9, the internal pull-down
> can't be relied on to hold an input low.

## Build

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
```

```bash
cd firmware && mkdir -p build && cd build
```

```bash
cmake .. && make -j
```

Options:

* `-DPICO_BOARD=pico`: build for the original Pico (RP2040). The default is
  `pico2` (Pico 2, RP2350).

Flash it by holding BOOTSEL while plugging in, then copy `build/nixie_tester.uf2`
to the USB drive that appears (`RP2350` for Pico 2, `RPI-RP2` for the original Pico).

## Connect

Plug the Pico into the PC over USB. It shows up as a serial port: `/dev/ttyACM0`
on Linux, `/dev/tty.usbmodem*` on macOS, or `COMx` on Windows. The baud rate
doesn't matter. The 1200-baud "reboot to BOOTSEL" trick of the SDK is disabled.

```bash
picocom /dev/ttyACM0
```

(minicom, PuTTY and screen work too.) The banner and pin list are printed each
time the port is opened. The **↑** key recalls the last command.

The outputs keep their state if the terminal is closed. Use `alloff` before you
disconnect.

The on-board LED (GP25) blinks at 1 Hz while the firmware is running.

### Troubleshooting: the Pico 2 goes back into BOOTSEL after copying the UF2

The UF2 was built for the wrong chip, most often the original Pico (RP2040).
The Pico 2 accepts the file, finds no valid RP2350 image and reboots back into
the boot ROM drive.

* The board setting is cached in `build/CMakeCache.txt`. A build directory
  configured before the default changed to `pico2` keeps building for `pico`.
  cmake now prints `nixie_tester: PICO_BOARD=..., PICO_PLATFORM=...` and a
  warning if the target isn't RP2350. Fix it with a clean build:

  ```bash
  rm -rf build && mkdir build && cd build && cmake .. && make -j
  ```

* Check the UF2 itself: `picotool info -a build/nixie_tester.uf2` should report
  `RP2350` / `rp2350-arm-s`.
* Use pico-sdk 2.0 or newer. Older SDKs can't build for RP2350.

## Commands

| Command | Description |
|---|---|
| `help` / `?` | help with examples |
| `pins` | list configured pins |
| `status` / `st` | state of every pin (off / ON / PWM / RR) |
| `on <pins> ...` | switch on |
| `off <pins> ...` | switch off |
| `toggle <pins> ...` / `t` | toggle |
| `alloff` / `x` / **Ctrl-C** | stop PWM and round robin, everything off |
| `pwm <pins> <freq> <duty>` | hardware PWM, e.g. `pwm A0 1k 30` |
| `pwm <pins> off` | stop PWM on those pins (they are switched off) |
| `rr <pins> <on> [off] [cycles]` | round robin, e.g. `rr K0-K9 1s 200` |
| `rr stop` | stop round robin (its pins are switched off) |

Argument formats:

* **pins**: `K3`, `GP5`, `5`, `K0,K5,A1`, `K0-K9` (table order; `K9-K0` runs
  in reverse), `2-11` (GPIO range), `cathodes`, `anodes`, `all`. You can mix them:
  `K0-K4,A0`.
* **times**: `500` (ms by default), `500ms`, `200us`, `1.5s`. The minimum is
  20 µs. The off time can be `0`, which means no gap.
* **freq**: `200`, `200hz`, `1k`, `2.5khz`, `1mhz`. On Pico 2 (150 MHz) the range
  is about 9 Hz to 75 MHz. On the original Pico (125 MHz) it is about 7.5 Hz to
  62.5 MHz. Duty resolution is best at low frequencies, where it goes up to
  16 bit.
* **duty**: `0`..`100`, optionally with `%`.
* **cycles**: if it is omitted or `0`, the round robin runs forever.

### Typical test session

```
nixie> on A0                 # anode optocoupler of tube 0 on
nixie> rr K0-K9 1s 200       # walk through all digits: 1 s each, 200 ms dark
nixie> rr stop
nixie> pwm A0 1k 30          # same tube dimmed by PWM on the anode
nixie> on K8
nixie> alloff
nixie> on K0
nixie> rr K0-K9 2ms 100us    # fast scan through the digits
nixie> rr K5 500 500 10      # blink K5 at 1 Hz, 10 times
nixie> status
```

### Rules worth knowing

* A pin that belongs to a running round robin can't be changed by `on`, `off`
  or `pwm`. Use `rr stop` or `alloff` first. Pins outside the round robin keep
  their state, so you can, for example, keep an anode on while the cathodes
  cycle.
* Starting a new `rr` stops the previous one.
* `on` or `off` on a pin running PWM switches it back to plain GPIO.
* Two GPIOs share one PWM slice (GPIO 2n and 2n+1), and so share one
  frequency. They can still have different duties. If you set a different frequency on
  the second pin, the firmware prints a warning and both pins use the new
  frequency.
