#!/bin/bash
# Pull POCO X6 5G (garnet) vendor firmware out of Android into fw-*/ directories.
# The phone must be connected via adb with Android running.
set -e
cd "$(dirname "$0")/../.."
pull() {
	local dir=$1; shift
	mkdir -p "$dir"
	for f in "$@"; do adb pull "$f" "$dir/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: not on phone"; done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
}
adb get-state >/dev/null

# GPU: Adreno 710 (SM7435-AB)
pull fw-gpu /vendor/firmware/gen71100_gmu.bin /vendor/firmware/a702_gmu.bin /vendor/firmware/a702_sqe.fw /vendor/firmware/a702_zap.mdt /vendor/firmware/a702_zap.b00 /vendor/firmware/a702_zap.b01 /vendor/firmware/a702_zap.b02

# Wi-Fi: the WPSS DSP's image, and the radio's board data and regulatory database (adrastea/). The second needs root
# on most builds: what adb cannot read is copied out through su into /data/local/tmp first.
pull fw-wifi /vendor/firmware_mnt/image/wpss.mdt $(for i in $(seq -w 0 10); do echo /vendor/firmware_mnt/image/wpss.b$i; done)
if ! adb pull /vendor/firmware_mnt/image/adrastea fw-wifi/ >/dev/null 2>&1; then
	adb shell "su -c 'rm -rf /data/local/tmp/adrastea && cp -r /vendor/firmware_mnt/image/adrastea /data/local/tmp/ && chmod -R a+r /data/local/tmp/adrastea'" &&
		adb pull /data/local/tmp/adrastea fw-wifi/ >/dev/null && adb shell rm -rf /data/local/tmp/adrastea ||
		echo "pull-firmware.sh: /vendor/firmware_mnt/image/adrastea: not readable (needs root)"
fi
[ -d fw-wifi/adrastea ] && cp fw-wifi/adrastea/* fw-wifi/ && rm -rf fw-wifi/adrastea
# The board's data for this phone's region, as Xiaomi's driver picks it (ro.boot.hwc), as fw-wifi/board.bin.
# BOARD=bd_n16X.bin chooses another.
hwc=$(adb shell getprop ro.boot.hwc | tr -d '\r' | tr '[:lower:]' '[:upper:]')
case "${BOARD:-$hwc}" in
	bd_n16*.bin) board=$BOARD ;;
	IN|INDIA) board=bd_n16i.bin ;;
	CN|CHINA) board=bd_n16.bin ;;
	JP|JAPAN) board=bd_n16j.bin ;;
	*) board=bd_n16g.bin ;;
esac
if [ -f "fw-wifi/$board" ]; then cp "fw-wifi/$board" fw-wifi/board.bin && echo "pull-firmware.sh: fw-wifi/board.bin is $board"
else echo "pull-firmware.sh: fw-wifi/$board: not pulled, so no board.bin (BOARD= names another)"; fi
echo "pull-firmware.sh: fw-wifi: $(ls -1 fw-wifi | wc -l | tr -d ' ') files"

# Audio: the audio DSP's image (every segment the phone has), and the speaker amplifiers' configuration
pull fw-audio /vendor/firmware_mnt/image/adsp.mdt $(for i in $(seq -w 0 36); do adb shell "[ -f /vendor/firmware_mnt/image/adsp.b$i ]" 2>/dev/null && echo /vendor/firmware_mnt/image/adsp.b$i; done)
pull fw-audio /vendor/etc/aw882xx_acf.bin

# Haptic: AW8697 / Awinic
pull fw-haptic /vendor/firmware/aw8697_haptic.bin

# Bluetooth: the chip's patch and its settings, for every revision of the chip the phone has files for (the driver
# asks the chip which it is). Readable only as root: copied out through su into /data/local/tmp first.
stage=/data/local/tmp/hakios-bt
if adb shell "su -c 'rm -rf $stage && mkdir -p $stage && cd /vendor/bt_firmware/image && cp crbtfw*.tlv crnv*.bin apbtfw*.tlv apnv*.bin $stage/ && chmod -R a+rX $stage'" 2>/dev/null; then
	rm -rf fw-bt && mkdir -p fw-bt
	adb pull $stage/. fw-bt/ >/dev/null 2>&1 || echo "pull-firmware.sh: the Bluetooth firmware: not pulled"
	adb shell "su -c 'rm -rf $stage'" >/dev/null 2>&1 || true
	echo "pull-firmware.sh: fw-bt: $(find fw-bt -type f | wc -l | tr -d ' ') files"
else
	echo "pull-firmware.sh: /vendor/bt_firmware/image: not readable (needs root)"
fi

# Modem: its image (every segment the phone has; they are readable only as root on most builds), and of the firmware
# partition's image/ what the modem and the kernel ask for while it runs: the IPA's firmware, the service registry's
# *.jsn and the carrier configurations (modem_pr/). They are copied out through su into /data/local/tmp first.
stage=/data/local/tmp/hakios-modem
if adb shell "su -c 'rm -rf $stage && mkdir -p $stage/image && cd /vendor/firmware_mnt/image && cp modem.mdt modem.b[0-9][0-9] $stage/ && cp ipa_fws.* *.jsn $stage/image/ && cp -r modem_pr $stage/image/ && chmod -R a+rX $stage'" 2>/dev/null; then
	rm -rf fw-modem && mkdir -p fw-modem
	adb pull $stage/. fw-modem/ >/dev/null 2>&1 || echo "pull-firmware.sh: the modem's firmware: not pulled"
	adb shell "su -c 'rm -rf $stage'" >/dev/null 2>&1 || true
	echo "pull-firmware.sh: fw-modem: $(find fw-modem -type f | wc -l | tr -d ' ') files"
else
	echo "pull-firmware.sh: /vendor/firmware_mnt/image/modem.*: not readable (needs root)"
fi

echo "pull-firmware.sh: done"
