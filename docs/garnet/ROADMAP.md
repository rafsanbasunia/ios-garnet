# Roadmap

Development happens on the `dev` branch; `main` holds what works.

## iOS 16.5 port

- Fix the GMEM tile-seam glitch.
- Faster Control Center (two full-screen passes).
- 120 Hz display, GPU power management.
- Wi-Fi: WPA3, active scanning on 5 GHz.
- Cellular: mobile data (the IPA 5.2), SMS, calls; the restart needed after a session with the modem.
- Microphones, Bluetooth, camera.

## iOS 26

iOS 26 is the target after the 16.5 port. What is known:

- iOS 17 and later ship only arm64e code. About 38 of every 1000 instructions in the iOS 26 system are pointer
  authentication instructions the SM7435's ARMv8.2 cores cannot execute; almost all have a same-size ARMv8.2 replacement,
  so the system can be rewritten offline, with a small kernel emulator for the rest.
- The kernel moves to xnu-12377.121.6, Apple's published source for iOS 26.
- The iOS 26 system (about 12 GB plus a 4.5 GB library cache) does not fit in RAM: it needs a storage solution.

Stages:

1. Build xnu-12377 with this port's toolchain.
2. Forward-port the kernel changes; boot the 16.5 system on it in QEMU.
3. Rewriter for arm64e code; kernel support for arm64e binaries.
4. iOS 26 system image; home screen in QEMU.
5. iOS 26 on the phone.
6. GPU and Wi-Fi under iOS 26.
