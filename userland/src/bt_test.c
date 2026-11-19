/*
 * bt_test: the Bluetooth controller through /dev/hakios-bt (drivers/bluetooth/hakios.btqca.c), HCI in H4
 * packets: its version, then Bluetooth LE advertisements counted for some seconds.
 *
 *   bt_test [SECONDS]      (5)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int device;
static uint8_t have[4096];
static size_t have_count;

/* One whole packet, waited for; its length with the H4 byte, 0 when none comes. */
static size_t
packet(uint8_t *to, size_t room, int wait_ms)
{
	for (;;) {
		struct pollfd waiting = { .fd = device, .events = POLLIN };
		size_t header = have_count == 0 ? 0 : have[0] == 4 ? 2 : have[0] == 2 ? 4 : 3, whole = 0;

		if (have_count >= 1 + header && header != 0) {
			whole = 1 + header + (have[0] == 2 ? (size_t)(have[3] | have[4] << 8) : have[0] == 4 ? have[2] : have[3]);
		}
		if (whole != 0 && have_count >= whole) {
			size_t given = whole < room ? whole : room;

			memcpy(to, have, given);
			memmove(have, have + whole, have_count - whole);
			have_count -= whole;
			return given;
		}
		if (poll(&waiting, 1, wait_ms) <= 0) {
			return 0;
		}
		ssize_t got = read(device, have + have_count, sizeof(have) - have_count);

		if (got <= 0) {
			return 0;
		}
		have_count += (size_t)got;
	}
}

/* A command, and its "command complete" waited for; the event's length. */
static size_t
command(uint16_t opcode, const uint8_t *parameters, uint8_t count, uint8_t *event, size_t room)
{
	uint8_t out[4 + 255] = { 1, (uint8_t)opcode, (uint8_t)(opcode >> 8), count };

	if (count != 0) {
		memcpy(out + 4, parameters, count);
	}
	if (write(device, out, 4u + count) != (ssize_t)(4u + count)) {
		printf("bt_test: command 0x%04x: write: %s\n", opcode, strerror(errno));
		return 0;
	}
	for (int n = 0; n < 50; n++) {
		size_t length = packet(event, room, 3000);

		if (length == 0) {
			break;
		}
		if (length >= 7 && event[0] == 4 && event[1] == 0x0e && (event[4] | event[5] << 8) == opcode) {
			return length;
		}
	}
	printf("bt_test: command 0x%04x: no answer\n", opcode);
	return 0;
}

int
main(int argc, char **argv)
{
	static const uint8_t scan[7] = { 0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
	int seconds = argc > 1 ? atoi(argv[1]) : 5, strongest = -128;
	uint8_t event[300], on[2] = { 1, 0 };
	unsigned reports = 0;
	time_t until;
	size_t length;

	/* (the controller is a program's half a minute after the kernel's start: its patch goes over at 115200 baud) */
	for (int waited = 0; (device = open("/dev/hakios-bt", O_RDWR)) < 0 && waited < 90; waited++) {
		sleep(1);
	}
	if (device < 0) {
		printf("bt_test: /dev/hakios-bt: %s\n", strerror(errno));
		return 1;
	}
	length = command(0x1001, NULL, 0, event, sizeof(event));
	if (length >= 15) {
		printf("bt_test: HCI version %u, LMP %u, maker 0x%04x\n", event[7], event[10], event[11] | event[12] << 8);
	}
	if (command(0x200b, scan, sizeof(scan), event, sizeof(event)) == 0 || command(0x200c, on, sizeof(on), event, sizeof(event)) == 0) {
		return 1;
	}
	until = time(NULL) + seconds;
	while (time(NULL) < until) {
		length = packet(event, sizeof(event), 1000);
		if (length >= 5 && event[0] == 4 && event[1] == 0x3e && event[3] == 0x02) {
			int rssi = (int8_t)event[length - 1];

			reports++;
			if (rssi > strongest && rssi < 20) {
				strongest = rssi;
			}
		}
	}
	on[0] = 0;
	(void)command(0x200c, on, sizeof(on), event, sizeof(event));
	printf("bt_test: %u advertisements in %d s, the strongest at %d dBm\n", reports, seconds, strongest);
	close(device);
	return 0;
}
