# Bluetooth on the POCO X6 5G

On (`bt=3`) with the phone's Bluetooth firmware in the image (`fw-bt/`, below).

## What works

- the chip switched on, asked which it is, and given its patch and its settings;
- HCI: reset, the controller's version (Bluetooth 5.2) and address, a scan for Bluetooth LE advertisements;
- the controller for programs: `/dev/hakios-bt` carries HCI in H4 packets both ways (`bt_test` reads the version
  through it and counts advertisements);
- Darwin's Bluetooth daemon on it: its stack comes up, Settings shows Bluetooth switched on and searches, the
  daemon scans for Bluetooth LE devices and runs inquiries;
- Bluetooth LE through CoreBluetooth, as an application uses it: `cb_test` scans for a fitness band's service,
  finds the band, connects to it and lists its services.

- Bluetooth Classic: a pair of earbuds in pairing mode is found in Settings, pairs (without a code) and is
  listed as connected; the daemon connects its A2DP, AVRCP and hands-free profiles.

Not done: sound. Nothing is played through a paired device yet: the UART stays at 115200 baud, which is enough
for commands and Bluetooth LE and far too slow for audio, and the audio server's side of Bluetooth is not there
(the daemon finds nobody to hand an audio device to). The phone has no name of its own in Settings ("(null)"). A program that is not an application in the foreground is given what a scan finds only when it scans
for services, as on an iPhone: `cb_test 60 band FEE0`.

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

## Darwin's Bluetooth daemon

`bluetoothd` is written for the Broadcom chip of an iPhone. Four things stand between it and this controller:

- the address: the daemon reads it from a registry entry named `bluetooth` (`local-mac-address`), which
  `drivers/bluetooth/hakios.btnode.cpp` makes. The chip's settings hold an address that is the same in every
  phone, so the driver writes one made of the SoC's serial number into the chip first;
- the firmware: before anything else the daemon asks the service `com.apple.BlueTool` to run its "boot" and
  "init" scripts, which load Broadcom's firmware. `userland/src/bt_tool.c` answers for that service: done;
- the UART: on an iPhone it is a kernel control (`com.apple.uart.sk.bluetooth`) whose bytes go through a Skywalk
  channel. `userland/src/inject_bt.c`, a library inside the daemon, answers for the control and is the channel:
  two rings of slots in the daemon's own memory, behind which is `/dev/hakios-bt`;
- the commands: the same library reads what the daemon sends. Commands of Broadcom's own (group 0x3f) are not
  sent on but answered as done, with the length of answer the daemon insists on for each (found by letting the
  daemon fail and trying the next length: the library does that by itself and says what it found). A command the
  controller does not know is answered as done too, and "LE read periodic advertiser list size" is kept from the
  controller, which would otherwise refuse the scan commands the daemon uses afterwards.

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
| `bt_log=1` | the Bluetooth daemon's quieter messages on the console too (a great many) |
