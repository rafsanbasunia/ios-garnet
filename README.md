# ios-garnet

**Apple's iOS 16.5 running on a POCO X6 5G.**

This project runs Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) and the iOS 16.5 userland on the Xiaomi
POCO X6 5G (`garnet`, Qualcomm Snapdragon 7s Gen 2 / SM7435-AB, Adreno 710). It is built on [HakiOS](#credits) and adds a
fully clocked, Metal-accelerated Adreno 710 and working Wi-Fi.

Status: November 2026.

> Experimental bring-up, not a usable phone OS. It boots from RAM over USB: **nothing is written to the phone**, and a
> reboot gives the phone back to Android. Settings and Wi-Fi passwords do not survive a reboot.

## What works

| Area | State |
|---|---|
| Boot | From RAM: `fastboot boot` of the loader, kernel and system image sent over USB |
| CPU | All 8 cores (4x Cortex-A78, 4x Cortex-A55), frequency scaling |
| Display | 1220x2712 panel through the UEFI framebuffer |
| GUI | SpringBoard, Settings, Safari and the stock apps |
| Graphics | CoreAnimation drawn through Metal on the Adreno 710 at 940 MHz; home-screen page swipes 62-68 fps |
| Touch | FocalTech FT3683G, multi-touch and system gestures |
| Buttons | Power, volume up, volume down (volume display, Control Center's volume slider) |
| Wi-Fi | 2.4 and 5 GHz (up to 80 MHz), WPA2; scan and join from Settings, DHCP, DNS, Safari |
| Battery | Level reading |
| Copy and paste | the pasteboard daemon (pasted); drag and drop, the share sheet |
| Files | the Files app with "On My iPhone" (kept in memory, like all of `/private/var`) |

## What does not work (yet)

- Cellular, Bluetooth, camera, sensors, GPS.
- Audio: the system's audio server runs (volume keys and slider work), but the speakers are silent: garnet's audio
  DSP speaks AudioReach (GPR), and that path is not written yet.
- Wi-Fi: no WPA3; channels 12-14 off; on 5 GHz a network shows only once its access point has been heard.
- Graphics: Control Center at about 35-40 fps; 120 Hz not reached.
- Memory: iOS gets about 6 GB of the 8 GB (the rest is below 4 GB, among the firmware's regions), and the system
  image takes 2.4 GB of it, so very heavy web pages can still run short.
- No charging while iOS runs.
- Nothing persists across reboots (RAM boot).
- Security is off: code signatures are not enforced. Apple services (activation, Apple ID, iCloud, App Store) are not
  supported.

## How it boots

```
stock bootloader ── fastboot boot garnet-boot.img (RAM only)
  └─ UEFI (Mu-Silicium, garnetPkg)
      └─ HakiOS loader (USB link to the PC)
          ├─ system image  ── uploaded once per session into RAM
          └─ XNU kernel    ── uploaded with the root image and boot arguments
              ├─ Wi-Fi: WPSS DSP started, ath10k driver brought up beside the boot
              └─ launchd ─ backboardd ─ SpringBoard
```

## How to do it yourself

You need:

- a POCO X6 5G with an **unlocked bootloader**;
- a Linux PC (Ubuntu 24.04 or WSL2) with about 40 GB free, and `adb`/`fastboot`;
- an iOS 16.5 IPSW for the iPhone X (iPhone10,3, build 20F66) that you obtain yourself;
- the phone's own Wi-Fi firmware, copied from the phone (below).

Nothing of Apple's and no vendor firmware is in this repository.

### 1. Get the sources

```sh
git clone https://github.com/rafsanbasunia/ios-garnet.git && cd ios-garnet
bash tools/garnet-prebuilt.sh                   # UEFI image, device tree, splash and root image (not in this repository)
devices/garnet/pull-firmware.sh                 # Android running, on adb: GPU, Wi-Fi, audio firmware into fw-*/
```

### 2. Build the kernel and the Wi-Fi driver

```sh
bash xnu/apply.sh                 # Apple's xnu-8796.121.2 plus this port's changes
bash xnu-deps/setup.sh            # the cross toolchain, once
bash tools/mk-ath10k.sh           # Linux ath10k + FreeBSD LinuxKPI -> xnu-deps/build/ath10k/out/ath10k.o
./xnu-build.sh -j"$(nproc)" -k    # links the Wi-Fi driver when ath10k.o exists
```

The kernel is `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.

### 3. Build the system images

The big system image (`userland/system.apfs`) is made from your IPSW as described in
[docs/getting-started/qemu.md](docs/getting-started/qemu.md#darwin-in-qemu). The Metal plug-in and Mesa are built as in
[.github/workflows/build-kernel.yml](.github/workflows/build-kernel.yml).

For Wi-Fi, build the small root image with the firmware that `pull-firmware.sh` put into `fw-wifi/` (the board data
needs root on Android):

```sh
DARWIN_ROOTFS=/path/to/unpacked/ios16.5/rootfs \
  bash tools/garnet-wifi-image.sh fw-wifi userland/root.apfs
```

Optional, once: a shader cache, so that no boot compiles CoreAnimation's shaders again (one run collects them and
hands them over through RAM; nothing is written to the phone's storage). See `tools/garnet-metal-cache.sh`.

### 4. Boot

Put the phone in fastboot (Power + Volume Down), then:

```sh
make garnet_defconfig               # common kernel + SM7435 + devices/garnet/configs/garnet.config -> .config
./xl enter                          # fastboot -> the loader, in RAM
./xl ramdisk userland/system.apfs   # the system image, once per loader session (about 80 s)
XL_SECS=600 XL_CMDLINE="$(python3 tools/hakios-config.py args)" ./xl run
./xl reboot                         # back to Android
```

On Windows use `xl.ps1` with the same commands. Release bundles also carry `garnet-boot.sh` / `garnet-boot.bat`, an
interactive version of these steps.

### 5. Read the kernel log

`./xl run` prints the log live and saves it under `logs/`. Wi-Fi lines start with `wifi:`, `lx:` and `ath10k`; GPU lines
with `adreno:` and `HakiosMetal`.

### Boot arguments

`devices/garnet/configs/garnet.config` sets these; `make config` shows what a configuration gives.

| Argument | What it does |
|---|---|
| `cpus=8 bootcpu=7 garnet_cpufreq=1` | all cores, frequency scaling |
| `garnet_touch=1 garnet_buttons=1` | touch screen and buttons |
| `gpu=1 gl=1 adreno_speed=8 ddr=2` | Adreno 710 at its top level (940 MHz), memory bus vote |
| `metal=1 metal_all=1 metal_gmem=1` | CoreAnimation through Metal, tile (GMEM) rendering |
| `wifi=1` | Wi-Fi brought up beside the boot |
| `wifi_mac=XX:XX:XX:XX:XX:XX` | optional: the radio's address (default: a stable one derived from the SoC) |

Every argument: [docs/boot-args.md](docs/boot-args.md).

## Repository layout

| Path | What |
|---|---|
| `xnu/` | the kernel patch over xnu-8796.121.2 and added kernel files; `apply.sh` |
| `drivers/` | the kernel's drivers: `gpu/` (Adreno 710), `wifi/` (radio bring-up, Apple80211 interface), `input/`, `power/`, ... |
| `wifi/lx/` | the layer that runs Linux Wi-Fi drivers inside XNU; `src10k/` is the WCN3990 (ath10k snoc) platform glue |
| `hakios-metal/` | Metal on the Adreno: command streams, render passes, shader translation |
| `userland/` | init, the Metal plug-in for CoreAnimation, injected shims, test programs |
| `xnuloader/` | the UEFI loader that receives kernel and images over USB |
| `devices/garnet/` | the phone: `device.conf` (what works), `configs/` (drivers and arguments), `soc/` (SM7435), `pull-firmware.sh`, UEFI platform |
| `tools/` | build and image tools (`mk-ath10k.sh`, `garnet-wifi-image.sh`, `garnet-prebuilt.sh`, `garnet-root-apfs.sh`, `apfs/`, ...) |
| `docs/garnet/` | this port's notes: [GPU](docs/garnet/GPU.md), [Wi-Fi](docs/garnet/WIFI.md), [roadmap](docs/garnet/ROADMAP.md) |
| `docs/boot-args.md` | every boot argument, generated from the source |

## Changes over HakiOS

| Area | Change |
|---|---|
| GPU clock | `gpu_cc_pll0` (Lucid EVO PLL) programmed by the kernel: the Adreno now runs 345-940 MHz instead of 295 MHz |
| GMEM rendering | `CP_SET_PSEUDO_REG` (draw/primitive stream addresses) set on A7xx: tile rendering no longer hangs |
| Metal | missing constant buffers zero-filled; per-pass statistics |
| Wi-Fi | WPSS DSP start, GLINK/QRTR/QMI, supply rails, SMMU stream 0x4c0; Linux ath10k (snoc) linked into the kernel |
| Wi-Fi glue | real interrupts (12 SPIs), receive poll thread, regulatory domain and transmit power, flow control |
| Wi-Fi identity | stable radio address derived from the SoC serial number when none is given |
| Images | `garnet-wifi-image.sh`: firmware plus the network daemons (configd, mDNSResponder, securityd, trustd, nsurlsessiond) and the audio server (mediaserverd, with nanoregistryd, without which it hangs the boot) |
| Debugging | `userland/src/stacks.c`: the threads of a running process and where each one waits (found the audio server's hang) |
| GPU memory | surface mappings no longer leak (browsing ran the phone out of memory); kept buffers capped for all clients; `mem_owners=1` |
| Memory | the kernel's window starts at 0x800000000: 6 GB instead of 5 on the 8 GB phone |
| Files, pasteboard | `garnet-wifi-image.sh` `FILEJOBS`: fileproviderd, filecoordinationd, pasted, druid, kbd, sharingd (copy and paste work; Files no longer dies at start) |
| On My iPhone | `select-system.py` keeps a framework plug-in's own program (the local storage provider was a bundle without one); the file provider daemon is told `/private/var` is APFS, since it keeps its library on APFS only and `/private/var` is ramfs (`var_apfs`, on by default) |

## Key hardware facts

- SoC: SM7435-AB, 4x Cortex-A78 + 4x Cortex-A55, ARMv8.2 (no pointer authentication).
- GPU: Adreno 710 (A7xx), GMU-controlled; core clock from `gpu_cc_pll0`, 8 levels up to 940 MHz.
- Wi-Fi: WCN3990-class radio ("adrastea") on the WPSS DSP; registers at 0x22800000, firmware memory 0x82a00000 (3 MB),
  SMMU stream 0x4c0, 12 level-triggered interrupts. Linux driver: ath10k (snoc). Board data: Xiaomi's `bd_n16*.bin`.
- Display: 1220x2712 AMOLED, UEFI framebuffer. Touch: FocalTech FT3683G on SPI.
- RAM: 8 GB in three banks; the kernel's window is 0x800000000-0x980000000 (6 GB, the hypervisor's 256 MB at
  0x830000000 left out), about 5.8 GB of it usable.

## Credits

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) — the XNU port, loader, drivers, tools and documentation this
  project is built on.
- [hakios-xnu-kernel-garnet](https://github.com/JosephMontfort/hakios-xnu-kernel-garnet) and
  [hakios-sm7435](https://github.com/JosephMontfort/hakios-sm7435) — the POCO X6 5G port (Montfort, Jotan).
- Apple — xnu, under the Apple Public Source License 2.0.
- Linux ath10k (BSD-3-Clause-Clear) and FreeBSD LinuxKPI (BSD-2-Clause).
- Mesa (freedreno), Project Mu / Mu-Silicium.

Maintainer: Rafsan ([@rafsanbasunia](https://github.com/rafsanbasunia)).

## License

The XNU patch and files derived from it are under the Apple Public Source License 2.0 ([LICENSE.APSL](LICENSE.APSL)); the
rest under MIT ([LICENSE](LICENSE)), as the file headers say. Third parties: [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

Not affiliated with or endorsed by Apple or Xiaomi.
