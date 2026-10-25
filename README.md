<img width="1080" height="534" alt="videoframe_100" src="https://github.com/user-attachments/assets/aa9ddc7a-89f9-4b98-b420-9eaf97638049" />


Gromofon is a project exploring how an electric arc can become a musical instrument. Instead of moving a conventional speaker cone, it produces sound by modulating the heating of the air around the arc.

**Status:** design phase. A draft schematic and the first firmware version are prepared, but no hardware has been built and no working HV prototype has been tested yet. The build is planned for week 8 of the Half Life programme.

## Why Gromofon?

The name combines two Polish words:
- **Gramofon** — a record player.
- **Grom** — thunder or a thunderbolt.

Together, they connect music with electrical discharges.

## Why this project?

I’m interested in high-voltage electronics and wanted to use that interest to build something that plays music. My first goal is to produce a clear test tone and then play music through an electric arc.

## How it would work

1. **Audio source**  
   The firmware can play a 440 Hz test tone or a constant-duty test signal, or take audio from a phone or computer over Bluetooth (the ESP32 acts as an A2DP receiver named "Gromofon"). Stored melodies and a 3.5 mm audio input are possible later additions.

2. **ESP32 control**  
   The ESP32 generates a high-frequency switching signal (25 kHz by default) and varies its duty cycle to modulate the power delivered to the arc. The switching frequency and the maximum pulse duration must be matched to the transformer.

3. **Gate driver and MOSFET**  
   A TC4420 gate driver, powered from 12 V, converts the ESP32’s logic signal into a suitable gate-drive signal for the power MOSFET. 

4. **High-voltage transformer**  
   The MOSFET switches the primary winding of a flyback transformer, which produces the voltage needed to sustain an arc between two electrodes.

5. **Sound generation**  
   Changes in heating make the surrounding air expand and contract, producing sound.

## Current circuit design

<img width="973" height="641" alt="image" src="https://github.com/user-attachments/assets/9a176831-f81c-4492-8e51-e7cf1aa6253e" />

The schematic currently includes:

- 24 V DC input.
- Input filtering capacitors.
- An L7812CV regulator supplying 12 V to the gate driver.
- A TC4420 gate driver.
- An IRFP260N as the proposed power MOSFET.
- A gate resistor, a PWM input pull-down and a gate-to-source pull-down.
- A fuse in the 24 V input; its rating has not been selected yet.
- An RCD clamp intended to limit drain-voltage spikes.

The transformer has not been selected. The RCD clamp values, operating frequency, maximum pulse duration and power level are therefore not finalized. 

The prototype is planned for perfboard assembly, with short gate-drive connections and the high-voltage wiring kept separate from the control electronics.

### Transformer selection

Finding a suitable transformer has been the main unresolved part of the design. Many inexpensive listings do not provide enough information about the windings, operating conditions or output capability to confirm whether they will work with my driver.

I am currently looking for a suitable transformer through local second-hand listings and electronics sellers. If I cannot find one, I will consider an inexpensive flyback transformer from AliExpress, although its specifications may be poorly documented. Another option is to investigate winding my own transformer, but I have not yet confirmed whether that is practical within the project schedule. Either option will require testing before I can choose the final operating settings.

## Firmware

The ESP32 firmware is in [`firmware/gromofon_v03`](firmware/gromofon_v03) (see its [README](firmware/README.md) for build settings and serial commands). It runs on a classic ESP32 and currently provides:

- Bluetooth A2DP audio input, a 440 Hz test tone and a constant-duty test mode.
- Output disabled at startup, an explicit arm command and a one-second soft start.
- A duty cap and a pulse-duration limit. The pulse-duration limit is deliberately left at zero until the transformer has been measured, so the output stays blocked on a normal build.
- Automatic disarming when the firmware detects Bluetooth source changes, missing audio, buffer faults or a missed control deadline, plus a task watchdog. These checks do not guarantee immediate shutdown if the control task stalls.

Both the default and bench configurations compiled successfully with Arduino-ESP32 3.3.8 and ESP32-A2DP 1.8.11.

The normal build has bench mode disabled. Bench mode uses a 3 microsecond pulse limit only for inspecting the GPIO signal with the power stage disconnected; it is not a validated transformer setting.

## Planned features

- Stored melodies.
- An optional 3.5 mm audio input.
- A 3D-printed enclosure and mounting parts.
- Hardware current limiting for a future revision.

## Parts and budget

The requested programme funding is 30 USD. This is the funding request, not a cap on the total project cost. I will pay costs above the grant from my own money.

I already have:
- A classic ESP32 DevKit (30-pin).
- A 24 V power supply recovered from a photocopier.
- An L7812CV regulator.
- Screw terminals.
- Perfboard and wire for the low-voltage assembly.
- A heatsink
- Basic soldering and measurement equipment.
- Access to a 3D printer.

The current parts list is in [BOM.csv](BOM.csv). Prices were converted at approximately 3.90 PLN per US dollar and include shop VAT

<img width="872" height="877" alt="image" src="https://github.com/user-attachments/assets/a5554fcc-130a-4481-be27-c295c893585c" />

## Inspiration and references

### Plasma Channel

**“Unreal Bluetooth Plasma Speaker (3D Printed)”**

Seeing an electric arc play music made me want to try it myself. My version will use an ESP32 for signal generation and modulation rather than a 555 timer.

[Watch the video](https://www.youtube.com/watch?v=yO9Igd3MyWs)

### Plasma speaker article

This article provides another reference for PWM modulation, gate driving, flyback transformers and RCD clamps.

[Read the article](https://conrad-gst.github.io/articles/highVoltage/plasmaSpeakerPWM.html)

## Next steps

- Select a suitable flyback transformer.
- Plan the perfboard layout.
- Test the firmware on the bench with the power stage disconnected.
- Test the low-voltage control section before connecting the HV stage.
- Measure the transformer and set the pulse-duration limit.
- Validate switching waveforms and thermal performance.

## Repository contents

- `README.md` — this file.
- `BOM.csv` — parts list with approximate prices.
- `JOURNAL.md` - development journal synchronized from Half Life.
- `gromofon.kicad_sch` — KiCad schematic.
- `firmware/` — ESP32 firmware and its README.

## Safety and limitations

This project involves high voltage, hot electrodes, UV radiation and gases produced by the arc. Guarding, ventilation, appropriate insulation and safe power disconnection must be considered.

The current design does not include hardware cycle-by-cycle current limiting. Firmware limits and any input fuse do not provide equivalent protection, so component damage remains a risk.

The circuit and its protection components have not yet been validated. This repository documents an experimental work in progress, not a verified build guide.
