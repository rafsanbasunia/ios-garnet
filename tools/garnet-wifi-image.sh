#!/bin/bash
# The garnet boot image with Wi-Fi: devices/garnet's root.apfs plus what the radio needs.
#
#   tools/garnet-wifi-image.sh FIRMWARE-DIR [OUT.apfs] [JOBS-FILE]
#
# FIRMWARE-DIR   the phone's own Wi-Fi firmware, copied from the phone (it is the vendor's and is in no repository):
#                /vendor/firmware_mnt/image/wpss.mdt and wpss.b??, and from /vendor/firmware_mnt/image/adrastea
#                board.bin (the phone's own region's bd_n16*.bin, as devices/garnet/pull-firmware.sh picks it),
#                bdwlan.* and regdb.bin. The kernel loads wpss.* into the Wi-Fi DSP (drivers/wifi/hakios.wifi.c) and
#                hands board.bin to the driver as its board's data.
# OUT.apfs       default out/root/root-wifi.apfs
# JOBS-FILE      /etc/rc.jobs of the image, for a run with no finger on the phone (devices/garnet/rc.jobs.wifi)
#
# Environment:
#   DARWIN_ROOTFS   the unpacked Darwin root filesystem (sysimage/rootfs: Apple's, in no repository), from which the
#                   launchd jobs of the daemons the network needs are taken. Without them the radio comes up and
#                   nothing uses it: Settings' Wi-Fi page waits for configd for ever.
#   NETJOBS         those jobs (default: configd, mDNSResponder, securityd, trustd, nsurlsessiond)
#   MEDIAJOBS       the audio server's jobs (default: mediaserverd, nanoregistryd). mediaserverd is what the volume
#                   keys, the volume display and Control Center's volume slider talk to; without it the keys reach
#                   the system and nothing answers them. nanoregistryd has to come with it: the audio server asks the
#                   paired-watch registry for its preferences while it starts and waits for the answer for ever, and
#                   SpringBoard, which waits for the audio server, never gets past the Apple logo.
#
# Start the kernel with wifi=1. The driver's object is made by tools/mk-ath10k.sh and tools/mk-wlfw-cnss.sh before
# the kernel is built (xnu-build.sh links xnu-deps/build/ath10k/out/ath10k.o when it is there).
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
F=${1:?usage: tools/garnet-wifi-image.sh FIRMWARE-DIR [OUT.apfs] [JOBS-FILE]}
OUT=${2:-out/root/root-wifi.apfs}
JOBS=${3:-}
DARWIN_ROOTFS=${DARWIN_ROOTFS:-sysimage/rootfs}
NETJOBS=${NETJOBS-"com.apple.configd com.apple.mDNSResponder com.apple.securityd com.apple.trustd com.apple.nsurlsessiond"}
MEDIAJOBS=${MEDIAJOBS-"com.apple.mediaserverd com.apple.nanoregistryd"}
[ -f "$F/wpss.mdt" ] || { echo "garnet-wifi-image.sh: no wpss.mdt in $F" >&2; exit 1; }
[ -f "$F/board.bin" ] || echo "garnet-wifi-image.sh: no board.bin in $F (devices/garnet/pull-firmware.sh makes it): the driver will have no board data" >&2
args=()
for f in "$F"/wpss.mdt "$F"/wpss.b??; do args+=(--file "$f=/fw/$(basename "$f")"); done
for f in "$F"/board.bin "$F"/bdwlan.??? "$F"/regdb.bin; do [ -f "$f" ] && args+=(--file "$f=/fw/$(basename "$f")"); done
rm -rf out/wifi-jobs; mkdir -p out/wifi-jobs out/root
if [ -n "$NETJOBS$MEDIAJOBS" ]; then
	[ -d "$DARWIN_ROOTFS/System/Library/LaunchDaemons" ] || { echo "garnet-wifi-image.sh: no Darwin root filesystem at $DARWIN_ROOTFS (DARWIN_ROOTFS)" >&2; exit 1; }
	python3 userland/mkjobs.py "$DARWIN_ROOTFS" out/wifi-jobs $NETJOBS $MEDIAJOBS
fi
for f in out/wifi-jobs/*.plist; do [ -f "$f" ] && args+=(--file "$f=/LaunchDaemons/$(basename "$f")"); done
[ -n "$JOBS" ] && args+=(--file "$JOBS=/etc/rc.jobs")
# the window server's compiled shader stages from an earlier run (tools/garnet-metal-cache.sh), and the tool that
# hands a run's to the host, when they have been made
ls userland/hakios-metal-cache/*.stage >/dev/null 2>&1 && args+=(--metal-cache userland/hakios-metal-cache)
[ -f out/stage_put ] && args+=(--file out/stage_put=/bin/stage_put)
# ... and the thread-stack tool, when it has been built (userland/src/stacks.c)
[ -f out/stacks ] && args+=(--file out/stacks=/bin/stacks)
bash tools/garnet-root-apfs.sh devices/garnet/userland/root.apfs "$OUT" --plugin out/metal/RedwoodMetal \
	--plugin-plist userland/src/RedwoodMetal-garnet.plist --osmesa xnu-deps/build/mesa/out/libOSMesa.8.dylib \
	--file out/metal/sleepd_stub=/bin/sleepd_stub --file userland/src/com.redwood.svcstub.plist=/LaunchDaemons/com.redwood.svcstub.plist \
	"${args[@]}" --manifest "$OUT.manifest" > out/root-wifi.log 2>&1 || { tail -20 out/root-wifi.log; exit 1; }
echo "garnet-wifi-image.sh: $OUT: $(grep -c '^/fw/' "$OUT.manifest") files of firmware, $(ls out/wifi-jobs | wc -l) jobs of Darwin's (network and audio)"
