# Cellular on garnet

## Result

With `modem=1 ufs=2` on the kernel's line the modem is started beside the system: it goes online, finds the SIM
cards (both slots), registers with the network (LTE) and reports the signal. The status bar shows the bars, and the
operator's name where the bar has room for it. The modem is stopped and shut down when the session ends.

Not done: mobile data, SMS, calls, 5G (not tried), choosing between the two SIMs, the Settings pages.

It is off unless asked for, for two reasons: it needs the phone's storage read through the loader (`ufs=2`), and
after a session with the modem the phone has to be restarted before the system is started again (below).

## The hardware

| | |
|---|---|
| Modem | the MPSS of the SM7435 (`modem.mdt` + `modem.bNN`, PAS id 4), memory 0x8bc00000, 230 MB |
| Its links | SMP2P and GLINK in the shared memory (0x80900000), doorbells through the IPCC (client 2), QRTR on the channel `IPCRTR` |
| Its rails | `cx.lvl` and `mss.lvl` (the device tree names the second "mx"), and the clock `xo.lvl` |
| Its file systems | the partitions `modemst1`, `modemst2`, `fsg`, `fsc`, read into memory through the loader; what the modem writes stays in memory |
| Packet engine | IPA 5.2 (0x3e00000), firmware `ipa_fws.*`; not driven yet |

## How it is built

`drivers/modem/hakios.mpss.c` is the driver of the first phone this system ran on, set for this one. It loads the
image through TrustZone, starts it, brings up SMP2P, GLINK and QRTR, and serves what the modem asks of the
application processor: its file systems (RMTFS), the files of the firmware partition (TFTP) and the service registry.
Then it speaks QMI: the device management service puts the modem online, the SIM service reports the cards, the
network access service the registration and the signal (`drivers/modem/hakios.nas.c`). What it learns is in
`sysctl kern.hakios.cellular_*` (`_up`, `_registered`, `_bars`, `_dbm`, `_radio`, `_operator`), which the status bar
reads (`userland/src/inject_sb.c`).

## Findings

- TrustZone has to be told the span of the image (215 MB), as Linux's loader does, not the whole of the modem's
  memory. Told the whole, the modem starts, says it has asked for its own power 22 ms later, and never says "up"
  nor reads a word of GLINK.
- The image's header has to be handed to TrustZone from memory below 4 GB (Linux: `dma_phys_below_32b`); the
  audio DSP's may be anywhere. The kernel has no memory of its own there: a page of the compute DSP's reserved
  memory is used, as for the modem's file buffer.
- The request to shut a modem down that has just stopped is refused once and taken the second time; Android's
  kernel meets the same and asks again.
- A request left unanswered for a hundred "interrupted" answers is not a failure: a shutdown takes many more.
- The modem's driver for the IPA gives up (the modem restarts itself) when it is told the IPA 4.11's layout, and
  when a data session is started before it has been told anything: neither is done here.
- A signal report on LTE carries a 5G field too, with 0x8000 for "no reading", after the LTE one.
- The status bar's setters for the bars and the operator's name take the value alone.
- The modem's rail is not turned off when it is given back, and CX is left where the start put it.

## Known problem: restart after a session

After a session in which the modem ran, a second start of the system in the same session of the loader hangs about
ten seconds in (after the memory bandwidth vote, before the processors' frequency is raised); the watchdog then
restarts the phone, and this phone's firmware answers that with its crash-dump mode (a black screen; holding the
power button leaves it). Why is not known. Restart the phone after a session with `modem=1`.

## Firmware

Nothing of the vendor's is in this repository. `devices/garnet/pull-firmware.sh` copies from the phone, with Android
running and root, the modem's image and, of the firmware partition's `image/`, the IPA's firmware, the service
registry's `*.jsn` and the carrier configurations into `fw-modem/`. `userland/build.sh` puts them into the system
image's `/fw` when they are there (110 MB: too much to go with the kernel). Without them the modem is not started.

## Boot arguments

| | |
|---|---|
| `modem=1` | bring the modem up beside the system (needs `ufs=2`) |
| `ufs=2` | the phone's storage, read through the loader (nothing is written) |
| `modem_step=N` | bring-up only: do steps 1 to N and stop (11: online, the SIM cards, the registration) |
| `modem_online=0` | do not put the modem online |
| `modem_data=1` | start a data session (not for this phone yet: its IPA is not driven) |
| `modem_ipa_qmi=1`, `modem_ipa_fw=1` | speak to the modem's IPA driver, load the IPA's firmware (bring-up only) |
| `modem_whole_memory=1` | tell TrustZone the whole of the modem's memory (the modem then does not come up) |
| `modem_xo_off=1` | do not ask for `xo.lvl` |
