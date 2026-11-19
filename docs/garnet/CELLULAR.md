# Cellular on garnet

## Result

With `modem=1 ufs=2` on the kernel's line the modem is started beside the system: it goes online, finds the SIM
cards (both slots) and registers with the network (LTE). Then:

- the status bar shows the signal's bars, and the operator's name where the bar has room for it;
- mobile data: a data session is started, its interface (`en1`) is configured by itself, and the system uses it
  (DNS, HTTP and HTTPS were fetched over it with Wi-Fi off);
- the Phone icon: while the modem is up, a tap on it asks for a number or a USSD code (SpringBoard asks, in the
  Phone application's place);
- USSD: a code is sent and the network's answer shown, in whatever script it comes, with a field when the network
  waits for an answer;
- calls: a call can be made and ended, and one that comes in answered or declined, from prompts on the screen, but
  **without sound**: the path between the modem and the speakers and microphones is not set up.

The modem is stopped and shut down when the session ends.

Not done: the sound of a call, SMS, 5G (not tried), IPv6, choosing between the two SIMs, the Phone application
itself and the Settings pages (there is no baseband of Apple's for them to talk to: calls and USSD go through the
prompts, the program `cell_test`, or the sysctls below). A call that comes in has not been tried.

It is off unless asked for, for two reasons: it needs the phone's storage read through the loader (`ufs=2`), and
after a session with the modem the phone has to be restarted before the system is started again (below; the loader
sees to it).

## The hardware

| | |
|---|---|
| Modem | the MPSS of the SM7435 (`modem.mdt` + `modem.bNN`, PAS id 4), memory 0x8bc00000, 230 MB |
| Its links | SMP2P and GLINK in the shared memory (0x80900000), doorbells through the IPCC (client 2), QRTR on the channel `IPCRTR` |
| Its rails | `cx.lvl` and `mss.lvl` (the device tree names the second "mx"), and the clock `xo.lvl` |
| Its file systems | the partitions `modemst1`, `modemst2`, `fsg`, `fsc`, read into memory through the loader; what the modem writes stays in memory |
| Packet engine | IPA 5.2 with GSI 3.0 (registers 0x3f40000, GSI 0x3e04000, local memory 0x3f50000), firmware `ipa_fws.*`, SMMU streams 0x4a0 and 0x4a2 |

## How it is built

`drivers/modem/hakios.mpss.c` is the driver of the first phone this system ran on, set for this one. It loads the
image through TrustZone, starts it, brings up SMP2P, GLINK and QRTR, and serves what the modem asks of the
application processor: its file systems (RMTFS), the files of the firmware partition (TFTP) and the service registry.
Then it speaks QMI: the device management service puts the modem online, the SIM service reports the cards, the
network access service the registration and the signal (`drivers/modem/hakios.nas.c`). What it learns is in
`sysctl kern.hakios.cellular_*` (`_up`, `_registered`, `_bars`, `_dbm`, `_radio`, `_operator`), which the status bar
reads (`userland/src/inject_sb.c`).

Data goes through the IPA, the SoC's packet engine (`drivers/modem/hakios.ipa.c`, after Linux's `drivers/net/ipa`
with its configuration for IPA 5.2). Before the modem starts, the kernel loads the IPA's firmware through TrustZone,
gives the IPA's two SMMU streams a translation context of its own, lays out the IPA's local memory, programs what
Linux programs, and sets up this processor's four channels and endpoints (commands 9, the default route 11, to the
modem 2, from the modem 18). Once the modem is up its IPA driver is told that layout over QMI; once it is registered
a data session is started (DPM, WDA, WDS) and its packets appear as the interface `en1`, behind a made-up router
that answers ARP and DHCP in the kernel with the session's settings (`drivers/modem/hakios.cell.c`). The job
`com.hakios.cell` (`cell_test data`) has configd configure `en1` when the session is there.

Calls and USSD are `drivers/modem/hakios.voice.c`, a client of the modem's voice service, driven through sysctls:

| | |
|---|---|
| `kern.hakios.cellular_dial` | write a number: a call is made |
| `kern.hakios.cellular_answer`, `_hangup` | write a call's id, or 0 for the one that rings / for every call |
| `kern.hakios.cellular_calls`, `_call_state` | the calls there are; the first one's state |
| `kern.hakios.cellular_ussd` | write a code, or the answer the network waits for |
| `kern.hakios.cellular_ussd_state`, `_ussd_reply` | where the USSD session is; what the network said |
| `kern.hakios.cellular_ussd_cancel` | write 1: the USSD session is ended |

`userland/src/cell_test.c` is a small program over them (`status`, `data`, `ussd`, `dial`, `pickup`, `hangup`,
`calls`), and SpringBoard's library (`userland/src/inject_sb.c`) puts prompts on the screen for them: the Phone icon's
tap, a call coming in or under way, a USSD answer. The sysctls that are written may be written by any user, since
SpringBoard is not root; an application in its sandbox may write none.

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
  when a data session is started before it has been told anything. Told the 5.2 layout as Linux tells it (eleven
  routes of nineteen for the modem), it takes it.
- This phone's kernel has most of its memory above 32 GB: the IPA's translation context has to show it that too,
  or its first command is refused by the SMMU.
- configd makes no network service of its own for `en1`: its IPConfiguration plugin has to be asked.
- The network answers a USSD code in UCS-2, the higher byte first, when it does not write in Latin letters; for
  USSD and for calls the phone leaves LTE for GSM and comes back (there is no voice over LTE here).
- A signal report on LTE carries a 5G field too, with 0x8000 for "no reading", after the LTE one.
- The status bar's setters for the bars and the operator's name take the value alone.
- The modem's rail is not turned off when it is given back, and CX is left where the start put it.

## Restart after a session

After a session in which the modem ran, a second start of the system in the same session of the loader hangs about
ten seconds in (after the memory bandwidth vote, before the processors' frequency is raised); the watchdog then
restarts the phone, and this phone's firmware answers that with its crash-dump mode (a black screen; holding the
power button leaves it). Why is not known.

So it is not let happen: a kernel that has started the modem says so when it returns (`loader_return.c`), the loader
then starts no other kernel (`oem run` is refused, `getvar restart-first` says yes), and `xl` and `xl.ps1` say so
instead of sending one. `xl reboot` restarts the phone; boot the loader again after that.

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
| `modem_data_off=1` | no data session |
| `modem_apn=NAME` | the session to that access point instead of the SIM's default profile |
| `modem_ipa_fw=0` | leave the IPA alone (no data) |
| `modem_ipa_data_trace=1` | a log line for each packet |
| `modem_step=N` | bring-up only: do steps 1 to N and stop (11: online, the SIM cards, the registration) |
| `modem_online=0` | do not put the modem online |
| `modem_no_ipa_qmi=1`, `modem_ipa=N`, `modem_ipa_init_android=1` | bring-up only: nothing said to the modem's IPA driver; the SMP2P entry `ipa`; the vendor's extra fields in what is said |
| `modem_whole_memory=1` | tell TrustZone the whole of the modem's memory (the modem then does not come up) |
| `modem_xo_off=1` | do not ask for `xo.lvl` |
