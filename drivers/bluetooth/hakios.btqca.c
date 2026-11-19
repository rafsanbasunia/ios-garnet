/*
 * Bluetooth of the POCO X6 5G (garnet): the Bluetooth half of its WCN3990-class radio chip, on a UART of the
 * SoC. Only with boot-arg bt=N (below).
 *
 * The phone's device tree (node bt_wcn3990, compatible qcom,wcn3990; alias hsuart0):
 *   the UART         serial engine 5 of QUP 1 at 0x00a94000, pins 14 (CTS), 15 (RTS), 16 (TX), 17 (RX), function 1
 *   its clocks       GCC: the engine's root at 0x28618, its branch at 0x28610, the votes in 0x62008
 *   the supplies     vdd-io   PM6450 L22   "ldob22"  1800 mV       vdd-core PM6450 L14   "ldob14"  1304 mV
 *                    vdd-pa   PM6150L L10  "ldoe10"  3312 mV       vdd-xtal PM6150L L1   "ldoe1"   1800 mV
 *                    (the last three are the Wi-Fi half's too, at the same voltages: they are never taken away here)
 * There is no reset or enable line: the chip is switched on and off with a byte on the UART.
 *
 * What is done, as Linux does it (drivers/bluetooth/hci_qca.c and btqca.c, drivers/tty/serial/qcom_geni_serial.c,
 * whose numbers these are):
 *   1. the supplies on, the UART's clocks and pins, the engine set up as a UART in FIFO mode, polled;
 *   2. the "power off" byte (0xc0) at 2400 baud, the "power on" byte (0xfc) at 115200, without flow control;
 *   3. the chip's version asked for (vendor command 0xfc00, 0x19);
 *   4. its patch and its settings sent (0xfc00, 0x1e, 243 bytes at a time): the files crbtfwNN.tlv and crnvNN.bin
 *      (apbtfwNN.tlv and apnvNN.bin for the 0x4009 chips) of the phone's /vendor/bt_firmware/image, which are
 *      NOT in the repository: they are read from fw/ of the image;
 *   5. HCI reset, and from there plain HCI in H4 packets.
 * The chip's sleep protocol is answered (it says 0xfd before it sends after a pause, and is told 0xfc). Once the
 * chip has said which it is, both sides go to 3.2 Mbaud (vendor command 0xfc48, and the same in the settings).
 *
 * Boot-args:
 *   bt=1           the engine looked at: its protocol, its FIFO, the pins (clocks are switched on; nothing else)
 *   bt=2           the chip switched on and asked for its version
 *   bt=3           its patch and settings sent, HCI reset, its address and version read
 *   bt=4           and a scan for Bluetooth LE advertisements for bt_scan seconds, counted
 * From bt=3 on the controller is then a program's: /dev/hakios-bt, HCI in H4 packets both ways (what is read is
 * whole packets, the chip's sleep bytes taken out and answered here).
 *   bt_delay=<s>   seconds after the kernel's start at which this begins (12: after the Wi-Fi half has its supplies)
 *   bt_scan=<s>    how long the scan of bt=4 lasts (5)
 *   bt_sample=<n>  the UART's samples per bit, 16 or 32 (16; the other is tried when the chip does not answer)
 *   bt_slow=1      the UART stays at 115200 baud
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern bool hakios_soc_serial(uint32_t *serial);
extern void hakios_bt_publish(const uint8_t *address);      /* (hakios.btnode.cpp) */

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;
static uint32_t level, sample = 16, scan_seconds = 5;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* ---- pins (TLMM) ----------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3
#define PIN_CTS                 14
#define PIN_RTS                 15
#define PIN_TX                  16
#define PIN_RX                  17
#define PIN_FUNCTION_UART       1       /* "qup1_se5" */

static void
pins_show(const char *when)
{
	for (unsigned pin = PIN_CTS; pin <= PIN_RX; pin++) {
		uint32_t cfg = rd(GPIO_CFG(pin));

		printf("bt: %s: gpio%u: function %u, pull %u, %u mA, reads %u\n", when, pin, cfg >> 2 & 15, cfg & 3,
		    ((cfg >> 6 & 7) + 1) * 2, rd(GPIO_IO(pin)) & 1);
	}
}

/* (the device tree's "active" state of the four) */
static void
pins_uart(void)
{
	wr(GPIO_CFG(PIN_CTS), PULL_DOWN | PIN_FUNCTION_UART << 2 | 0 << 6);
	wr(GPIO_CFG(PIN_RTS), PULL_DOWN | PIN_FUNCTION_UART << 2 | 0 << 6);
	wr(GPIO_CFG(PIN_TX), PULL_UP | PIN_FUNCTION_UART << 2 | 0 << 6);
	wr(GPIO_CFG(PIN_RX), PULL_NONE | PIN_FUNCTION_UART << 2 | 0 << 6);
}

/* ---- the supplies (RPMh) ---------------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return false;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

static const struct {
	const char *what, *name;
	uint32_t millivolts;
} supplies[] = {
	{ "vdd-io", "ldob22", 1800 }, { "vdd-xtal", "ldoe1", 1800 }, { "vdd-core", "ldob14", 1304 }, { "vdd-pa", "ldoe10", 3312 },
};

static bool
supplies_on(void)
{
	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		uint32_t base = rpmh_address(supplies[i].name);
		uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
		uint32_t data[3] = { supplies[i].millivolts, 1, VRM_MODE_LDO_HPM };

		if (base == 0 || (base >> 16 & 0xf) != 4) {
			printf("bt: the supply %s (\"%s\") is not a regulator of the command database (0x%x)\n", supplies[i].what,
			    supplies[i].name, base);
			return false;
		}
		if (!rpmh_write(address, data, 3)) {
			printf("bt: the supply %s (\"%s\") was not acknowledged\n", supplies[i].what, supplies[i].name);
			return false;
		}
		printf("bt: the supply %s (\"%s\") is on at %u mV\n", supplies[i].what, supplies[i].name, supplies[i].millivolts);
	}
	return true;
}

/* ---- clocks (GCC) ----------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x70000UL
#define GCC_SE_CMD_RCGR         (gcc + 0x28618)
#define GCC_SE_CFG_RCGR         (gcc + 0x2861c)
#define GCC_SE_M                (gcc + 0x28620)
#define GCC_SE_N                (gcc + 0x28624)
#define GCC_SE_D                (gcc + 0x28628)
#define GCC_SE_CBCR             (gcc + 0x28610)
#define GCC_VOTE                (gcc + 0x62008)         /* bit 20 the wrapper's master bus clock, 21 its slave's, 27 this engine */
#define CLOCK_OFF               (1u << 31)
#define SOURCE_GPLL0_EVEN       6                       /* 300 MHz */
#define ROOT_HZ                 7372800                 /* 300 MHz x 384 / 15625 */

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | 1u << 20 | 1u << 21);
	/* the root: half the PLL's even output through M/N = 384/15625 */
	wr(GCC_SE_M, 384);
	wr(GCC_SE_N, ~(15625u - 384u) & 0xffff);
	wr(GCC_SE_D, ~15625u & 0xffff);
	wr(GCC_SE_CFG_RCGR, 0 | SOURCE_GPLL0_EVEN << 8 | 2u << 12);
	wr(GCC_SE_CMD_RCGR, rd(GCC_SE_CMD_RCGR) | 1);
	wr(GCC_VOTE, rd(GCC_VOTE) | 1u << 27);
	for (int i = 0; i < 2000 && ((rd(GCC_SE_CMD_RCGR) & 1) || (rd(GCC_SE_CBCR) & CLOCK_OFF)); i++) {
		sleep_us(10);
	}
	printf("bt: clocks: votes 0x%x, root command 0x%x, configuration 0x%x, branch 0x%x\n", rd(GCC_VOTE), rd(GCC_SE_CMD_RCGR),
	    rd(GCC_SE_CFG_RCGR), rd(GCC_SE_CBCR));
	return (rd(GCC_SE_CBCR) & CLOCK_OFF) == 0 && (rd(GCC_SE_CMD_RCGR) & 1) == 0;
}

/* ---- the UART: the serial engine (GENI) in FIFO mode, polled ------------------------------------------ */

#define SE_PHYS                 0x00a94000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a main command is running, bit 12: a secondary one */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_SER_S_CLK_CFG      (se + 0x04c)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 2 = UART */
#define GENI_CLK_SEL            (se + 0x07c)
#define UART_LOOPBACK_CFG       (se + 0x22c)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define UART_TX_TRANS_CFG       (se + 0x25c)    /* bit 1: CTS is not looked at */
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define UART_TX_WORD_LEN        (se + 0x268)
#define UART_TX_STOP_BIT_LEN    (se + 0x26c)
#define UART_TX_TRANS_LEN       (se + 0x270)
#define UART_RX_TRANS_CFG       (se + 0x280)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define UART_RX_WORD_LEN        (se + 0x28c)
#define UART_RX_STALE_CNT       (se + 0x294)
#define UART_TX_PARITY_CFG      (se + 0x2a4)
#define UART_RX_PARITY_CFG      (se + 0x2a8)
#define UART_MANUAL_RFR         (se + 0x2ac)    /* bit 31 by hand, bit 1 "not ready" */
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_CMD0             (se + 0x630)
#define GENI_S_CMD_CTRL         (se + 0x634)
#define GENI_S_IRQ_STATUS       (se + 0x640)
#define GENI_S_IRQ_EN           (se + 0x644)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] the transmit FIFO's depth in words */
#define SE_HW_PARAM_1           (se + 0xe28)    /* [21:16] the receive FIFO's */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)
#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define UART_START              (1u << 27)      /* operation 1 of either sequencer */

static uint32_t tx_depth = 16, rx_depth = 16;

/*
 * The engine's root clock, of which a bit time is a whole fraction: 7.3728 MHz for the slow speeds, 102.4 MHz
 * (300 MHz x 128 / 375) for 3.2 Mbaud, as the phone's own kernel has them in its table for this engine.
 */
static uint32_t root_hz = ROOT_HZ;

static void
root_set(uint32_t m, uint32_t n, uint32_t hz)
{
	if (root_hz == hz) {
		return;
	}
	wr(GCC_SE_M, m);
	wr(GCC_SE_N, ~(n - m) & 0xffff);
	wr(GCC_SE_D, ~n & 0xffff);
	wr(GCC_SE_CFG_RCGR, 0 | SOURCE_GPLL0_EVEN << 8 | 2u << 12);
	wr(GCC_SE_CMD_RCGR, rd(GCC_SE_CMD_RCGR) | 1);
	for (int i = 0; i < 2000 && (rd(GCC_SE_CMD_RCGR) & 1); i++) {
		sleep_us(10);
	}
	root_hz = hz;
}

static void
uart_baud(uint32_t baud)
{
	uint32_t divider;

	if (baud > 1000000) {
		root_set(128, 375, 102400000);
	} else {
		root_set(384, 15625, ROOT_HZ);
	}
	divider = root_hz / (baud * sample);
	wr(GENI_CLK_SEL, 0);
	wr(GENI_SER_M_CLK_CFG, divider << 4 | 1);
	wr(GENI_SER_S_CLK_CFG, divider << 4 | 1);
}

/* With flow control: the chip's "ready" line holds our bytes back, and ours follows the room in the receive FIFO. */
static void
uart_flow(bool on)
{
	wr(UART_TX_TRANS_CFG, on ? 0 : 1u << 1);
	wr(UART_MANUAL_RFR, on ? 0 : 1u << 31 | 1u << 1);
}

static void
uart_receive_start(void)
{
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_CMD0, UART_START);
}

static const char *
uart_init(void)
{
	uint32_t protocol = rd(GENI_FW_REVISION_RO) >> 8 & 0xff;

	tx_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	rx_depth = rd(SE_HW_PARAM_1) >> 16 & 0x3f;
	printf("bt: the serial engine: protocol %u (2 is a UART), FIFO %u words out and %u in, status 0x%x\n", protocol, tx_depth,
	    rx_depth, rd(GENI_STATUS));
	if (protocol != 2) {
		return "the serial engine has no UART firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	if (tx_depth < 4 || rx_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	if (level < 2) {
		return NULL;
	}
	/* whatever ran is stopped */
	if (rd(GENI_STATUS) & 1) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		sleep_ms(2);
	}
	if (rd(GENI_STATUS) & 1u << 12) {
		wr(GENI_S_CMD_CTRL, 1u << 1);
		sleep_ms(2);
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(SE_IRQ_EN, 0);                               /* polled: the engine raises no interrupt */
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, 1);
	wr(GENI_RX_RFR_WATERMARK, rx_depth - 4);
	wr(GENI_TX_WATERMARK, 2);
	wr(GENI_M_IRQ_EN, 0x7fc0007f);
	wr(GENI_S_IRQ_EN, 0x7fc0007f | 1u << 26 | 1u << 27);
	/* eight bits, no parity, one stop bit; four bytes to a FIFO word, the first in its low byte */
	wr(GENI_TX_PACKING_CFG0, 0x4380e);
	wr(GENI_TX_PACKING_CFG1, 0xc3e0e);
	wr(GENI_RX_PACKING_CFG0, 0x4380e);
	wr(GENI_RX_PACKING_CFG1, 0xc3e0e);
	wr(UART_LOOPBACK_CFG, 0);
	wr(UART_TX_PARITY_CFG, 0);
	wr(UART_RX_PARITY_CFG, 0);
	wr(UART_RX_TRANS_CFG, 0);
	wr(UART_TX_WORD_LEN, 8);
	wr(UART_RX_WORD_LEN, 8);
	wr(UART_TX_STOP_BIT_LEN, 0);
	wr(UART_RX_STALE_CNT, 160);
	uart_baud(115200);
	uart_flow(false);
	return NULL;
}

static LCK_GRP_DECLARE(bt_lock_group, "hakios-bt");
static LCK_MTX_DECLARE(send_lock, &bt_lock_group);      /* the transmitter: the thread that reads, and a program's write */
static LCK_MTX_DECLARE(packet_lock, &bt_lock_group);    /* the packets waiting for a program */

static bool uart_send_locked(const uint8_t *bytes, uint32_t count, uint32_t wait_ms);

/* Bytes out, and waited for until the last has left the engine. */
static bool
uart_send(const uint8_t *bytes, uint32_t count, uint32_t wait_ms)
{
	bool sent;

	lck_mtx_lock(&send_lock);
	sent = uart_send_locked(bytes, count, wait_ms);
	lck_mtx_unlock(&send_lock);
	return sent;
}

static bool
uart_send_locked(const uint8_t *bytes, uint32_t count, uint32_t wait_ms)
{
	uint64_t until = now_ms() + wait_ms;
	uint32_t sent = 0;

	while (rd(GENI_STATUS) & 1) {
		if (now_ms() > until) {
			printf("bt: uart: the transmitter stays busy (status 0x%x)\n", rd(GENI_STATUS));
			return false;
		}
		sleep_us(200);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(UART_TX_TRANS_LEN, count);
	wr(GENI_M_CMD0, UART_START);
	for (;;) {
		uint32_t status = rd(GENI_M_IRQ_STATUS);

		while (sent < count && (rd(GENI_TX_FIFO_STATUS) & 0xfffffff) < tx_depth - 1) {
			uint32_t word = 0;

			for (unsigned b = 0; b < 4 && sent < count; b++) {
				word |= (uint32_t)bytes[sent++] << (8 * b);
			}
			wr(GENI_TX_FIFO, word);
		}
		if (status & M_CMD_DONE) {
			wr(GENI_M_IRQ_CLEAR, status);
			return sent == count;
		}
		if (now_ms() > until) {
			printf("bt: uart: %u of %u bytes were not sent in %u ms (the chip's ready line, or the clock): interrupt status "
			    "0x%x, FIFO %u words\n", count - sent, count, wait_ms, status, rd(GENI_TX_FIFO_STATUS) & 0xfffffff);
			wr(GENI_M_CMD_CTRL, 1u << 1);
			sleep_ms(2);
			wr(GENI_M_IRQ_CLEAR, 0xffffffff);
			return false;
		}
		sleep_us(100);
	}
}

/* What has come in, into a ring; the chip is held back by the engine itself when the FIFO fills. */
#define RING                    8192
static uint8_t ring[RING];
static uint32_t ring_in, ring_out;      /* counts of bytes ever put and taken */
static uint32_t ring_lost;

static void
uart_poll(void)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x1ffffff;
	uint32_t bytes = words * 4;

	if (words == 0) {
		return;
	}
	if ((status & 1u << 31) && (status >> 28 & 7) != 0) {
		bytes -= 4 - (status >> 28 & 7);
	}
	for (uint32_t w = 0; w < words; w++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (unsigned b = 0; b < 4 && bytes != 0; b++, bytes--) {
			if (ring_in - ring_out < RING) {
				ring[ring_in++ % RING] = (uint8_t)(word >> (8 * b));
			} else {
				ring_lost++;
			}
		}
	}
	wr(GENI_S_IRQ_CLEAR, rd(GENI_S_IRQ_STATUS));
}

static void
uart_flush(void)
{
	uart_poll();
	ring_out = ring_in;
}

/* ---- HCI in H4 packets ------------------------------------------------------------------------------ */

#define H4_COMMAND              1
#define H4_ACL                  2
#define H4_SCO                  3
#define H4_EVENT                4
#define EVENT_COMMAND_COMPLETE  0x0e
#define EVENT_COMMAND_STATUS    0x0f
#define EVENT_LE                0x3e
#define EVENT_VENDOR            0xff

/*
 * The chip's sleep protocol ("in-band sleep"), bytes of their own between packets: before it sends after a pause
 * the chip says 0xfd and waits to be answered 0xfc; 0xfe says that it will be quiet. This side never says that
 * it sleeps, so the chip need not be woken before a command.
 */
#define IBS_SLEEP               0xfe
#define IBS_WAKE                0xfd
#define IBS_WAKE_ACK            0xfc

static uint32_t others;         /* bytes that began no packet */
static uint8_t stray[16];       /* the last of them */
static uint32_t wakes;          /* times the chip asked to be heard */

/* A byte between packets that is none of a packet's: the sleep protocol's, or noise. */
static void
between(uint8_t byte)
{
	if (byte == IBS_WAKE) {
		uint8_t answer = IBS_WAKE_ACK;

		wakes++;
		(void)uart_send(&answer, 1, 200);
	} else if (byte != IBS_SLEEP && byte != IBS_WAKE_ACK) {
		stray[others++ % sizeof(stray)] = byte;
	}
}

/* One byte of what has come in, waited for; -1 when none comes in time. */
static int
byte_in(uint64_t until)
{
	for (;;) {
		if (ring_out != ring_in) {
			return ring[ring_out++ % RING];
		}
		uart_poll();
		if (ring_out != ring_in) {
			continue;
		}
		if (now_ms() > until) {
			return -1;
		}
		sleep_us(500);
	}
}

/*
 * One event of the chip's, without its H4 byte: the event's code, the length, the parameters. Its length is
 * returned (0: none came in time). Data packets are read and dropped, and so are the chip's sleep bytes.
 */
static uint32_t
event_in(uint8_t *event, uint32_t room, uint32_t wait_ms)
{
	uint64_t until = now_ms() + wait_ms;

	for (;;) {
		int kind = byte_in(until);
		uint32_t header, length = 0, taken = 0;

		if (kind < 0) {
			return 0;
		}
		if (kind == H4_EVENT) {
			header = 2;
		} else if (kind == H4_ACL) {
			header = 4;
		} else if (kind == H4_SCO) {
			header = 3;
		} else {
			between((uint8_t)kind);
			continue;
		}
		for (uint32_t i = 0; i < header + length; i++) {
			int c = byte_in(now_ms() + 300);

			if (c < 0) {
				printf("bt: a packet of kind %d stopped after %u bytes\n", kind, i);
				return 0;
			}
			if (i < room) {
				event[i] = (uint8_t)c;
			}
			taken = i + 1;
			if (i == header - 1) {
				length = kind == H4_ACL ? (uint32_t)(event[2] | event[3] << 8) : kind == H4_EVENT ? event[1] : event[2];
			}
		}
		if (kind == H4_EVENT) {
			return taken < room ? taken : room;
		}
	}
}

static void
event_show(const char *what, const uint8_t *event, uint32_t length)
{
	char text[3 * 24 + 1] = "";
	uint32_t shown = length < 24 ? length : 24;

	for (uint32_t i = 0; i < shown; i++) {
		snprintf(text + 3 * i, 4, "%02x ", event[i]);
	}
	printf("bt: %s: %u bytes: %s%s\n", what, length, text, length > shown ? "..." : "");
}

static bool
command_out(uint16_t opcode, const uint8_t *parameters, uint8_t count)
{
	uint8_t packet[4 + 255];

	packet[0] = H4_COMMAND;
	packet[1] = (uint8_t)opcode;
	packet[2] = (uint8_t)(opcode >> 8);
	packet[3] = count;
	if (count != 0) {
		memcpy(packet + 4, parameters, count);
	}
	return uart_send(packet, 4u + count, 3000);
}

/*
 * An event waited for: of this code and, for "command complete" and "command status", of this command. Others
 * that come first are shown and passed over. Its length; 0 when it does not come.
 */
static uint32_t
event_wait(uint8_t code, uint16_t opcode, uint8_t *event, uint32_t room, uint32_t wait_ms, bool quiet)
{
	uint64_t until = now_ms() + wait_ms;

	while (now_ms() <= until) {
		uint32_t length = event_in(event, room, (uint32_t)(until - now_ms()) + 1);

		if (length == 0) {
			break;
		}
		if (event[0] == code && (code != EVENT_COMMAND_COMPLETE || (length >= 5 && (event[3] | event[4] << 8) == opcode))) {
			return length;
		}
		if (!quiet) {
			event_show("an event passed over", event, length);
		}
	}
	return 0;
}

/* A command and its "command complete"; the event's length, 0 when it failed. */
static uint32_t
command(uint16_t opcode, const uint8_t *parameters, uint8_t count, uint8_t *event, uint32_t room, const char *what)
{
	uint32_t length;

	if (!command_out(opcode, parameters, count)) {
		printf("bt: %s: the command was not sent\n", what);
		return 0;
	}
	length = event_wait(EVENT_COMMAND_COMPLETE, opcode, event, room, 3000, false);
	if (length == 0) {
		printf("bt: %s: no answer (%u bytes that began no packet; the last: %02x %02x %02x %02x %02x %02x %02x %02x; %u lost; "
		    "receive status 0x%x, engine 0x%x, pins CTS %u RX %u)\n", what, others, stray[(others + 8) % 16],
		    stray[(others + 9) % 16], stray[(others + 10) % 16], stray[(others + 11) % 16], stray[(others + 12) % 16],
		    stray[(others + 13) % 16], stray[(others + 14) % 16], stray[(others + 15) % 16], ring_lost,
		    rd(GENI_S_IRQ_STATUS), rd(GENI_STATUS), rd(GPIO_IO(PIN_CTS)) & 1, rd(GPIO_IO(PIN_RX)) & 1);
	} else if (length >= 6 && event[5] != 0) {
		printf("bt: %s: refused, status 0x%02x\n", what, event[5]);
		return 0;
	}
	return length;
}

/* ---- the chip ----------------------------------------------------------------------------------------- */

#define QCA_PATCH_OPCODE        0xfc00
#define QCA_VERSION_REQUEST     0x19
#define QCA_TLV_REQUEST         0x1e
#define QCA_SEGMENT             243
#define QCA_POWER_ON            0xfc
#define QCA_POWER_OFF           0xc0
#define NVM_TAG_HCI             17
#define NVM_TAG_DEEP_SLEEP      27

static uint32_t product_id, soc_id;
static uint16_t rom_version, patch_version;
static bool newer;              /* a WCN3991 or later: it answers with "command complete", and its settings differ */
static bool fast = true;        /* the UART at 3.2 Mbaud once the chip has said which it is (bt_slow=1: at 115200) */
#define QCA_BAUD_115200         0x00
#define QCA_BAUD_3200000        0x11

/* The chip switched off and on again with its two bytes; then flow control, and what it said meanwhile dropped. */
static bool
power_pulse(void)
{
	uint8_t byte;
	bool sent;

	uart_flow(false);
	uart_baud(2400);
	byte = QCA_POWER_OFF;
	sent = uart_send(&byte, 1, 500);
	sleep_ms(10);
	uart_baud(115200);
	byte = QCA_POWER_ON;
	sent = uart_send(&byte, 1, 500) && sent;
	sleep_ms(100);
	uart_receive_start();
	uart_flush();
	uart_flow(true);
	return sent;
}

static bool
version_read(void)
{
	uint8_t request = QCA_VERSION_REQUEST, event[64];
	const uint8_t *version = NULL;
	uint64_t until = now_ms() + 2000;

	if (!command_out(QCA_PATCH_OPCODE, &request, 1)) {
		return false;
	}
	while (version == NULL && now_ms() <= until) {
		uint32_t length = event_in(event, sizeof(event), (uint32_t)(until - now_ms()) + 1);

		if (length == 0) {
			break;
		}
		event_show("after the version request", event, length);
		if (event[0] == EVENT_VENDOR && length >= 4 + 12 && event[2] == 0x00 && (event[3] == 0x02 || event[3] == 0x19)) {
			version = event + 4;
		} else if (event[0] == EVENT_COMMAND_COMPLETE && length >= 8 + 12 && event[5] == 0x00 && event[6] == 0x19) {
			version = event + 8;            /* (the newer chips, the WCN3991 on, answer with this one) */
			newer = true;
		}
	}
	if (version == NULL) {
		return false;
	}
	product_id = version[0] | version[1] << 8 | version[2] << 16 | (uint32_t)version[3] << 24;
	patch_version = (uint16_t)(version[4] | version[5] << 8);
	rom_version = (uint16_t)(version[6] | version[7] << 8);
	soc_id = version[8] | version[9] << 8 | version[10] << 16 | (uint32_t)version[11] << 24;
	printf("bt: THE CHIP ANSWERS: product 0x%08x, chip 0x%08x, ROM 0x%04x, patch 0x%04x\n", product_id, soc_id, rom_version,
	    patch_version);
	return true;
}

/* A file of the chip's sent to it, 243 bytes at a time. */
static uint8_t settings[16384];

static bool
download(const char *name, bool is_settings)
{
	uint64_t size = 0, started = now_ms();
	const uint8_t *data = hakios_image_file("fw", name, &size, NULL);
	uint8_t packet[2 + QCA_SEGMENT], event[32];
	uint32_t mode = 0, segments = 0, answers = 0;

	if (data == NULL || size < 16) {
		printf("bt: no fw/%s in the image: it is the phone's /vendor/bt_firmware/image/%s\n", name, name);
		return false;
	}
	if (is_settings) {
		uint32_t length, at = 4;

		if (size > sizeof(settings)) {
			printf("bt: %s is %llu bytes, more than the %u there is room for\n", name, size, (unsigned)sizeof(settings));
			return false;
		}
		memcpy(settings, data, size);
		data = settings;
		/* the settings are a block of kind 2, which may be wrapped in one of kind 4 */
		if (settings[0] != 2 && settings[4] == 2) {
			at = 8;
		}
		length = (settings[at - 3] | settings[at - 2] << 8 | (uint32_t)settings[at - 1] << 16) + at;
		if (length > size) {
			length = (uint32_t)size;
		}
		/* its entries: a number, a length, eight bytes, the value */
		while (at + 12 <= length) {
			uint32_t tag = settings[at] | settings[at + 1] << 8, tag_length = settings[at + 2] | settings[at + 3] << 8;

			if (at + 12 + tag_length > length) {
				break;
			}
			if (tag == NVM_TAG_HCI || tag == NVM_TAG_DEEP_SLEEP) {
				printf("bt: %s: entry %u, %u bytes: %02x %02x %02x %02x %02x %02x\n", name, tag, tag_length, settings[at + 12],
				    settings[at + 13], settings[at + 14], settings[at + 15], settings[at + 16], settings[at + 17]);
			}
			if (tag == NVM_TAG_HCI && tag_length >= 3) {
				settings[at + 12] |= 0x80;
				settings[at + 12 + (newer ? 1 : 2)] = fast ? QCA_BAUD_3200000 : QCA_BAUD_115200;
			} else if (tag == NVM_TAG_DEEP_SLEEP && tag_length >= 1) {
				settings[at + 12] &= (uint8_t)~1u;      /* the chip does not go to sleep on the UART */
			}
			at += 12 + tag_length;
		}
	} else {
		mode = data[14];        /* which answers the chip leaves out while the patch comes: 0 none, 1 or 3 its own events */
	}
	printf("bt: %s: %llu bytes to send%s\n", name, size, is_settings ? "" : mode == 0 ? ", each piece answered" :
	    ", only the last piece answered");
	for (uint64_t at = 0; at < size; at += QCA_SEGMENT) {
		uint32_t piece = size - at < QCA_SEGMENT ? (uint32_t)(size - at) : QCA_SEGMENT;
		bool last = at + piece >= size, answered = mode == 0 || mode == 2 || last || piece < QCA_SEGMENT;

		packet[0] = QCA_TLV_REQUEST;
		packet[1] = (uint8_t)piece;
		memcpy(packet + 2, data + at, piece);
		if (!command_out(QCA_PATCH_OPCODE, packet, (uint8_t)(piece + 2))) {
			printf("bt: %s: piece %u was not sent\n", name, segments);
			return false;
		}
		segments++;
		if (answered) {
			/* the older chips answer with an event of their own, the newer with "command complete" */
			uint32_t length = event_wait(newer ? EVENT_COMMAND_COMPLETE : EVENT_VENDOR, QCA_PATCH_OPCODE, event, sizeof(event),
			    last ? 5000 : 3000, false);

			if (length == 0) {
				printf("bt: %s: piece %u of %llu bytes at %llu was not answered\n", name, segments - 1, size, at);
				return false;
			}
			if (newer ? (length >= 7 && (event[5] != 0x00 || event[6] != QCA_TLV_REQUEST)) :
			    (length >= 5 && (event[2] != 0x00 || event[3] != 0x04 || event[4] != 0x00))) {
				event_show("a piece was answered with", event, length);
				return false;
			}
			answers++;
		}
	}
	printf("bt: %s: sent in %u pieces, %u answered, %llu ms\n", name, segments, answers, now_ms() - started);
	return true;
}

/*
 * The controller's address. The chip's settings hold one that is the same in every phone; Android keeps the
 * phone's own in its storage and writes it into the chip at every start. Here one is made of the SoC's serial
 * number: this phone's alone, and the same at every start. It is written with the chip's own command for it.
 */
static uint8_t address[6];      /* as it is written down, the first byte first */
static bool address_set;
static int controller_up;

static void
address_give(void)
{
	uint8_t reversed[6], event[32];
	uint32_t serial = 0, length;
	uint64_t mixed;

	if (!hakios_soc_serial(&serial)) {
		printf("bt: the SoC's serial number is not to be had: the controller keeps the address of its settings\n");
	} else {
		mixed = (serial ^ 0x426c7565ull) * 0x9e3779b97f4a7c15ull;      /* (spread over the bytes: not the number itself) */
		mixed ^= mixed >> 29;
		address[0] = 0x06;
		for (unsigned i = 1; i < 6; i++) {
			address[i] = (uint8_t)(mixed >> (8 * i));
		}
		/* (this command takes the address as it is written down, not last byte first as HCI has it) */
		if (command_out(0xfc14, address, 6)) {
			length = event_wait(newer ? EVENT_COMMAND_COMPLETE : EVENT_VENDOR, 0xfc14, event, sizeof(event), 2000, false);
			if (length == 0) {
				printf("bt: the address was not taken\n");
			}
		}
	}
	length = command(0x1009, NULL, 0, event, sizeof(event), "the address");
	if (length >= 12) {
		bool ours = true, none = true;

		for (unsigned i = 0; i < 6; i++) {
			ours = ours && event[6 + i] == address[5 - i];
			none = none && event[6 + i] == 0;
			reversed[i] = event[11 - i];
		}
		memcpy(address, reversed, 6);
		address_set = !none;
		printf("bt: THE CONTROLLER IS UP: its address is %s\n", ours ? "this phone's own, made of the SoC's serial number" :
		    none ? "missing" : "the one of its settings");
		if (address_set) {
			hakios_bt_publish(address);
		}
	}
}

SYSCTL_DECL(_kern_hakios);

static int
sysctl_bt_address SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[24] = "";

	if (address_set) {
		snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2], address[3], address[4],
		    address[5]);
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, bt_address, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_bt_address, "A",
    "the Bluetooth controller's address");
SYSCTL_INT(_kern_hakios, OID_AUTO, bt_up, CTLFLAG_RD | CTLFLAG_LOCKED, &controller_up, 0,
    "0 no Bluetooth asked for, 1 the controller is being started, 2 it is a program's (/dev/hakios-bt), -1 it failed");

static bool
chip_start(void)
{
	char name[32];
	uint8_t event[64];
	uint32_t length, rom;
	bool apache;

	if (!power_pulse()) {
		printf("bt: the power bytes could not be sent\n");
		return false;
	}
	if (!version_read()) {
		/* the engine may count its samples differently: once more the other way */
		sample = sample == 16 ? 32 : 16;
		printf("bt: no version from the chip; once more with %u samples to the bit\n", sample);
		if (!power_pulse() || !version_read()) {
			printf("bt: the chip does not answer (%u bytes that began no packet)\n", others);
			return false;
		}
	}
	if (level < 3) {
		return true;
	}
	if (fast) {
		uint8_t code = QCA_BAUD_3200000;

		/*
		 * The chip's speed, then ours. Its answer to this comes at the new speed, if it comes: whatever comes in
		 * the next moments is dropped.
		 */
		if (!command_out(0xfc48, &code, 1)) {
			printf("bt: the command for the UART's speed could not be sent\n");
			return false;
		}
		sleep_ms(10);
		uart_baud(3200000);
		sleep_ms(40);
		uart_flush();
		printf("bt: the UART is at 3.2 Mbaud\n");
	}
	rom = (rom_version & 0xf00) >> 4 | (rom_version & 0xf);
	apache = soc_id >> 16 == 0x4009;
	snprintf(name, sizeof(name), "%sbtfw%02x.tlv", apache ? "ap" : "cr", rom);
	if (!download(name, false)) {
		return false;
	}
	sleep_ms(10);
	snprintf(name, sizeof(name), "%snv%02x.bin", apache ? "ap" : "cr", rom);
	if (!download(name, true)) {
		return false;
	}
	sleep_ms(50);
	uart_poll();
	if (newer) {
		static const uint8_t quiet[2] = { 0x14, 0x00 };

		/* (the chip's own log, which it would send up the UART) */
		(void)command(0xfc17, quiet, sizeof(quiet), event, sizeof(event), "the chip's logging off");
	}
	if (command(0x0c03, NULL, 0, event, sizeof(event), "HCI reset") == 0) {
		return false;
	}
	length = command(0x1001, NULL, 0, event, sizeof(event), "the local version");
	if (length >= 14) {
		printf("bt: HCI version %u (revision 0x%04x), LMP %u (0x%04x), maker 0x%04x\n", event[6], event[7] | event[8] << 8,
		    event[9], event[12] | event[13] << 8, event[10] | event[11] << 8);
	}
	address_give();
	return address_set;
}

/* Bluetooth LE advertisements counted for a while: the radio hears. */
static void
scan(void)
{
	static const uint8_t parameters[7] = { 0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
	uint8_t enable[2] = { 1, 0 }, event[64];
	uint32_t reports = 0;
	int strongest = -128;
	uint64_t until;

	if (command(0x200b, parameters, sizeof(parameters), event, sizeof(event), "the scan's settings") == 0 ||
	    command(0x200c, enable, sizeof(enable), event, sizeof(event), "the scan's start") == 0) {
		return;
	}
	until = now_ms() + scan_seconds * 1000;
	while (now_ms() < until) {
		uint32_t length = event_in(event, sizeof(event), (uint32_t)(until - now_ms()) + 1);

		if (length >= 4 && event[0] == EVENT_LE && event[2] == 0x02) {
			/* (one report to an event, as a rule: its signal strength is its last byte) */
			int rssi = (int8_t)event[length - 1];

			reports++;
			if (length == (uint32_t)event[1] + 2 && rssi > strongest && rssi < 20) {
				strongest = rssi;
			}
		}
	}
	enable[0] = 0;
	(void)command(0x200c, enable, sizeof(enable), event, sizeof(event), "the scan's end");
	printf("bt: THE RADIO HEARS: %u advertisements in %u s, the strongest at %d dBm (%u bytes lost, %u that began no packet, "
	    "the chip woke %u times)\n", reports, scan_seconds, strongest, ring_lost, others, wakes);
}

/* ---- /dev/hakios-bt: the controller for a program, HCI in H4 packets both ways --------------------------- */

#define PACKETS                 32768
static uint8_t packets[PACKETS];        /* whole packets, each with its H4 byte first */
static uint32_t packets_in, packets_out, packets_dropped;
static struct selinfo packets_select;
static bool opened;

/* What has come in, cut into packets and handed on; the bytes between them dealt with. True if anything was taken. */
static bool
packets_take(void)
{
	static uint64_t waiting_since;
	bool taken = false;

	for (;;) {
		uint32_t have = ring_in - ring_out, header, length, whole;
		uint8_t kind;

		if (have == 0) {
			break;
		}
		kind = ring[ring_out % RING];
		header = kind == H4_EVENT ? 2 : kind == H4_ACL ? 4 : kind == H4_SCO ? 3 : 0;
		if (header == 0) {
			ring_out++;
			between(kind);
			taken = true;
			continue;
		}
		if (have >= 1 + header) {
			length = kind == H4_ACL ? (uint32_t)(ring[(ring_out + 3) % RING] | ring[(ring_out + 4) % RING] << 8) :
			    kind == H4_EVENT ? ring[(ring_out + 2) % RING] : ring[(ring_out + 3) % RING];
			whole = 1 + header + length;
		} else {
			whole = 1 + header;
		}
		if (have < whole || have < 1 + header) {
			/* the rest is on its way; a packet that never ends is given up byte by byte */
			if (waiting_since == 0) {
				waiting_since = now_ms();
			} else if (now_ms() - waiting_since > 1000) {
				ring_out++;
				others++;
				waiting_since = 0;
			}
			break;
		}
		waiting_since = 0;
		lck_mtx_lock(&packet_lock);
		if (opened && PACKETS - (packets_in - packets_out) >= whole) {
			for (uint32_t i = 0; i < whole; i++) {
				packets[packets_in++ % PACKETS] = ring[(ring_out + i) % RING];
			}
		} else if (opened) {
			packets_dropped++;
		}
		lck_mtx_unlock(&packet_lock);
		ring_out += whole;
		taken = true;
		if (opened) {
			wakeup(&packets_in);
			selwakeup(&packets_select);
		}
	}
	return taken;
}

static int
btdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&packet_lock);
	if (opened) {
		lck_mtx_unlock(&packet_lock);
		return EBUSY;
	}
	packets_out = packets_in;
	opened = true;
	lck_mtx_unlock(&packet_lock);
	printf("bt: /dev/hakios-bt was opened\n");
	return 0;
}

static int
btdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&packet_lock);
	opened = false;
	lck_mtx_unlock(&packet_lock);
	wakeup(&packets_in);
	printf("bt: /dev/hakios-bt was closed (%u packets were dropped for want of room)\n", packets_dropped);
	return 0;
}

static int
btdev_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	uint8_t piece[1024];
	uint32_t count = 0;
	int error = 0;

	lck_mtx_lock(&packet_lock);
	while (packets_in == packets_out) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&packet_lock);
			return EAGAIN;
		}
		error = msleep(&packets_in, &packet_lock, PZERO | PCATCH, "hakios-bt", NULL);
		if (error != 0 || !opened) {
			lck_mtx_unlock(&packet_lock);
			return error != 0 ? error : ENXIO;
		}
	}
	while (count < sizeof(piece) && count < (uint32_t)uio_resid(uio) && packets_out != packets_in) {
		piece[count++] = packets[packets_out++ % PACKETS];
	}
	lck_mtx_unlock(&packet_lock);
	return uiomove((caddr_t)piece, (int)count, uio);
}

static int
btdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t piece[1024];

	while (uio_resid(uio) > 0) {
		uint32_t count = (uint32_t)MIN((user_ssize_t)sizeof(piece), uio_resid(uio));
		int error = uiomove((caddr_t)piece, (int)count, uio);

		if (error != 0) {
			return error;
		}
		if (!uart_send(piece, count, 3000)) {
			return EIO;
		}
	}
	return 0;
}

static int
btdev_select(__unused dev_t dev, int which, void *wql, struct proc *p)
{
	int ready = 0;

	if (which == FWRITE) {
		return 1;
	}
	if (which != FREAD) {
		return 0;
	}
	lck_mtx_lock(&packet_lock);
	if (packets_in != packets_out) {
		ready = 1;
	} else {
		selrecord(p, &packets_select, wql);
	}
	lck_mtx_unlock(&packet_lock);
	return ready;
}

static const struct cdevsw btdev_cdevsw = {
	.d_open = btdev_open, .d_close = btdev_close, .d_read = btdev_read, .d_write = btdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = btdev_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* The controller is a program's from here on: what it sends is cut into packets for whoever has the device open. */
static void
serve(void)
{
	int major = cdevsw_add(-1, &btdev_cdevsw);
	uint64_t said = now_ms();

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-bt") == NULL) {
		printf("bt: /dev/hakios-bt cannot be made\n");
		return;
	}
	printf("bt: THE CONTROLLER IS A PROGRAM'S (/dev/hakios-bt): HCI in H4 packets, %s baud\n", fast ? "3.2 M" : "115200");
	controller_up = 2;
	for (;;) {
		uart_poll();
		if (!packets_take()) {
			sleep_us(opened ? 1000 : 5000);
		}
		if (now_ms() - said > 30000) {
			said = now_ms();
			printf("bt: %u bytes in, %u lost, %u that began no packet, the chip woke %u times, %u packets dropped\n", ring_in,
			    ring_lost, others, wakes, packets_dropped);
		}
	}
}

static void
bt_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t delay = 12;
	const char *why;

	(void)PE_parse_boot_argn("bt_delay", &delay, sizeof(delay));
	sleep_ms((delay > 600 ? 600 : delay) * 1000);
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	if (tlmm == 0 || gcc == 0 || se == 0 || rsc == 0 || cmd_db == 0) {
		printf("bt: the registers could not be mapped; off\n");
		return;
	}
	printf("bt: level %u\n", level);
	pins_show("at start");
	if (!clocks_on()) {
		printf("bt: the UART's clock does not run; off\n");
		return;
	}
	why = uart_init();
	if (why != NULL) {
		printf("bt: %s; off\n", why);
		return;
	}
	if (level < 2) {
		return;
	}
	if (!supplies_on()) {
		return;
	}
	sleep_ms(50);
	pins_uart();
	pins_show("as a UART");
	if (!chip_start()) {
		printf("bt: the chip was not started\n");
		controller_up = -1;
		return;
	}
	if (level >= 4) {
		scan();
	}
	serve();
}

__startup_func
static void
bt_init(void)
{
	thread_t thread;
	uint32_t value;

	if (!PE_parse_boot_argn("bt", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (PE_parse_boot_argn("bt_sample", &value, sizeof(value)) && (value == 16 || value == 32)) {
		sample = value;
	}
	if (PE_parse_boot_argn("bt_slow", &value, sizeof(value)) && value != 0) {
		fast = false;
	}
	if (PE_parse_boot_argn("bt_scan", &value, sizeof(value)) && value >= 1 && value <= 120) {
		scan_seconds = value;
	}
	controller_up = level >= 3 ? 1 : 0;
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt_init);
