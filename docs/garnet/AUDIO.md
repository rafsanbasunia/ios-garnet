# Sound on garnet

## Result

What iOS plays comes out of both speakers: system sounds, the keyboard, music and video. The audio DSP is up about 5 s
after the kernel starts it, beside the system's boot; the amplifiers are on only while there is sound. It is on by
default on this phone (`adsp=0` leaves the DSP alone, `sound=0` keeps the speakers silent).

Not done: the earpiece as a separate route, the microphones, headphones (USB-C), Bluetooth audio, calls.

## The hardware

| | |
|---|---|
| Audio DSP | ADSP of the SM7435 (`adsp.mdt` + `adsp.bNN`, PAS id 1), memory 0x9f500000, 49 MB |
| Its framework | AudioReach: GPR packets on the GLINK channel `adsp_apps` (not the older APR) |
| Speaker bus | the low-power island's primary TDM interface: 96 kHz, 32 bits, 4 slots (12.288 MHz), pins 6-9 of the island's pin controller (0x3440000) |
| Amplifiers | two Awinic AW88261 on I2C (GPIO 24 data, 25 clock): 0x34 (channel 0, reset GPIO 43) and 0x35 (channel 1, reset GPIO 34) |
| SMMU stream | 0x1801 (apps SMMU), the DSP's view of our buffers from 0x10000000 |
| Battery | the same DSP runs the charger and fuel gauge (`PMIC_RTR_ADSP_APPS`): level, voltage, current, temperature |

## How it is built

1. `drivers/audio/hakios.adsp.c` loads the firmware into the DSP through TrustZone, starts it, and brings up GLINK,
   QRTR and GPR. It then asks the DSP's resource manager for the audio hardware, sets the four bus pins, and hands the
   graphs' manager a playback graph: shared-memory input, PCM decoder, PCM converter, TDM interface. The graph takes
   96 kHz, 16 bits, two channels.
2. `drivers/audio/hakios.sound.c` shows the DSP eight pages of our memory through the SMMU and keeps them filled:
   iOS's 48 kHz samples (`/dev/hakios-audio`, written by the audio server's plug-in) go through the speakers' guard
   (`hakios.guard.c`: high-pass, power averages, peak limiter), are doubled to 96 kHz and written period by period
   (10 ms each) as the DSP gives them back.
3. `drivers/audio/hakios.speaker.c` sets the two amplifiers up over I2C as Android's driver does, from the phone's
   own configuration file (its "Music" profile), starts them when sound comes and stops them after three seconds of
   silence. It watches their status, temperature and the battery's voltage while they play.

## Findings

- The TDM interface's configuration is 28 bytes on this firmware, one word more than Linux's structure; that word
  has to be 0 (Android's calibration database has 2 there, which the module refuses).
- Parameters to the DSP's modules carry the header {module instance, parameter, size, error} after a four-word
  command header; the shared-memory map command has no command header.
- The I2C bus has no pull-ups but the pins' own. A repeated start has to wait until the data line is high again
  after the device's acknowledge, or the device takes the read for a write; the clock is driven both ways.
- When the DSP starts, the touch controller's chip-select pin (GPIO 13) loses its configuration: the touch driver
  sets its pins again when a read fails.
- With Wi-Fi and the audio DSP together the kernel's 8 MB area for device mappings ran out (`io_map`): it is 64 MB.
- This phone's loader wants every segment of the DSP's image, `adsp.b00` and `adsp.b01` included.
- On a PC's USB port the charger takes about 0.4 A, less than the phone uses with the screen on: the battery
  level falls slowly although the status says charging.

## Firmware

Nothing of the vendor's is in this repository. `devices/garnet/pull-firmware.sh` copies from the phone, with Android
running, the DSP's image (`/vendor/firmware_mnt/image/adsp.*`) and the amplifiers' configuration
(`/vendor/etc/aw882xx_acf.bin`) into `fw-audio/`. `userland/build.sh` puts them into the system image's `/fw`;
`tools/garnet-wifi-image.sh` puts the small ones into the root image as well. Without them the DSP is not started
and the speakers stay silent; nothing else changes.

## Boot arguments

| | |
|---|---|
| `adsp=0` | do not bring the audio DSP up (no sound, no battery reading through it) |
| `sound=0` | the DSP up, the speakers silent |
| `sound_below=N` | the loudest level, in eighths of a dB under Android's own (default 48: 6 dB under) |
| `adsp_step=N` | bring-up only: do steps 1 to N and stop (14: a test tone into the graph, the amplifiers off; 15: the tone through the speakers, `adsp_below=N` under Android's level for `adsp_tone_secs=N` seconds) |
| `speaker_probe=2` | read-only: ask the two amplifiers who they are |
