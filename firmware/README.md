# Gromofon firmware

ESP32 firmware for Bluetooth audio modulation, a 440 Hz test tone and constant-duty testing. The output is on GPIO18, connected to the TC4420 input.

## Build

- Board: classic ESP32 with Bluetooth Classic. ESP32-S3 and ESP32-C3 are not supported.
- Arduino-ESP32: 3.3.8.
- ESP32-A2DP: 1.8.11.
- Board setting: ESP32 Dev Module, partition scheme **Huge APP**.

Open `gromofon_v03/gromofon_v03.ino` in Arduino IDE. The sketch uses the A2DP PCM callback without I2S output; AudioTools is not needed for this configuration.

## Configuration

The firmware starts disarmed. `MEASURED_MAX_ON_US` is deliberately set to zero, which blocks output until a transformer pulse limit has been established. There is no validated pulse limit for the current hardware yet.

`BENCH_NO_POWER_STAGE=1` enables a separate test configuration with a 3 microsecond pulse limit. Use it only with the power stage disconnected, to inspect the GPIO signal. This value is not a transformer operating recommendation. The default build has this mode disabled.

The initial carrier is 25 kHz, with 10-bit PWM and a 5% duty cap. The allowed output is the smaller of the duty cap and pulse-duration limit. These settings are provisional. The 30% hard ceiling is a software bound, not a verified operating point.

## Serial commands

Use 115200 baud. Commands are case-sensitive; settings can only be changed while disarmed.

| Command | Action |
| --- | --- |
| `?` | Print status |
| `a` | Arm output, if the configuration and selected source are ready |
| `x` | Disarm and clear pending commands |
| `b` | Bluetooth audio mode |
| `t` | 440 Hz tone mode |
| `c` | Constant-duty mode; uses the bias setting |
| `f` / `F` | Decrease / increase carrier by 500 Hz, within 10–60 kHz |
| `d` / `D` | Decrease / increase duty cap by one percentage point |
| `p` / `P` | Decrease / increase bias by 0.05 |
| `g` / `G` | Decrease / increase modulation gain by 0.05 |
| `s` | Toggle linear / square-root mapping |

In Bluetooth mode, pair with **Gromofon**, start playback and wait for buffered audio before sending `a`. The audio path is mono, with a two-stage 2 kHz low-pass filter and an 8 kHz modulation update rate. Square-root mapping is experimental; it does not regulate measured arc power.

## Stop and fault handling

Output ramps up over one second after arming. Bluetooth source changes, missing audio, buffer faults and missed control deadlines disarm it. Restarting playback does not automatically rearm it.

The control task is monitored by a 300 ms task watchdog. A watchdog registration or feed error stops output and blocks rearming until restart. Status includes `wdt`, `fault`, buffer fill and missed-deadline counts.

These are software checks. Hardware PWM can continue while the control task is stalled; neither the deadline check nor the watchdog guarantees immediate shutdown or protects against excess current. The TC4420 shares ground with the ESP32 and does not provide galvanic isolation.

## Validation

Build checked with the versions listed above. The firmware has not been tested on the complete power stage. Transformer limits, switching waveforms, audio performance and fault behavior still need hardware verification.
