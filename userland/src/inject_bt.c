/*
 * libhakios.bt.dylib: code of ours inside Darwin's Bluetooth daemon (see inject.c for how it gets there;
 * build.sh's INJECT names the program).
 *
 * bluetoothd is written for the Broadcom chip of an iPhone on a serial port (/dev/cu.bluetooth). This phone's
 * controller is another maker's, and the kernel hands it over as /dev/hakios-bt, HCI in H4 packets
 * (drivers/bluetooth/hakios.btqca.c). In between:
 *   - the daemon is told the controller's address (BT_DEVICE_ADDRESS, from sysctl kern.hakios.bt_address), which
 *     it otherwise looks for in an iPhone's device tree, and without which it ends;
 *   - when it connects to its UART (the kernel control "com.apple.uart.sk.bluetooth" of an iPhone, or the
 *     file /dev/cu.bluetooth) it is given one end of a pair of sockets, and what it does to the UART's settings
 *     is answered without being done;
 *   - what it writes is read packet by packet: HCI goes on to the controller, and a command of the chip maker's
 *     own (the group 0x3f), which this controller would not know, is answered here as done.
 */
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* (the kernel control's own structures: the SDK built against has no header for them) */
#ifndef PF_SYSTEM
#define PF_SYSTEM               32
#define AF_SYSTEM               PF_SYSTEM
#endif
#ifndef SYSPROTO_CONTROL
#define SYSPROTO_CONTROL        2
#endif
struct ctl_info {
	uint32_t ctl_id;
	char ctl_name[96];
};
struct sockaddr_ctl {
	uint8_t sc_len, sc_family;
	uint16_t ss_sysaddr;
	uint32_t sc_id, sc_unit, sc_reserved[5];
};
#define CTLIOCGINFO             _IOWR('N', 3, struct ctl_info)

#define INTERPOSE(replacement, replacee) \
	__attribute__((used)) static struct { const void *with, *what; } interpose_##replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&replacement, (const void *)(unsigned long)&replacee }

static void
say(const char *format, ...)
{
	static int console = -1;
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	if (length > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1);
	}
}

static int port = -1;           /* the daemon's end of the pair: its "serial port" */
static int ours = -1;           /* our end */
static int controller = -1;     /* /dev/hakios-bt */
static unsigned said;           /* lines said about packets: the first few hundred only */
static unsigned refused;        /* commands the controller did not know */

static void
all_out(int to, const uint8_t *bytes, size_t count)
{
	while (count != 0) {
		ssize_t done = write(to, bytes, count);

		if (done <= 0) {
			if (done < 0 && errno == EINTR) {
				continue;
			}
			return;
		}
		bytes += done;
		count -= (size_t)done;
	}
}

/*
 * How many bytes a command of the chip maker's returns after its status. The daemon was written for Broadcom's
 * chip and checks the length of each answer; nothing here says what the lengths are. So they are found: the last
 * such command answered is written down in a file, and when the daemon starts again without having sent anything
 * after it, that command's answer is taken to have been the wrong length and is made one byte longer for the next
 * try (the daemon is started again every ten seconds). What was found is said on the console, to be put into
 * the table below; the file is in /var/tmp and is gone with the session.
 */
#define LEARNED_FILE            "/var/tmp/hakios.bt.returns"
#define LEARNED_MOST            48

struct returned {
	uint16_t opcode;
	uint8_t first, more;            /* (first: the command's first byte, which for some says which command it is) */
};
static const struct returned known[] = {
	{ 0xfd48, 0x01, 1 }, { 0xfce2, 0x15, 2 }, { 0xfd1b, 0x51, 2 }, { 0xfce2, 0x36, 2 },
};
static struct {
	uint32_t count;
	struct returned entry[64];
	struct returned last;
	uint32_t last_open;             /* nothing has been sent since the last one was answered */
} learned;

static void
learned_save(void)
{
	int file = open(LEARNED_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (file >= 0) {
		(void)write(file, &learned, sizeof(learned));
		close(file);
	}
}

static struct returned *
learned_find(uint16_t opcode, uint8_t first)
{
	for (uint32_t i = 0; i < learned.count; i++) {
		if (learned.entry[i].opcode == opcode && learned.entry[i].first == first) {
			return &learned.entry[i];
		}
	}
	return NULL;
}

static void
learned_load(void)
{
	int file = open(LEARNED_FILE, O_RDONLY);

	if (file >= 0) {
		if (read(file, &learned, sizeof(learned)) != (ssize_t)sizeof(learned) || learned.count > 64) {
			memset(&learned, 0, sizeof(learned));
		}
		close(file);
	}
	if (learned.last_open) {
		struct returned *entry = learned_find(learned.last.opcode, learned.last.first);

		if (entry == NULL && learned.count < 64) {
			entry = &learned.entry[learned.count++];
			*entry = learned.last;
		}
		if (entry != NULL) {
			entry->more = entry->more >= LEARNED_MOST ? 0 : (uint8_t)(entry->more + 1);
			say("hakios: bt: learning: the daemon ended after command 0x%04x (first byte 0x%02x) was answered with %u bytes "
			    "after its status: now %u\n", entry->opcode, entry->first, learned.last.more, entry->more);
		}
		learned.last_open = 0;
		learned_save();
	}
	for (uint32_t i = 0; i < learned.count; i++) {
		say("hakios: bt: learned so far: { 0x%04x, 0x%02x, %u }\n", learned.entry[i].opcode, learned.entry[i].first,
		    learned.entry[i].more);
	}
}

static uint8_t
returns_of(uint16_t opcode, uint8_t first)
{
	struct returned *entry = learned_find(opcode, first);
	uint8_t more = 0;

	if (entry != NULL) {
		more = entry->more;
	} else {
		for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
			if (known[k].opcode == opcode && known[k].first == first) {
				more = known[k].more;
			}
		}
	}
	learned.last.opcode = opcode;
	learned.last.first = first;
	learned.last.more = more;
	learned.last_open = 1;
	learned_save();
	return more;
}

/* The daemon has sent something more: the last answer was taken. */
static void
survived(void)
{
	if (learned.last_open) {
		learned.last_open = 0;
		learned_save();
	}
}

/* The controller's packets to the daemon. */
static void *
up(__unused void *nothing)
{
	uint8_t bytes[2048];

	for (;;) {
		ssize_t got = read(controller, bytes, sizeof(bytes));

		if (got <= 0) {
			if (got < 0 && errno == EINTR) {
				continue;
			}
			say("hakios: bt: the controller's device ended (%d)\n", errno);
			return NULL;
		}
		/* (what is read is whole packets, one or more) */
		for (ssize_t at = 0; at < got;) {
			uint8_t *packet = bytes + at;
			ssize_t left = got - at, whole = left;

			if (packet[0] == 4 && left >= 3) {
				whole = 3 + packet[2];
			} else if (packet[0] == 2 && left >= 5) {
				whole = 5 + (packet[3] | packet[4] << 8);
			} else if (packet[0] == 3 && left >= 4) {
				whole = 4 + packet[3];
			}
			if (whole > left) {
				whole = left;
			}
			if (packet[0] == 4 && whole >= 7 && packet[1] == 0x0e) {
				unsigned opcode = packet[4] | packet[5] << 8;

				if (opcode == 0x1002 && packet[6] == 0 && whole >= 7 + 64) {
					/*
					 * The commands the controller says it has. The daemon will not send one that is missing here, and
					 * gives up where an iPhone's chip has it: it is told that all are there, and the ones that are
					 * not are answered below.
					 */
					memset(packet + 7, 0xff, 64);
				} else if (packet[6] == 0x01) {
					refused++;
					say("hakios: bt: the controller does not know command 0x%04x: the daemon is told that it was done\n", opcode);
					packet[6] = 0;
				}
			}
			if (packet[0] == 4 && whole >= 4 && packet[1] == 0x3e) {
				static unsigned heard;

				if (heard++ < 6 || heard % 100 == 0) {
					say("hakios: bt: < Bluetooth LE event %u (kind 0x%02x, %d bytes)\n", heard, packet[3], (int)whole);
				}
			}
			if (said < 400 && packet[0] == 4 && whole >= 3 && packet[1] != 0x3e) {
				said++;
				say("hakios: bt: < event 0x%02x, %d bytes: %02x %02x %02x %02x\n", packet[1], (int)whole, whole > 3 ? packet[3] : 0,
				    whole > 4 ? packet[4] : 0, whole > 5 ? packet[5] : 0, whole > 6 ? packet[6] : 0);
			}
			at += whole;
		}
		all_out(ours, bytes, (size_t)got);
	}
}

/* The daemon's packets: to the controller, or answered here. */
static void *
down(__unused void *nothing)
{
	static uint8_t have[8192];
	size_t count = 0;

	for (;;) {
		ssize_t got = read(ours, have + count, sizeof(have) - count);

		if (got <= 0) {
			if (got < 0 && errno == EINTR) {
				continue;
			}
			say("hakios: bt: the daemon closed its serial port\n");
			return NULL;
		}
		count += (size_t)got;
		for (;;) {
			size_t header = count == 0 ? 0 : have[0] == 1 ? 3 : have[0] == 2 ? 4 : have[0] == 3 ? 3 : 0, whole;

			if (count == 0) {
				break;
			}
			if (header == 0) {
				/* none of HCI's: a byte of the Broadcom sleep protocol, or of a download */
				if (said < 400) {
					said++;
					say("hakios: bt: > a byte that begins no packet: 0x%02x (%zu more)\n", have[0], count - 1);
				}
				memmove(have, have + 1, --count);
				continue;
			}
			if (count < 1 + header) {
				break;
			}
			whole = 1 + header + (have[0] == 2 ? (size_t)(have[3] | have[4] << 8) : have[3]);
			if (whole > sizeof(have)) {
				count = 0;
				break;
			}
			if (count < whole) {
				break;
			}
			if (have[0] == 1) {
				unsigned opcode = have[1] | have[2] << 8;

				survived();
				if (opcode == 0x200b && whole >= 11) {
					say("hakios: bt: > scan settings: type %u, interval %u, window %u, own address %u, filter %u\n", have[4],
					    have[5] | have[6] << 8, have[7] | have[8] << 8, have[9], have[10]);
				}

				if (said < 400) {
					said++;
					say("hakios: bt: > command 0x%04x, %u bytes: %02x %02x %02x %02x\n", opcode, have[3], whole > 4 ? have[4] : 0,
					    whole > 5 ? have[5] : 0, whole > 6 ? have[6] : 0, whole > 7 ? have[7] : 0);
				}
				if (opcode == 0x204a) {
					/*
					 * "LE read periodic advertiser list size". It is one of the commands of extended advertising, and
					 * a controller that has been sent one of those refuses the older scan and advertising commands
					 * from then on, which are the ones this daemon goes on to use (Broadcom's chip lets both be
					 * mixed). So this one is not sent: the list is said to have no room.
					 */
					uint8_t done[8] = { 4, 0x0e, 5, 1, have[1], have[2], 0, 0 };

					all_out(ours, done, sizeof(done));
				} else if (opcode >> 10 == 0x3f) {
					/*
					 * "command complete": one more command may come, this command, done. Some of these commands
					 * return something more, of a length the daemon insists on: that many zeros follow (returns_of).
					 */
					uint8_t done[7 + 64] = { 4, 0x0e, 4, 1, have[1], have[2], 0 };
					uint8_t more = returns_of((uint16_t)opcode, have[3] != 0 ? have[4] : 0);

					done[2] = (uint8_t)(4 + more);
					all_out(ours, done, 7u + more);
				} else {
					all_out(controller, have, whole);
				}
			} else {
				all_out(controller, have, whole);
			}
			memmove(have, have + whole, count - whole);
			count -= whole;
		}
	}
}

/*
 * The serial port: one end of a pair of sockets, with the controller behind the other. Where the daemon has a
 * descriptor of its own already (the socket it made for the UART's kernel control), that descriptor becomes the end.
 */
static int
port_open(const char *path, int in_place_of, int kind)
{
	int pair[2];
	pthread_t thread;

	if (port >= 0) {
		say("hakios: bt: %s opened again: the same port\n", path);
		if (in_place_of >= 0) {
			return dup2(port, in_place_of) < 0 ? -1 : in_place_of;
		}
		return dup(port);
	}
	controller = open("/dev/hakios-bt", O_RDWR);
	if (controller < 0) {
		say("hakios: bt: %s asked for, and /dev/hakios-bt cannot be opened (%d)\n", path, errno);
		errno = ENOENT;
		return -1;
	}
	if (socketpair(AF_UNIX, kind, 0, pair) != 0) {
		close(controller);
		controller = -1;
		return -1;
	}
	if (in_place_of >= 0) {
		if (dup2(pair[0], in_place_of) < 0) {
			close(pair[0]);
			close(pair[1]);
			close(controller);
			controller = -1;
			return -1;
		}
		close(pair[0]);
		pair[0] = in_place_of;
	}
	port = pair[0];
	ours = pair[1];
	pthread_create(&thread, NULL, up, NULL);
	pthread_detach(thread);
	pthread_create(&thread, NULL, down, NULL);
	pthread_detach(thread);
	say("hakios: bt: %s is the controller of /dev/hakios-bt (descriptor %d, %s)\n", path, port,
	    kind == SOCK_STREAM ? "a stream" : "datagrams");
	return port;
}

static bool
is_port(int descriptor)
{
	return port >= 0 && descriptor == port;
}

static int
my_open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list list;

		va_start(list, flags);
		mode = (mode_t)va_arg(list, int);
		va_end(list);
	}
	if (path != NULL && (strcmp(path, "/dev/cu.bluetooth") == 0 || strcmp(path, "/dev/uart.bluetooth") == 0)) {
		return port_open(path, -1, SOCK_STREAM);
	}
	if (path != NULL && strcmp(path, "/dev/btwake") == 0) {
		return open("/dev/null", O_RDWR);
	}
	return open(path, flags, mode);
}
INTERPOSE(my_open, open);

static int
my_close(int descriptor)
{
	if (is_port(descriptor)) {
		say("hakios: bt: the daemon closes its serial port: kept\n");
		return 0;
	}
	return close(descriptor);
}
INTERPOSE(my_close, close);

static int
my_tcgetattr(int descriptor, struct termios *settings)
{
	if (is_port(descriptor)) {
		memset(settings, 0, sizeof(*settings));
		cfmakeraw(settings);
		cfsetspeed(settings, 115200);
		return 0;
	}
	return tcgetattr(descriptor, settings);
}
INTERPOSE(my_tcgetattr, tcgetattr);

static int
my_tcsetattr(int descriptor, int when, const struct termios *settings)
{
	if (is_port(descriptor)) {
		say("hakios: bt: the serial port's settings: speed %lu, flags 0x%lx (answered, not done)\n",
		    (unsigned long)cfgetospeed(settings), (unsigned long)settings->c_cflag);
		return 0;
	}
	return tcsetattr(descriptor, when, settings);
}
INTERPOSE(my_tcsetattr, tcsetattr);

static int
my_tcflush(int descriptor, int which)
{
	return is_port(descriptor) ? 0 : tcflush(descriptor, which);
}
INTERPOSE(my_tcflush, tcflush);

static int
my_tcdrain(int descriptor)
{
	return is_port(descriptor) ? 0 : tcdrain(descriptor);
}
INTERPOSE(my_tcdrain, tcdrain);

/*
 * On an iPhone the UART is not a file but a kernel control, "com.apple.uart.sk.bluetooth": the daemon makes a
 * socket of the system's own family, asks for the control's number by its name, and connects to it. The number
 * is answered here, and the connection is the pair of sockets.
 */
#define UART_CONTROL            "com.apple.uart.sk.bluetooth"
#define UART_CONTROL_ID         0x48414b42u

static int control_socket = -1, control_kind = SOCK_STREAM;
static const uint8_t uart_nexus[16] = { 'H', 'A', 'K', 'I', 'O', 'S', '-', 'B', 'T', '-', 'U', 'A', 'R', 'T', 0, 1 };

static int
my_socket(int domain, int kind, int protocol)
{
	int descriptor = socket(domain, kind, protocol);

	if (domain == PF_SYSTEM && protocol == SYSPROTO_CONTROL && descriptor >= 0) {
		control_socket = descriptor;
		control_kind = kind;
	}
	return descriptor;
}
INTERPOSE(my_socket, socket);

static int
my_connect(int descriptor, const struct sockaddr *to, socklen_t length)
{
	const struct sockaddr_ctl *control = (const struct sockaddr_ctl *)(const void *)to;

	if (to != NULL && length >= sizeof(*control) && to->sa_family == AF_SYSTEM && control->sc_id == UART_CONTROL_ID) {
		return port_open(UART_CONTROL, descriptor, descriptor == control_socket ? control_kind : SOCK_STREAM) < 0 ? -1 : 0;
	}
	return connect(descriptor, to, length);
}
INTERPOSE(my_connect, connect);

static int
my_setsockopt(int descriptor, int level, int option, const void *value, socklen_t length)
{
	if (is_port(descriptor)) {
		const uint8_t *bytes = value;

		say("hakios: bt: the UART is set: level %d, option %d, %u bytes: %02x %02x %02x %02x (answered, not done)\n", level, option,
		    (unsigned)length, length > 0 ? bytes[0] : 0, length > 1 ? bytes[1] : 0, length > 2 ? bytes[2] : 0, length > 3 ? bytes[3] : 0);
		return 0;
	}
	return setsockopt(descriptor, level, option, value, length);
}
INTERPOSE(my_setsockopt, setsockopt);

static int
my_getsockopt(int descriptor, int level, int option, void *value, socklen_t *length)
{
	if (is_port(descriptor) && level != SOL_SOCKET) {
		say("hakios: bt: the UART is asked: level %d, option 0x%x, %u bytes\n", level, (unsigned)option,
		    length != NULL ? (unsigned)*length : 0);
		if (value != NULL && length != NULL) {
			memset(value, 0, *length);
			if (*length == sizeof(uart_nexus)) {
				/* the UART's data path: the "nexus" a channel is then opened to (below) */
				memcpy(value, uart_nexus, sizeof(uart_nexus));
			}
		}
		return 0;
	}
	return getsockopt(descriptor, level, option, value, length);
}
INTERPOSE(my_getsockopt, getsockopt);

static int
my_ioctl(int descriptor, unsigned long request, ...)
{
	va_list list;
	void *argument;

	va_start(list, request);
	argument = va_arg(list, void *);
	va_end(list);
	if (request == CTLIOCGINFO && argument != NULL && strcmp(((struct ctl_info *)argument)->ctl_name, UART_CONTROL) == 0) {
		((struct ctl_info *)argument)->ctl_id = UART_CONTROL_ID;
		return 0;
	}
	if (is_port(descriptor)) {
		say("hakios: bt: ioctl 0x%lx on the serial port (answered, not done)\n", request);
		return 0;
	}
	return ioctl(descriptor, request, argument);
}
INTERPOSE(my_ioctl, ioctl);

/*
 * The UART's bytes. On an iPhone they do not go through the socket: the socket only says which "nexus" the UART
 * is, and the daemon opens a channel to it -- rings of slots in memory shared with the kernel, which it fills and
 * empties and asks the kernel to "sync" (xnu's bsd/skywalk; the functions are libsystem_kernel's os_channel_*).
 * There is no such nexus here. The channel the daemon opens to ours is made of these functions instead: two rings
 * in this process, whose sync is a read from and a write to the pair of sockets. Any other channel is the system's.
 */
typedef void *channel_t;
typedef void *channel_ring_t;
typedef void *channel_slot_t;
typedef void *channel_attr_t;
typedef struct {
	uint16_t sp_flags, sp_len;
	uint32_t sp_idx;
	uint64_t sp_ext_ptr, sp_buf_ptr, sp_mdata_ptr;
	uint32_t sp_pad[8];
} slot_prop_t;
extern channel_t os_channel_create(const uint8_t *uuid, uint16_t port_number);
extern channel_t os_channel_create_extended(const uint8_t *uuid, uint16_t port_number, int direction, uint32_t ring, channel_attr_t attr);
extern int os_channel_get_fd(channel_t channel);
extern int os_channel_read_attr(channel_t channel, channel_attr_t attr);
extern int os_channel_attr_get(channel_attr_t attr, int type, uint64_t *value);
extern void os_channel_attr_destroy(channel_attr_t attr);
extern int os_channel_sync(channel_t channel, int mode);
extern void os_channel_destroy(channel_t channel);
extern uint32_t os_channel_ring_id(channel_t channel, int type);
extern channel_ring_t os_channel_tx_ring(channel_t channel, uint32_t ring);
extern channel_ring_t os_channel_rx_ring(channel_t channel, uint32_t ring);
extern uint32_t os_channel_available_slot_count(channel_ring_t ring);
extern channel_slot_t os_channel_get_next_slot(channel_ring_t ring, channel_slot_t slot, slot_prop_t *prop);
extern int os_channel_advance_slot(channel_ring_t ring, channel_slot_t slot);
extern void os_channel_set_slot_properties(channel_ring_t ring, channel_slot_t slot, const slot_prop_t *prop);
extern uint64_t os_channel_slot_get_packet(channel_ring_t ring, channel_slot_t slot);

#define SLOTS                   32
#define SLOT_BYTES              2048
#define SYNC_TX                 0
#define SYNC_RX                 1

/* A ring: the slots from head up to tail are the daemon's to use, the rest are "the kernel's" (ours). */
struct ring {
	bool transmit;
	uint32_t head, tail, sent;      /* (sent: for the transmit ring, up to where its slots have gone out) */
	uint16_t length[SLOTS];
	uint8_t tag[SLOTS];             /* a slot's handle is the address of its byte here */
	uint8_t bytes[SLOTS][SLOT_BYTES];
};
static struct {
	bool open;
	struct ring out, in;
	channel_attr_t attr;            /* the attributes object last filled for it */
	unsigned syncs_out, syncs_in;
} uart;

static bool
is_uart_ring(channel_ring_t ring)
{
	return ring == (channel_ring_t)&uart.out || ring == (channel_ring_t)&uart.in;
}

/* What has come from the controller, into the slots of the receive ring that are free. */
static void
uart_fill(void)
{
	struct ring *ring = &uart.in;

	while ((ring->tail + 1) % SLOTS != ring->head) {
		ssize_t got = recv(port, ring->bytes[ring->tail], SLOT_BYTES, MSG_DONTWAIT);

		if (got <= 0) {
			break;
		}
		ring->length[ring->tail] = (uint16_t)got;
		ring->tail = (ring->tail + 1) % SLOTS;
	}
}

static channel_t
uart_channel_open(const uint8_t *uuid)
{
	if (uuid == NULL || memcmp(uuid, uart_nexus, sizeof(uart_nexus)) != 0 || port < 0) {
		return NULL;
	}
	memset(&uart, 0, sizeof(uart));
	uart.out.transmit = true;
	uart.out.tail = SLOTS - 1;
	uart.open = true;
	say("hakios: bt: the UART's channel is opened: %u slots of %u bytes each way, in this process\n", SLOTS, SLOT_BYTES);
	return (channel_t)&uart;
}

static channel_t
my_channel_create(const uint8_t *uuid, uint16_t port_number)
{
	channel_t channel = uart_channel_open(uuid);

	return channel != NULL ? channel : os_channel_create(uuid, port_number);
}
INTERPOSE(my_channel_create, os_channel_create);

static channel_t
my_channel_create_extended(const uint8_t *uuid, uint16_t port_number, int direction, uint32_t ring, channel_attr_t attr)
{
	channel_t channel = uart_channel_open(uuid);

	return channel != NULL ? channel : os_channel_create_extended(uuid, port_number, direction, ring, attr);
}
INTERPOSE(my_channel_create_extended, os_channel_create_extended);

static int
my_channel_get_fd(channel_t channel)
{
	return channel == (channel_t)&uart ? port : os_channel_get_fd(channel);
}
INTERPOSE(my_channel_get_fd, os_channel_get_fd);

static int
my_channel_read_attr(channel_t channel, channel_attr_t attr)
{
	if (channel == (channel_t)&uart) {
		uart.attr = attr;
		return 0;
	}
	return os_channel_read_attr(channel, attr);
}
INTERPOSE(my_channel_read_attr, os_channel_read_attr);

static int
my_channel_attr_get(channel_attr_t attr, int type, uint64_t *value)
{
	if (attr != NULL && attr == uart.attr && uart.open) {
		/* rings each way, slots to a ring each way, bytes to a slot; nothing else is said of it */
		static const uint64_t known[5] = { 1, 1, SLOTS, SLOTS, SLOT_BYTES };

		if (value != NULL) {
			*value = type >= 0 && type < 5 ? known[type] : 0;
		}
		return 0;
	}
	return os_channel_attr_get(attr, type, value);
}
INTERPOSE(my_channel_attr_get, os_channel_attr_get);

static void
my_channel_attr_destroy(channel_attr_t attr)
{
	if (attr == uart.attr) {
		uart.attr = NULL;
	}
	os_channel_attr_destroy(attr);
}
INTERPOSE(my_channel_attr_destroy, os_channel_attr_destroy);

static int
my_channel_sync(channel_t channel, int mode)
{
	if (channel != (channel_t)&uart) {
		return os_channel_sync(channel, mode);
	}
	if (mode == SYNC_TX) {
		struct ring *ring = &uart.out;

		uart.syncs_out++;
		while (ring->sent != ring->head) {
			all_out(port, ring->bytes[ring->sent], ring->length[ring->sent]);
			ring->sent = (ring->sent + 1) % SLOTS;
		}
		ring->tail = (ring->sent + SLOTS - 1) % SLOTS;
	} else if (mode == SYNC_RX) {
		uart.syncs_in++;
		uart_fill();
	}
	return 0;
}
INTERPOSE(my_channel_sync, os_channel_sync);

static void
my_channel_destroy(channel_t channel)
{
	if (channel == (channel_t)&uart) {
		say("hakios: bt: the UART's channel is closed (%u syncs out, %u in)\n", uart.syncs_out, uart.syncs_in);
		uart.open = false;
		return;
	}
	os_channel_destroy(channel);
}
INTERPOSE(my_channel_destroy, os_channel_destroy);

static uint32_t
my_channel_ring_id(channel_t channel, int type)
{
	return channel == (channel_t)&uart ? (uint32_t)(type >= 2 ? 1 : 0) : os_channel_ring_id(channel, type);
}
INTERPOSE(my_channel_ring_id, os_channel_ring_id);

static channel_ring_t
my_channel_tx_ring(channel_t channel, uint32_t ring)
{
	return channel == (channel_t)&uart ? (channel_ring_t)&uart.out : os_channel_tx_ring(channel, ring);
}
INTERPOSE(my_channel_tx_ring, os_channel_tx_ring);

static channel_ring_t
my_channel_rx_ring(channel_t channel, uint32_t ring)
{
	return channel == (channel_t)&uart ? (channel_ring_t)&uart.in : os_channel_rx_ring(channel, ring);
}
INTERPOSE(my_channel_rx_ring, os_channel_rx_ring);

static uint32_t
my_channel_available_slot_count(channel_ring_t ring)
{
	struct ring *ours_ring = ring;

	if (!is_uart_ring(ring)) {
		return os_channel_available_slot_count(ring);
	}
	if (!ours_ring->transmit && ours_ring->head == ours_ring->tail) {
		uart_fill();
	}
	return (ours_ring->tail + SLOTS - ours_ring->head) % SLOTS;
}
INTERPOSE(my_channel_available_slot_count, os_channel_available_slot_count);

static channel_slot_t
my_channel_get_next_slot(channel_ring_t ring, channel_slot_t slot, slot_prop_t *prop)
{
	struct ring *ours_ring = ring;
	uint32_t index;

	if (!is_uart_ring(ring)) {
		return os_channel_get_next_slot(ring, slot, prop);
	}
	if (slot == NULL) {
		if (!ours_ring->transmit && ours_ring->head == ours_ring->tail) {
			uart_fill();
		}
		index = ours_ring->head;
	} else {
		index = ((uint32_t)((uint8_t *)slot - ours_ring->tag) + 1) % SLOTS;
	}
	if (index == ours_ring->tail) {
		return NULL;
	}
	if (prop != NULL) {
		memset(prop, 0, sizeof(*prop));
		prop->sp_idx = index;
		prop->sp_len = ours_ring->transmit ? SLOT_BYTES : ours_ring->length[index];
		prop->sp_buf_ptr = (uint64_t)(uintptr_t)ours_ring->bytes[index];
	}
	return &ours_ring->tag[index];
}
INTERPOSE(my_channel_get_next_slot, os_channel_get_next_slot);

static int
my_channel_advance_slot(channel_ring_t ring, channel_slot_t slot)
{
	struct ring *ours_ring = ring;

	if (!is_uart_ring(ring)) {
		return os_channel_advance_slot(ring, slot);
	}
	ours_ring->head = ((uint32_t)((uint8_t *)slot - ours_ring->tag) + 1) % SLOTS;
	return 0;
}
INTERPOSE(my_channel_advance_slot, os_channel_advance_slot);

static void
my_channel_set_slot_properties(channel_ring_t ring, channel_slot_t slot, const slot_prop_t *prop)
{
	struct ring *ours_ring = ring;

	if (!is_uart_ring(ring)) {
		os_channel_set_slot_properties(ring, slot, prop);
		return;
	}
	ours_ring->length[(uint32_t)((uint8_t *)slot - ours_ring->tag) % SLOTS] = prop->sp_len > SLOT_BYTES ? SLOT_BYTES : prop->sp_len;
}
INTERPOSE(my_channel_set_slot_properties, os_channel_set_slot_properties);

static uint64_t
my_channel_slot_get_packet(channel_ring_t ring, channel_slot_t slot)
{
	if (is_uart_ring(ring)) {
		say("hakios: bt: a packet handle is asked of the UART's ring: there is none\n");
		return 0;
	}
	return os_channel_slot_get_packet(ring, slot);
}
INTERPOSE(my_channel_slot_get_packet, os_channel_slot_get_packet);

__attribute__((constructor))
static void
bt_start(void)
{
	char text[32] = "";
	size_t size = sizeof(text);
	int state = 0, waited = 0;

	learned_load();
	/* the controller is a program's half a minute after the kernel's start: the daemon waits for it here */
	for (;; waited++) {
		size = sizeof(state);
		if (sysctlbyname("kern.hakios.bt_up", &state, &size, NULL, 0) != 0 || state != 1 || waited >= 90) {
			break;
		}
		sleep(1);
	}
	size = sizeof(text) - 1;
	if (state == 2 && sysctlbyname("kern.hakios.bt_address", text, &size, NULL, 0) == 0 && text[0] != 0) {
		/* (its address is in the registry by now, on the entry "bluetooth": hakios.btnode.cpp) */
		say("hakios: bt: the controller is there after %d s\n", waited);
	} else {
		say("hakios: bt: no controller (state %d): the daemon is left as it is\n", state);
	}
}
