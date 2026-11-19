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
| K0      | GP6       | cathode transistor (MPSA42), digit 0   |
| K9      | GP7       | digit 9                                |
| K8      | GP8       | digit 8                                |
| K1..K7  | GP9..GP15 | digits 1..7                            |
| A0      | GP18      | anode optocoupler, tube 0              |
| A1      | GP17      | anode optocoupler, tube 1              |

Both tubes share the cathodes K0..K9. Each tube has its own anode.

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
| `gpio [pins]` | read the real pin registers and show whether each pin is driven LOW/HIGH, PWM or HIGH-Z |
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

## Multiplexing (two tubes, shared cathodes)

The multiplexing commands add to the commands above; those work as before.
In the mx commands the anode can be written `0` / `1` or `A0` / `A1`. A bare
number in a cathode list is a digit, so `5` means `K5` and `0-9` means `K0-K9`.

| Command | Description |
|---|---|
| `mxa <freq> <duty1> <duty2> <delay>` | start multiplexing (or retune it while it runs) |
| `mxa off` | stop multiplexing. The tube selections are kept |
| `mxhold [time]` | how long the cathodes stay on after the anode goes off (default 100 µs, `0` = none, must be ≤ delay). Without an argument it shows the current value |
| `mxpre [time\|off]` | optional pre-discharge: all cathodes on (anodes off) for this long right before each tube turns on. Default off; hold + pre must be ≤ delay. Without an argument it shows the current value |
| `mx on <anode> <cathodes>` | switch cathode(s) on for that tube |
| `mx off <anode> <cathodes>` | switch cathode(s) off for that tube |
| `mx off <anode>` | blank the tube (also stops its `mxrr`) |
| `mxrr <anode> <cathodes> <on> [off] [cycles]` | round robin of cathodes on one tube |
| `mxrr <anode> stop` | stop that tube's round robin |

### Timing

One period (`1/freq`) is split into four parts:

```
| A0 on            | delay | A1 on            | rest of period (>= delay) |
| duty1 % * period |       | duty2 % * period |                           |
```

The **idle** state is all anodes off and all cathodes off.

* **Idle → tube on:** switch on that tube's cathodes (if any are selected),
  then switch on its anode.
* **Tube on → idle:** switch off the anode, wait the cathode hold time
  (`mxhold`, default 100 µs), then switch off all cathodes.
* So the cathodes of the tube that just went dark stay grounded for the hold
  time while its anode discharges. The rest of the delay is idle. The next
  tube's cathodes and anode switch on only after the full delay.

```
        anode off       hold        idle (all off)        next tube
A0  ‾‾‾‾‾\________________________________________________________
K   ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______________________________/‾‾‾‾‾‾‾‾‾  (K of next tube)
A1  _______________________________________________________/‾‾‾‾‾
         |<--- hold --->|<-------- delay - hold -------->|
         |<------------------- delay ------------------->|
```

* The hold is part of the delay, so `mxa` and `mxhold` refuse a hold longer
  than the delay. A hold of `0` switches the cathodes off together with the
  anode. `mxhold` applies immediately while multiplexing runs.
* Starting, retuning and stopping (`mxa`, `mxa off`, `alloff`) use the same
  steps: anodes off, hold, cathodes off.
* **Optional pre-discharge (`mxpre`, off by default):** at the end of each
  idle, right before a tube turns on, all cathodes are switched on for the
  set time while both anodes stay off. The next tube then selects its own
  cathodes (the others go off) and switches on its anode. This drains charge
  left on the shared, floating cathode lines. Sequence:
  `anode off → hold → idle (all off) → all cathodes on (pre) → tube's cathodes → anode on`.
  Hold + pre must fit in the delay, and the pre-discharge also runs before the
  first tube after `mxa`. With `mxpre off` the timing is exactly as without
  this feature.
* `mxa` checks that `duty1 + duty2 + 2 * delay / period <= 100 %`. It counts
  the delay twice because there are two anode changes per period. It also
  checks that every non-zero part is at least 20 µs long, including the hold
  and `delay - hold`, and that the frequency is between 0.1 Hz and 12.5 kHz.
* A duty of `0` keeps that tube dark.
* The anodes are pulsed even when no cathode is selected, so you can check the
  anode waveform on a scope.

The anodes are switched by a timer interrupt, not by hardware PWM. That way the
cathodes and anodes always change in the right order. Expect a few
microseconds of jitter on the edges.

### Example

```
nixie> mx on 0 5               # tube 0 will show 5
nixie> mx on 1 7               # tube 1 will show 7
nixie> mxa 200 40 40 300us     # 200 Hz: A0 2 ms, 300 us gap, A1 2 ms, 700 us gap
nixie> mxhold 150us            # cathodes stay on 150 us after each anode off
nixie> mxrr 1 0-9 1s           # tube 1 counts 0..9, tube 0 keeps showing 5
nixie> mxrr 0 9-0 500 100 2    # tube 0 counts down twice, then goes blank
nixie> mxa 1k 40 40 200us      # retune on the fly, digits keep running
nixie> status                  # shows timing and what each tube is doing
nixie> mxa off                 # stop, or alloff to stop and forget everything
```

Rules:

* While multiplexing runs, it owns K0..K9, A0 and A1. `on`, `off`, `pwm` and
  `rr` on those pins are refused until you run `mxa off` or `alloff`.
* `mxa` stops a running `rr` that uses any of these pins.
* `mx on` and `mx off` with cathodes are refused while that tube runs `mxrr`.
  Run `mxrr <anode> stop` or `mx off <anode>` first.
* `mx` and `mxrr` can be set up before `mxa` starts. They take effect as soon
  as multiplexing runs.
* `alloff` and Ctrl-C stop multiplexing and clear both tubes.
* The `mxrr` on time should be longer than one multiplexing period, or some
  digits will be skipped.
