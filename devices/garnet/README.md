# POCO X6 5G (garnet) and Snapdragon 7s Gen 2 (SM7435-AB)

HakiOS device repository for the **Xiaomi POCO X6 5G** (codename: `garnet`), powered by the Qualcomm Snapdragon 7s Gen 2 (`SM7435-AB`, "parrot" family) and Adreno 710 GPU.

The device folder of ios-garnet, kept in the tree (`devices/manifest.conf`: `path devices/garnet`). Its binary parts (UEFI image, device tree, splash, root image) are not in the repository: `tools/garnet-prebuilt.sh` fetches them.

---

## 1. Hardware Architecture

| Subsystem | Specification on POCO X6 5G (Garnet) |
|---|---|
| **SoC** | Qualcomm Snapdragon 7s Gen 2 (`SM7435-AB`, 4x Kryo Gold Cortex-A78 @ 2.40 GHz + 4x Kryo Silver Cortex-A55 @ 1.95 GHz) |
| **GPU** | Qualcomm Adreno 710 (Gen 7_3_0, chip ID `0x07010000`, 8 DCVS clock levels from 345 MHz to 940 MHz) |
| **Touchscreen** | FocalTech FT3683G on QUP1 SE3 SPI (`0x00a8c000`), Protocol Version 2 (PV2), TianMa PRAM firmware |
| **Display Panel** | 1.5K Flow AMOLED (1220 x 2712 @ 120 Hz, 68B colors, Dolby Vision) |
| **PMIC & Rails** | PM6450 / PM7250B (SPMI Arbiter v7 `0x0c400000`, `touch_vddio` 1.8V on GPIO3, RPMh RSC DRV2 `ldoe9` 3.224V) |
| **UEFI Platform** | Project Mu / Mu-Silicium (`garnetPkg`, `SILICIUM_UEFI.fd`) |
| **Memory Window** | 8 GB: 6 GB window from `0x800000000` (the hypervisor's 256 MB at `0x830000000` and the firmware carveouts notched); 12 GB: 5 GB from `0x840000000`. `0xA1000000` loader image base, `0xF4000000` payload base |

---

## 2. Hardware Implementation Status

| Subsystem | Status | Technical Details |
|---|---|---|
| **Display** | **Works** | UEFI GOP framebuffer passed to `MiniIOMobileFramebuffer`. Mesa GPU rendering active. |
| **Touchscreen** | **Works** | FocalTech FT3683G SPI controller on SE3 (`0x00a8c000`), PV2 packet parsing, PRAM firmware upload, SPMI power rails. Multi-touch and system gestures functional. |
| **Buttons** | **Works** | Hardware Volume Up/Down and Power keys through PM6450 SPMI Arbiter v7 (`0x0c400000`). |
| **CPU / SMP** | **Works** | Full 8-core multicore bringup (`cpus=8`, `bootcpu=7`, `cpufreq=1`). CPACR_EL1 FP/SIMD active on secondary cores. |
| **GPU** | **Works** | Adreno 710 at 345-940 MHz (the kernel programs `gpu_cc_pll0`). The window server draws through Metal with GMEM tiles (`gpu=1 metal=1 metal_gmem=1`); OpenGL ES through Mesa freedreno. Known: coloured blocks at a tile seam during fast swipes. |
| **Storage (UFS)** | **Works** | UFS reading through UEFI loader; mutable `/private/var` storage with `var_store=1 ufs_write=1`. |
| **Audio** | **Works** (speakers) | The audio DSP (AudioReach/GPR) plays what iOS plays through both AW88261 amplifiers; on by default with the phone's audio firmware. No microphones, headphones or calls. See `docs/garnet/AUDIO.md`. |
| **Battery** | **Partial** | Battery level read through PMIC; in-kernel charging not implemented (phone discharges during session). |
| **Wi-Fi** | **Works** | WCN3990-class radio on the WPSS DSP, Linux ath10k (snoc) in the kernel (`wifi=1`). Settings scans and joins 2.4 and 5 GHz WPA2 networks; DHCP and DNS. No WPA3. |
| **Cellular** | **Partial** | With `modem=1 ufs=2` the modem goes online, finds the SIM cards and registers on LTE; signal bars in the status bar; mobile data through the IPA 5.2 (`en1`); USSD; calls made and answered but silent. No SMS; restart the phone after such a session. See `docs/garnet/CELLULAR.md`. |

---

## 3. SoC Address Arguments (`soc.args`)

The kernel reads non-default SoC addresses from `soc.args`:

| Argument | Device-Tree Source | Garnet Value |
|---|---|---|
| `soc_rsc` | `qcom,rpmh-rsc` reg drv-2 | `0x17a20000` |
| `soc_tlmm` | `tlmm` physical GPIO base | `0x0f100000` |
| `soc_spmi` | SPMI PMIC Arbiter v7 core base | `0x0c400000` |
| `soc_cmd_db` | `reserved-memory` `cmd_db` | `0x80860000` |
| `soc_smem` | `reserved-memory` `qcom,smem` | `0x80900000` |
| `soc_imem` | `qcom,msm-imem` | `0x14600000` |

---

## 4. Booting the Device

Run the interactive boot manager from the root of the repository:
```bash
./garnet-boot.sh
```

**Workflow:**
1. Put the POCO X6 5G in fastboot mode (`adb reboot bootloader` or Hold Power + Volume Down).
2. Boot the UEFI image:
   ```bash
   fastboot boot prebuilt/garnet-boot.img
   ```
3. Once the HakiOS boot screen appears, select:
   - **Option 1:** Push Ramdisk (`userland/system.apfs`).
   - **Option 2:** Load HakiOS (Release mode).
   - **Option 3:** Load HakiOS with live debug logging.

---

## 5. Firmware Extraction

To extract vendor firmware binaries from your phone running Android:
```bash
./pull-firmware.sh
```
Pulls the Adreno 710 GMU/SQE/ZAP firmware (`fw-gpu/`), the Wi-Fi DSP image, board data and regulatory database (`fw-wifi/`; the board data needs root on Android), audio DSP firmware and the speaker amplifiers' configuration (`fw-audio/`) and haptics firmware (`fw-haptic/`). All vendor binaries stay out of Git.

---

## Maintainer and credits

Maintainer: Rafsan ([@rafsanbasunia](https://github.com/rafsanbasunia)).

<!--
Credits:
  Montfort (github.com/JosephMontfort) and Jotan (github.com/sudo-joaopuser) -- the POCO X6 5G port of HakiOS
  (hakios-sm7435, hakios-xnu-kernel-garnet): this device folder, the UEFI platform, touch, buttons and the first
  Adreno 710 bring-up.
  The HakiOS developers (github.com/oosexgang) -- the XNU port, loader, drivers and tools.
-->
Built on the POCO X6 5G port of HakiOS by Montfort and [Jotan](https://github.com/sudo-joaopuser), on
[HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), Apple's open-source XNU (`xnu-8796.121.2`), Project Mu /
Mu-Silicium UEFI, and Mesa freedreno.
