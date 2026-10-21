<img width="1080" height="534" alt="videoframe_100" src="https://github.com/user-attachments/assets/aa9ddc7a-89f9-4b98-b420-9eaf97638049" />

Gromofon is a plasma speaker project that aims to turn audio signals into sound by modulating an electric arc. Instead of using a conventional speaker cone, it produces sound through changes in the heating of the surrounding air.

## Why Gromofon?

The name combines two Polish words:
- **Gramofon** — a record player.
- **Grom** — lightning bolt.

Together, they connect music with electrical discharges.

## Why this project?

I’m already interested in high-voltage electronics, and I wanted to use that interest to build something that plays music. A plasma speaker seemed like an interesting way to combine electronics, sound and instrument design.

## How it would work

1. **Audio input**  
   An audio signal comes from a phone or computer, either through a cable or over Bluetooth (the ESP32 acts as the receiver).

2. **ESP32 control**  
   An ESP32 generates the high-frequency pulses that drive the arc and uses the audio signal to modulate them. Because this is done in firmware, the same board can also play test tones and stored melodies.

3. **High-voltage stage**  
   A MOSFET stage, driven by the ESP32 through a gate driver and isolated from it, switches a flyback transformer to create a powerful electric arc between two electrodes.

4. **Sound generation**  
   Changes in the arc’s heating make the surrounding air vibrate, producing sound.

The exact circuit and component choices are still being developed. The goal is to keep the hardware as simple as possible and do the rest in software.

## Planned features

- Audio input for an external music source (3.5 mm jack).
- Optional Bluetooth audio input.
- Adjustable carrier frequency, for tuning to the transformer and for initial testing.
- Playback of simple tones and short melodies.

## Inspiration

This project was inspired by **“Unreal Bluetooth Plasma Speaker (3D Printed)” by Plasma Channel**.

Seeing an electric arc play music made me want to try it myself. I plan to develop my own version rather than reproduce the circuit exactly. The main difference is that I’m using an ESP32 for the control and modulation instead of analog timer circuitry.

[Watch the inspiration video](https://www.youtube.com/watch?v=yO9Igd3MyWs)

## Current status

The project is in the planning and design stage. I’m choosing a circuit approach and identifying the parts needed for the first prototype.

Schematics, a parts list, enclosure files and test results will be added as the project develops.

## Safety

This project involves high voltage, hot electrodes and electrical discharges. The design must account for electrical isolation, guarding, ventilation and safe power disconnection. An enclosure alone does not make the system safe.

This repository documents a work in progress, not a verified build guide.
