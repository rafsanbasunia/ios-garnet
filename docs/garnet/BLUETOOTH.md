# Bluetooth on the POCO X6 5G

Off unless asked for: `bt=3` on the kernel's command line.

## What works

- the chip switched on, asked which it is, and given its patch and its settings;
- HCI: reset, the controller's version (Bluetooth 5.2) and address, a scan for Bluetooth LE advertisements;
- the controller for programs: `/dev/hakios-bt` carries HCI in H4 packets both ways (`bt_test` reads the version
  through it and counts advertisements).

Not done: Darwin's Bluetooth daemon does not use the controller, so there is nothing in Settings, no pairing and
no audio. The UART stays at 115200 baud, which is enough for commands and Bluetooth LE and not for audio.

## The hardware

| | |
|---|---|
| Chip | a WCN3991 (the device tree calls it `qcom,wcn3990`): the Bluetooth half of the radio chip whose Wi-Fi half is `docs/garnet/WIFI.md` |
| UART | serial engine 5 of QUP 1 at 0x00a94000; pins 14 (CTS), 15 (RTS), 16 (TX), 17 (RX) |
| Clock | GCC: the engine's root at 0x28618, 7.3728 MHz from the 300 MHz PLL output through M/N 384/15625 |
| Supplies | `ldob22` 1.8 V, `ldoe1` 1.8 V, `ldob14` 1.304 V, `ldoe10` 3.312 V (the last three are the Wi-Fi half's too) |
| Reset line | none: the chip is switched off and on with a byte on the UART (0xc0 at 2400 baud, 0xfc at 115200) |

## How it is brought up

`drivers/bluetooth/hakios.btqca.c`, the way Linux's `hci_qca.c` and `btqca.c` do it, on a serial engine that is
polled (`qcom_geni_serial.c` has the registers):

1. the supplies, the UART's clock and pins, the engine as a UART in FIFO mode with hardware flow control;
2. the two power bytes; then the chip's version (vendor command 0xfc00 with 0x19). This phone's answers with
   chip 0x40010320, ROM 0x0302: files `crbtfw32.tlv` and `crnv32.bin`;
3. the patch, 243 bytes to a command (0xfc00 with 0x1e), of which only the last is answered; then the settings,
   each piece answered. In the settings the UART's speed is set to 115200 (entry 17), which the chip takes up at
   the reset that follows; the file's own value is 3.2 Mbaud;
4. HCI reset. From here on the chip uses its sleep protocol on the UART: it says 0xfd before it sends after a
   pause and waits for 0xfc. The driver answers, takes those bytes out, and hands whole packets on.

Half a minute passes between the kernel's start and the controller being there: twelve seconds of waiting
(`bt_delay`) and thirteen for the patch at 115200 baud.

## Firmware

Nothing of the vendor's is in this repository. `devices/garnet/pull-firmware.sh` copies the chip's files from the
phone's `/vendor/bt_firmware/image` (as root) into `fw-bt/`, and `userland/build.sh` puts them into the image's
`fw/`, where the kernel reads them.

## Boot arguments

| | |
|---|---|
| `bt=1` | the serial engine looked at (its clocks are switched on; nothing else) |
| `bt=2` | the chip switched on and asked for its version |
| `bt=3` | patch, settings, HCI reset; then `/dev/hakios-bt` |
| `bt=4` | and a scan counted in the kernel first, for `bt_scan` seconds (5) |
| `bt_delay=<s>` | when this begins after the kernel's start (12) |
| `bt_sample=<n>` | the UART's samples per bit, 16 or 32 (16; the other is tried when the chip does not answer) |
