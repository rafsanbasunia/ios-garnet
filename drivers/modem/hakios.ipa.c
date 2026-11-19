/*
 * The IPA's data path for this processor: GSI rings, the IPA's endpoints for the application
 * processor, and the immediate commands that give the IPA its tables.
 *
 * The IPA (Qualcomm's "IP accelerator", here IPA 5.2 with GSI 3.0: the POCO X6 5G, SM7435) moves
 * packets between the modem and this processor. Each side has "endpoints" (pipes) in the IPA and
 * "channels" in the GSI (the DMA engine beside it): a channel is a ring of transfer elements (TREs:
 * address, length, flags; 16 bytes) that this processor fills and the hardware consumes, and an
 * event ring the hardware fills with one event per finished TRE. Writing a doorbell register tells
 * the hardware how far the ring is filled; the event ring's write pointer, read from a register,
 * tells us how far the hardware got. No interrupt is used: the interface's thread polls
 * (hakios.cell.c).
 *
 * What this processor needs, as Linux's driver sets them up for IPA 5.2 (drivers/net/ipa,
 * data/ipa_data-v5.2.c, gsi.c, ipa_endpoint.c, ipa_cmd.c, ipa_table.c, ipa_mem.c; Linux drives
 * 5.2 with the register layout of 5.0: reg/gsi_reg-v5.0.c and reg/ipa_reg-v5.0.c):
 *   - the command endpoint (9, channel 6): immediate commands to the IPA itself, which is how it
 *     is told where its filter and route tables and its header table are;
 *   - the "LAN" receive endpoint (11, channel 7): the IPA's default route (what matches no rule)
 *     and its exceptions end here; it is looked at and dropped;
 *   - the modem transmit endpoint (2, channel 5): IP packets to the modem, each behind a QMAP
 *     header (4 bytes: pad length, mux id, packet length, big-endian);
 *   - the modem receive endpoint (18, channel 9): IP packets from the modem, QMAP-framed, several
 *     per buffer (the IPA aggregates them until 12 KB or 500 us).
 * No checksum offload, no status packets: the network stack computes its checksums itself and
 * the modem is told QMAP without checksum (hakios.mpss.c, WDA Set Data Format). Linux's data for
 * 5.2 has both on (checksum on the two modem endpoints, status on the LAN receive and the modem
 * transmit endpoint); that the IPA works without them here as it did on 4.11 is assumed.
 *
 * All of this is done before the modem starts (Linux does the same): the tables the commands
 * write hold pointers to one "zero rule", and the modem then writes its own rules into its part
 * of them. The IPA reaches this memory through the SMMU context the modem driver gave it, which
 * shows DDR as it is (physical addresses). The memory is cached for us, so every buffer is
 * written back and dropped from the cache (dc civac) before the hardware looks at it and before
 * we look at what the hardware wrote.
 *
 * The order is Linux's ipa_setup(): the GSI looked at (enabled, how many channels and event
 * rings), every interrupt masked, the four event rings and channels allocated and programmed,
 * the four endpoints programmed, the command channel started, the header table and the regions
 * Linux zeroes, the four tables, the LAN receive channel started and the default route pointed
 * at it, then the two modem channels. Each stage says what it read back ("ipa: gsi:", "ipa:
 * endpoint", "ipa: command:"); one that fails says so in capitals ("ipa: DATA PATH FAILED"),
 * what was set up is undone (channels stopped, reset and given back, the event rings too), and
 * the modem driver is told that there is no data path.
 *
 * modem_ipa_data_off=1: none of this. modem_ipa_data_trace=1: a line per packet.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <sys/hakios.ipa.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

static LCK_GRP_DECLARE(ipa_lock_group, "hakios-ipa");
static LCK_MTX_DECLARE(ipa_lock, &ipa_lock_group);

/* ---- the registers ---------------------------------------------------------------------------- */

/*
 * Where the IPA and the GSI are on this phone (dt: qcom,ipa@3e00000: reg "ipa-base" 0x3e00000,
 * whose registers begin qcom,ipa-cfg-offset = 0x140000 further, and "gsi-base" 0x3e04000; the
 * same addresses as Linux's "ipa-reg" and "gsi" for the SoCs with IPA 5.x). Another phone
 * overrides them with the boot-args soc_ipa_base (the first IPA register, not the node's reg)
 * and soc_gsi_base. Of the IPA's registers the first 0x2000 bytes are used (the endpoints'
 * registers end at 0x1970 for route 18); of the GSI's, from the inter-EE masks (0xc01c) to our
 * execution environment's scratch (0x25400). Nothing outside these two mappings is ever read or
 * written from here: ipa_rd/ipa_wr/gsi_rd/gsi_wr refuse an offset outside them.
 */
#define IPA_REGS_PHYS_DEFAULT   0x03f40000ull
#define GSI_PHYS_DEFAULT        0x03e04000ull
#define IPA_REGS_SIZE           0x2000
#define GSI_FROM                0xc000          /* what is used of the GSI's registers: from the inter-EE masks (0xc01c) to the scratch (0x25400) */
#define GSI_SIZE                0x1a000

static uint64_t ipa_regs_phys = IPA_REGS_PHYS_DEFAULT, gsi_phys = GSI_PHYS_DEFAULT;
static vm_offset_t ipa_regs, gsi_regs;         /* gsi_regs maps gsi_phys + GSI_FROM */
static uint32_t outside;                        /* accesses refused for being outside the mappings (a mistake in this file) */

static inline bool
ipa_inside(uint32_t offset)
{
	if (ipa_regs != 0 && offset <= IPA_REGS_SIZE - 4 && (offset & 3) == 0) {
		return true;
	}
	if (outside++ < 4) {
		printf("ipa: AN IPA REGISTER OFFSET %#x IS OUTSIDE WHAT IS MAPPED: not touched\n", offset);
	}
	return false;
}

static inline bool
gsi_inside(uint32_t offset)
{
	if (gsi_regs != 0 && offset >= GSI_FROM && offset - GSI_FROM <= GSI_SIZE - 4 && (offset & 3) == 0) {
		return true;
	}
	if (outside++ < 4) {
		printf("ipa: A GSI REGISTER OFFSET %#x IS OUTSIDE WHAT IS MAPPED: not touched\n", offset);
	}
	return false;
}

static inline uint32_t
ipa_rd(uint32_t offset)
{
	return ipa_inside(offset) ? *(volatile uint32_t *)(ipa_regs + offset) : 0;
}

static inline void
ipa_wr(uint32_t offset, uint32_t value)
{
	if (ipa_inside(offset)) {
		*(volatile uint32_t *)(ipa_regs + offset) = value;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static inline uint32_t
gsi_rd(uint32_t offset)
{
	return gsi_inside(offset) ? *(volatile uint32_t *)(gsi_regs + (offset - GSI_FROM)) : 0;
}

static inline void
gsi_wr(uint32_t offset, uint32_t value)
{
	if (gsi_inside(offset)) {
		*(volatile uint32_t *)(gsi_regs + (offset - GSI_FROM)) = value;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

/*
 * The GSI's registers for our execution environment (EE 0; another EE's are 0x12000 further),
 * GSI 3.0. Linux: reg/gsi_reg-v5.0.c. The context registers are 0x80 apart per channel or event
 * ring, the doorbells 8. The interrupt source registers are arrays of 32 channels each in
 * Android's driver (gsihal_reg.c); every number used here is below 32, so the first of each.
 */
#define CH_CNTXT(c, n)          (0x14000 + 0x80 * (c) + 4 * (n))       /* n 0..3: type/dir/state, length and event ring, address */
#define CH_QOS(c)               (0x14048 + 0x80 * (c))
#define CH_SCRATCH(c, n)        (0x1404c + 0x80 * (c) + 4 * (n))
#define EV_CNTXT(e, n)          (0x1c000 + 0x80 * (e) + 4 * (n))       /* n 0..4, 8..13: type/state, length, address, write pointer, moderation, MSI */
#define CH_DOORBELL(c)          (0x24000 + 8 * (c))
#define EV_DOORBELL(e)          (0x24800 + 8 * (e))
#define GSI_STATUS              0x25000         /* bit 0: enabled */
#define CH_CMD                  0x25008
#define EV_CH_CMD               0x25010
#define GSI_HW_PARAM_2          0x25040         /* bits 7:0: channels per EE */
#define GSI_SW_VERSION          0x25044         /* (Android's gsihal_reg.c) */
#define GSI_HW_PARAM_4          0x25050         /* bits 7:0: event rings per EE */
#define CNTXT_TYPE_IRQ          0x25080
#define CNTXT_TYPE_IRQ_MSK      0x25088
#define CNTXT_SRC_CH_IRQ        0x25090
#define CNTXT_SRC_CH_IRQ_MSK    0x25094
#define CNTXT_SRC_CH_IRQ_CLR    0x25098
#define CNTXT_SRC_EV_CH_IRQ     0x2509c
#define CNTXT_SRC_EV_CH_IRQ_MSK 0x250a0
#define CNTXT_SRC_EV_CH_IRQ_CLR 0x250a4
#define CNTXT_SRC_IEOB_IRQ      0x250a8
#define CNTXT_SRC_IEOB_IRQ_MSK  0x250ac
#define CNTXT_SRC_IEOB_IRQ_CLR  0x250b0
#define CNTXT_GLOB_IRQ_STTS     0x25200
#define CNTXT_GLOB_IRQ_EN       0x25204
#define CNTXT_GLOB_IRQ_CLR      0x25208
#define CNTXT_GSI_IRQ_STTS      0x2520c
#define CNTXT_GSI_IRQ_EN        0x25210
#define CNTXT_GSI_IRQ_CLR       0x25214
#define CNTXT_INTSET            0x25220
#define ERROR_LOG               0x25240
#define ERROR_LOG_CLR           0x25244
#define INTER_EE_SRC_CH_IRQ_MSK 0xc01c
#define INTER_EE_SRC_EV_CH_IRQ_MSK 0xc028

#define GSI_CH_ALLOCATE         0x0     /* channel commands (CH_CMD bits 31:24; the channel in 7:0) */
#define GSI_CH_START            0x1
#define GSI_CH_STOP             0x2
#define GSI_CH_RESET            0x9
#define GSI_CH_DE_ALLOC         0xa
#define GSI_EVT_ALLOCATE        0x0     /* event ring commands (EV_CH_CMD, the same layout) */
#define GSI_EVT_RESET           0x9
#define GSI_EVT_DE_ALLOC        0xa
#define CH_STATE_NOT_ALLOCATED  0       /* CH_CNTXT 0 bits 23:20 */
#define CH_STATE_ALLOCATED      1
#define CH_STATE_STARTED        2
#define CH_STATE_STOPPED        3
#define CH_STATE_STOP_IN_PROC   4
#define EV_STATE_NOT_ALLOCATED  0       /* EV_CNTXT 0 bits 23:20 */
#define EV_STATE_ALLOCATED      1
#define GSI_CHANNEL_TYPE_GPI    2
#define ELEMENT_SIZE            16

/*
 * The IPA's registers, IPA 5.0's layout. Linux: reg/ipa_reg-v5.0.c; the two version registers
 * from Android's ipahal_reg.c. The endpoints' are 0x80 apart from 0x1000.
 */
#define IPA_FLAVOR_0            0x000           /* bits 7:0 pipes, 15:8 consumer pipes (ours that send), 23:16 producer pipes, 31:24 the lowest producer */
#define IPA_COMP_HW_VERSION     0x024
#define IPA_VERSION             0x028
#define IPA_ROUTE               0x038           /* bits 7:0 default pipe, 15:8 fragments' pipe, 25:16 header offset, 26 header table, 27 retain header, 28 disable */
#define IPA_SHARED_MEM_SIZE     0x040           /* bits 15:0 the local memory's size, 31:16 its offset, both in 8-byte units */
#define ENDP_INIT_CFG(ep)       (0x1008 + 0x80 * (ep))
#define ENDP_INIT_NAT(ep)       (0x100c + 0x80 * (ep))
#define ENDP_INIT_HDR(ep)       (0x1010 + 0x80 * (ep))
#define ENDP_INIT_HDR_EXT(ep)   (0x1014 + 0x80 * (ep))
#define ENDP_INIT_HDR_METADATA_MASK(ep) (0x1018 + 0x80 * (ep))
#define ENDP_INIT_MODE(ep)      (0x1020 + 0x80 * (ep))
#define ENDP_INIT_AGGR(ep)      (0x1024 + 0x80 * (ep))
#define ENDP_INIT_HOL_BLOCK_EN(ep) (0x102c + 0x80 * (ep))
#define ENDP_INIT_DEAGGR(ep)    (0x1034 + 0x80 * (ep))
#define ENDP_INIT_RSRC_GRP(ep)  (0x1038 + 0x80 * (ep))
#define ENDP_INIT_SEQ(ep)       (0x103c + 0x80 * (ep))
#define ENDP_STATUS(ep)         (0x1040 + 0x80 * (ep))

/* The default route as Linux's ipa_endpoint_default_route_set() makes it: everything unmatched, fragments too, to one endpoint */
#define ROUTE_VALUE(ep)         ((uint32_t)(ep) | (uint32_t)(ep) << 8 | 1u << 26 /* header table */ | 1u << 27 /* retain header */)

/* ---- memory the hardware reads and writes ------------------------------------------------------ */

struct dma {
	vm_offset_t     virt;
	uint64_t        phys;
};

/* One page (16 KB), zeroed, written out of the cache; false if there is none. */
static bool
dma_page(struct dma *d)
{
	vm_offset_t virt = 0;

	if (kmem_alloc(kernel_map, &virt, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	d->virt = virt;
	d->phys = kvtophys(virt);
	FlushPoC_DcacheRegion(virt, PAGE_SIZE);
	return true;
}

/* Written back and dropped from the cache: before the hardware reads it, and before we read what the hardware wrote. */
static inline void
dma_sync(vm_offset_t virt, size_t size)
{
	vm_offset_t from = virt & ~63ul;

	FlushPoC_DcacheRegion(from, ((virt + size + 63) & ~63ul) - from);
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ---- the channels ------------------------------------------------------------------------------ */

struct tre {
	uint64_t        addr;
	uint16_t        len_opcode;     /* bytes to transfer, or the immediate command's opcode */
	uint16_t        reserved;
	uint32_t        flags;          /* TRE_* */
};
#define TRE_CHAIN               (1u << 0)
#define TRE_IEOT                (1u << 9)       /* an event when done */
#define TRE_BEI                 (1u << 10)      /* ... but no interrupt for it */
#define TRE_TYPE_XFER           (2u << 16)
#define TRE_TYPE_COMMAND        (3u << 16)

struct event {
	uint64_t        xfer;           /* the address of the TRE that finished */
	uint16_t        len;            /* bytes moved (what a receive buffer got) */
	uint8_t         reserved1;
	uint8_t         code;
	uint16_t        reserved2;
	uint8_t         type;
	uint8_t         chid;
};

struct channel {
	const char *    name;
	uint8_t         id, ring_id, endpoint;  /* the channel, its event ring, the IPA's endpoint */
	uint8_t         group;          /* the endpoint's resource group (ipa_data-v5.2.c: source UL 0; destination UL 0, DL 1) */
	uint8_t         pad_align;      /* receive: packets padded to 1 << this many bytes (HDR_EXT) */
	bool            to_ipa, command, qmap, aggregate, close_eof;
	uint32_t        slots;          /* TREs in the ring, and buffers (a power of two; they fit one page: at most 1024) */
	uint32_t        tlv;            /* the endpoint's TLV FIFO: how many TREs may be outstanding (ipa_data-v5.2.c tlv_count) */
	uint32_t        buffer_size;    /* bytes per slot (a divisor or a multiple of the page size) */
	struct dma      tre_ring, event_ring;
	struct dma *    pages;          /* the buffers, slot by slot */
	uint32_t        page_count;
	uint32_t        next;           /* the next TRE to fill (counts up; the slot is next % slots) */
	uint32_t        done;           /* the next TRE whose completion is awaited (transmit) */
	uint32_t        events;         /* the next event to read (counts up) */
	bool            ring_allocated, allocated, started;     /* what the GSI has been asked for and has not yet been asked to undo */
	uint64_t        packets, bytes, dropped, errors;
};

/*
 * The four: numbers from Linux's data/ipa_data-v5.2.c (channel, endpoint, TLV counts, resource
 * group, which has QMAP and aggregation); Android's driver has the same channel, endpoint and
 * TLV numbers for them (its ipa_utils.c, IPA_5_2: APPS_CMD_PROD, APPS_LAN_CONS, APPS_WAN_PROD,
 * APPS_WAN_CONS). The event rings are 0 to 3, the lowest free ones in this order, as Linux
 * takes them. Rings here are smaller than Linux's; the buffers are a page for a receive slot
 * (the IPA aggregates into it) and 2 KB for a transmit slot (one packet and its header).
 */
#define RECEIVE_BUFFER          16384           /* a page: the IPA aggregates into it */
static struct channel command = { "command", 6, 0, 9, 0, 0, true, true, false, false, false, 16, 20, 64 };
static struct channel lan_rx = { "lan-rx", 7, 1, 11, 0, 2, false, false, false, true, false, 16, 9, RECEIVE_BUFFER };
static struct channel modem_tx = { "modem-tx", 5, 2, 2, 0, 0, true, false, true, false, false, 64, 25, 2048 };
static struct channel modem_rx = { "modem-rx", 9, 3, 18, 1, 0, false, false, true, true, true, 64, 9, RECEIVE_BUFFER };
static struct channel *channels[] = { &command, &lan_rx, &modem_tx, &modem_rx };
#define CHANNELS                4
#define AGGR_LIMIT_KB           12              /* the IPA closes a receive buffer once this much is in it (a page holds another 4 KB packet) */
#define AGGR_TIME_TICKS         5               /* ... or after 5 ticks of the first pulse generator: 500 us (TIMERS_PULSE_GRAN_CFG gran 0 = 100 us) */

static bool ready, trace, route_set;
static uint32_t mem_offset;                     /* the local memory's offset within the IPA's shared memory */
static struct dma table, zero;                  /* the tables' page (below) and a page of zeros */
static char failure[200];                       /* why there is no data path: the first stage that failed */

/* A stage has failed: said in capitals, and kept (the first one) for the modem driver's own log. */
static void failed(const char *format, ...) __printflike(1, 2);

static void
failed(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("ipa: DATA PATH FAILED: %s\n", line);
	if (failure[0] == 0) {
		strlcpy(failure, line, sizeof(failure));
	}
}

static inline vm_offset_t
slot_virt(const struct channel *ch, uint32_t slot)
{
	uint64_t at = (uint64_t)slot * ch->buffer_size;

	return ch->pages[at / PAGE_SIZE].virt + at % PAGE_SIZE;
}

static inline uint64_t
slot_phys(const struct channel *ch, uint32_t slot)
{
	uint64_t at = (uint64_t)slot * ch->buffer_size;

	return ch->pages[at / PAGE_SIZE].phys + at % PAGE_SIZE;
}

static inline struct tre *
tre_at(const struct channel *ch, uint32_t index)
{
	return (struct tre *)(ch->tre_ring.virt + (index % ch->slots) * ELEMENT_SIZE);
}

/* The channel's rings and buffers: made once, and emptied each time the data path is set up. */
static bool
channel_memory(struct channel *ch)
{
	uint64_t bytes = (uint64_t)ch->slots * ch->buffer_size;

	if (ch->pages == NULL) {
		ch->page_count = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
		ch->pages = kalloc_type(struct dma, ch->page_count, Z_WAITOK | Z_ZERO);
	}
	if (ch->pages == NULL || (ch->tre_ring.virt == 0 && !dma_page(&ch->tre_ring)) ||
	    (ch->event_ring.virt == 0 && !dma_page(&ch->event_ring))) {
		return false;
	}
	for (uint32_t p = 0; p < ch->page_count; p++) {
		if (ch->pages[p].virt == 0 && !dma_page(&ch->pages[p])) {
			return false;
		}
	}
	bzero((void *)ch->tre_ring.virt, PAGE_SIZE);
	bzero((void *)ch->event_ring.virt, PAGE_SIZE);
	FlushPoC_DcacheRegion(ch->tre_ring.virt, PAGE_SIZE);
	FlushPoC_DcacheRegion(ch->event_ring.virt, PAGE_SIZE);
	ch->next = ch->done = ch->events = 0;
	ch->ring_allocated = ch->allocated = ch->started = false;
	return true;
}

static void
gsi_error(const char *what)
{
	uint32_t log = gsi_rd(ERROR_LOG);

	/* ERROR_LOG: arguments [11:0], code [15:12], index [23:19], type [27:24], EE [31:28] (the same on GSI 3.0) */
	printf("ipa: gsi: %s: GSI status %#x, error log %#x (code %u, type %u, EE %u, index %u, args %#x), global %#x, general %#x, "
	    "type irq %#x\n", what, gsi_rd(GSI_STATUS), log, (log >> 12) & 0xf, (log >> 24) & 0xf, log >> 28, (log >> 19) & 0x1f, log & 0xfff,
	    gsi_rd(CNTXT_GLOB_IRQ_STTS), gsi_rd(CNTXT_GSI_IRQ_STTS), gsi_rd(CNTXT_TYPE_IRQ));
	if (log != 0) {
		gsi_wr(ERROR_LOG, 0);
		gsi_wr(ERROR_LOG_CLR, ~0u);
	}
}

static inline uint32_t
channel_state(const struct channel *ch)
{
	return (gsi_rd(CH_CNTXT(ch->id, 0)) >> 20) & 0xf;
}

static inline uint32_t
event_ring_state(const struct channel *ch)
{
	return (gsi_rd(EV_CNTXT(ch->ring_id, 0)) >> 20) & 0xf;
}

static const char *
gsi_command_name(bool event, uint32_t opcode)
{
	switch (opcode) {
	case GSI_CH_ALLOCATE:   return "ALLOCATE";      /* (the event rings' opcodes are the channels') */
	case GSI_CH_START:      return event ? "?" : "START";
	case GSI_CH_STOP:       return event ? "?" : "STOP";
	case GSI_CH_RESET:      return "RESET";
	case GSI_CH_DE_ALLOC:   return "DE_ALLOC";
	default:                return "?";
	}
}

/*
 * A command to the GSI about a channel or an event ring; true once the state is `wanted` (within
 * 50 ms, Linux's limit). The state before and after goes to the log either way.
 */
static bool
gsi_command(struct channel *ch, bool event, uint32_t opcode, uint32_t wanted)
{
	uint32_t state = 0, number = event ? ch->ring_id : ch->id;
	uint32_t before = event ? event_ring_state(ch) : channel_state(ch);
	unsigned us;

	gsi_wr(event ? EV_CH_CMD : CH_CMD, number | opcode << 24);
	for (us = 0; us < 50000; us += 100) {
		state = event ? event_ring_state(ch) : channel_state(ch);
		if (state == wanted) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	/* (the completion is also flagged as an interrupt source, masked: cleared) */
	gsi_wr(event ? CNTXT_SRC_EV_CH_IRQ_CLR : CNTXT_SRC_CH_IRQ_CLR, 1u << number);
	printf("ipa: gsi: %s: %s on %s %u: state %u -> %u after %u us (wanted %u; context 0 %#x)%s\n", ch->name,
	    gsi_command_name(event, opcode), event ? "event ring" : "channel", number, before, state, us, wanted,
	    gsi_rd(event ? EV_CNTXT(number, 0) : CH_CNTXT(number, 0)), state == wanted ? "" : ": NOT TAKEN");
	if (state != wanted) {
		gsi_error(ch->name);
		return false;
	}
	return true;
}

/*
 * As Linux's gsi_irq_setup(): interrupts, not MSIs; every source masked (nothing is handled here:
 * the rings are polled). And as its gsi_setup(): the error log emptied.
 */
static void
gsi_interrupts_off(void)
{
	gsi_wr(CNTXT_INTSET, 1);
	gsi_wr(CNTXT_TYPE_IRQ_MSK, 0);
	gsi_wr(CNTXT_SRC_CH_IRQ_MSK, 0);
	gsi_wr(CNTXT_SRC_EV_CH_IRQ_MSK, 0);
	gsi_wr(CNTXT_GLOB_IRQ_EN, 0);
	gsi_wr(CNTXT_SRC_IEOB_IRQ_MSK, 0);
	gsi_wr(INTER_EE_SRC_CH_IRQ_MSK, 0);
	gsi_wr(INTER_EE_SRC_EV_CH_IRQ_MSK, 0);
	gsi_wr(CNTXT_GSI_IRQ_EN, 0);
	gsi_wr(ERROR_LOG, 0);
	printf("ipa: gsi: interrupts off: INTSET %#x, type mask %#x, channel mask %#x, event ring mask %#x, IEOB mask %#x, global %#x, "
	    "general %#x; pending: type %#x channels %#x event rings %#x IEOB %#x\n", gsi_rd(CNTXT_INTSET), gsi_rd(CNTXT_TYPE_IRQ_MSK),
	    gsi_rd(CNTXT_SRC_CH_IRQ_MSK), gsi_rd(CNTXT_SRC_EV_CH_IRQ_MSK), gsi_rd(CNTXT_SRC_IEOB_IRQ_MSK), gsi_rd(CNTXT_GLOB_IRQ_EN),
	    gsi_rd(CNTXT_GSI_IRQ_EN), gsi_rd(CNTXT_TYPE_IRQ), gsi_rd(CNTXT_SRC_CH_IRQ), gsi_rd(CNTXT_SRC_EV_CH_IRQ), gsi_rd(CNTXT_SRC_IEOB_IRQ));
}

/*
 * As Linux's gsi_evt_ring_program() and gsi_channel_program(), for the GPI protocol, in GSI 3.0's
 * layout: the event ring's "interrupt, not MSI" is bit 7 of its context 0 (bit 16 there is now
 * the EE), a channel's direction bit 7 of its context 0, and the channel's event ring is in bits
 * 31:24 of its context 1, beside the ring's length. What the hardware then reads back is shown;
 * a ring's length or address that does not read back as written ends the setup, since the
 * hardware would then be working on memory that is not this ring's.
 */
static bool
channel_setup(struct channel *ch)
{
	uint32_t e = ch->ring_id, c = ch->id, value, length = ch->slots * ELEMENT_SIZE;

	printf("ipa: gsi: %s: before anything: channel %u in state %u, event ring %u in state %u (0: not allocated)\n", ch->name, c,
	    channel_state(ch), e, event_ring_state(ch));
	if (!gsi_command(ch, true, GSI_EVT_ALLOCATE, EV_STATE_ALLOCATED)) {
		failed("the GSI does not allocate event ring %u (%s)", e, ch->name);
		return false;
	}
	ch->ring_allocated = true;
	gsi_wr(EV_CNTXT(e, 0), GSI_CHANNEL_TYPE_GPI | 1u << 7 /* INTYPE: interrupt, not MSI */ | ELEMENT_SIZE << 24);
	gsi_wr(EV_CNTXT(e, 1), length);
	gsi_wr(EV_CNTXT(e, 2), (uint32_t)ch->event_ring.phys);
	gsi_wr(EV_CNTXT(e, 3), (uint32_t)(ch->event_ring.phys >> 32));
	gsi_wr(EV_CNTXT(e, 8), 32 /* moderation: 1 ms at 32 kHz */ | 1u << 16 /* MODC */);
	for (unsigned n = 9; n <= 13; n++) {
		gsi_wr(EV_CNTXT(e, n), 0);              /* no MSI, no read pointer updates */
	}
	/* "the last processed event": the ring's last entry, as nothing has been processed */
	gsi_wr(EV_DOORBELL(e), (uint32_t)(ch->event_ring.phys + (ch->slots - 1) * ELEMENT_SIZE));
	printf("ipa: gsi: %s: event ring %u programmed: context %#x %#x, base %#x:%08x (ours %#llx), write pointer %#x, moderation %#x\n",
	    ch->name, e, gsi_rd(EV_CNTXT(e, 0)), gsi_rd(EV_CNTXT(e, 1)), gsi_rd(EV_CNTXT(e, 3)), gsi_rd(EV_CNTXT(e, 2)), ch->event_ring.phys,
	    gsi_rd(EV_CNTXT(e, 4)), gsi_rd(EV_CNTXT(e, 8)));
	if ((gsi_rd(EV_CNTXT(e, 1)) & 0xffffff) != length || gsi_rd(EV_CNTXT(e, 2)) != (uint32_t)ch->event_ring.phys ||
	    event_ring_state(ch) != EV_STATE_ALLOCATED) {
		failed("event ring %u (%s) does not read back as programmed", e, ch->name);
		return false;
	}

	if (!gsi_command(ch, false, GSI_CH_ALLOCATE, CH_STATE_ALLOCATED)) {
		failed("the GSI does not allocate channel %u (%s)", c, ch->name);
		return false;
	}
	ch->allocated = true;
	gsi_wr(CH_CNTXT(c, 0), GSI_CHANNEL_TYPE_GPI | (ch->to_ipa ? 1u << 7 : 0) | ELEMENT_SIZE << 24);
	gsi_wr(CH_CNTXT(c, 1), length | e << 24);
	gsi_wr(CH_CNTXT(c, 2), (uint32_t)ch->tre_ring.phys);
	gsi_wr(CH_CNTXT(c, 3), (uint32_t)(ch->tre_ring.phys >> 32));
	/* QOS: the command channel gets the lowest round-robin weight and no escape buffer; the doorbell counts bytes */
	value = (ch->command ? 0xf : 0) | (ch->command ? 0 : 1u << 10 /* PREFETCH_MODE: escape buffer only */) | 1u << 24;
	gsi_wr(CH_QOS(c), value);
	/* the GPI scratch: the most outstanding TREs (in bytes), the threshold 2 TREs */
	gsi_wr(CH_SCRATCH(c, 0), (ch->tlv * ELEMENT_SIZE) << 16);
	gsi_wr(CH_SCRATCH(c, 1), (2 * ELEMENT_SIZE) << 16);
	gsi_wr(CH_SCRATCH(c, 2), 0);
	gsi_wr(CH_SCRATCH(c, 3), gsi_rd(CH_SCRATCH(c, 3)) & 0xffff);
	printf("ipa: gsi: %s: channel %u programmed: context %#x %#x, base %#x:%08x (ours %#llx), QOS %#x, scratch %#x %#x %#x %#x\n",
	    ch->name, c, gsi_rd(CH_CNTXT(c, 0)), gsi_rd(CH_CNTXT(c, 1)), gsi_rd(CH_CNTXT(c, 3)), gsi_rd(CH_CNTXT(c, 2)), ch->tre_ring.phys,
	    gsi_rd(CH_QOS(c)), gsi_rd(CH_SCRATCH(c, 0)), gsi_rd(CH_SCRATCH(c, 1)), gsi_rd(CH_SCRATCH(c, 2)), gsi_rd(CH_SCRATCH(c, 3)));
	if (gsi_rd(CH_CNTXT(c, 1)) != (length | e << 24) || gsi_rd(CH_CNTXT(c, 2)) != (uint32_t)ch->tre_ring.phys ||
	    channel_state(ch) != CH_STATE_ALLOCATED) {
		failed("channel %u (%s) does not read back as programmed", c, ch->name);
		return false;
	}
	return true;
}

/*
 * A channel and its event ring given back to the GSI, as Linux's gsi_channel_stop(),
 * gsi_channel_reset() and gsi_channel_teardown_one() do it: stopped (which a channel with work
 * in hand may need to be asked more than once), reset, de-allocated; the event ring reset and
 * de-allocated. Only what this file allocated.
 */
static void
channel_teardown(struct channel *ch)
{
	uint32_t state;

	if (ch->allocated) {
		state = channel_state(ch);
		for (unsigned tries = 0; tries < 10 && (state == CH_STATE_STARTED || state == CH_STATE_STOP_IN_PROC); tries++) {
			if (!gsi_command(ch, false, GSI_CH_STOP, CH_STATE_STOPPED)) {
				delay_for_interval(4, NSEC_PER_MSEC);
			}
			state = channel_state(ch);
		}
		if (state == CH_STATE_STOPPED) {
			delay_for_interval(1, NSEC_PER_MSEC);
			gsi_command(ch, false, GSI_CH_RESET, CH_STATE_ALLOCATED);
			state = channel_state(ch);
		}
		if (state == CH_STATE_ALLOCATED) {
			gsi_command(ch, false, GSI_CH_DE_ALLOC, CH_STATE_NOT_ALLOCATED);
		} else {
			printf("ipa: gsi: %s: CHANNEL %u IS LEFT IN STATE %u: it cannot be given back\n", ch->name, ch->id, state);
		}
		ch->allocated = false;
	}
	ch->started = false;
	if (ch->ring_allocated) {
		if (channel_state(ch) == CH_STATE_NOT_ALLOCATED && gsi_command(ch, true, GSI_EVT_RESET, EV_STATE_ALLOCATED)) {
			/* (a reset leaves the state as it was, so there is nothing to see it finish by: it is given 2 ms) */
			delay_for_interval(2, NSEC_PER_MSEC);
			gsi_command(ch, true, GSI_EVT_DE_ALLOC, EV_STATE_NOT_ALLOCATED);
		} else {
			printf("ipa: gsi: %s: EVENT RING %u IS LEFT ALLOCATED (its channel is in state %u)\n", ch->name, ch->ring_id,
			    channel_state(ch));
		}
		ch->ring_allocated = false;
	}
}

/*
 * As Linux's ipa_endpoint_program() for IPA 5.0 and later, with no checksum offload and no
 * status. The registers' fields are those of 4.11 but for the wider endpoint numbers (the DMA
 * destination in MODE is bits 11:4) and the resource group (bits 2:0). What reads back goes to
 * the log.
 */
static void
endpoint_program(const struct channel *ch)
{
	uint32_t ep = ch->endpoint, value;

	ipa_wr(ENDP_INIT_CFG(ep), 0);                   /* no fragment or checksum offload */
	if (ch->to_ipa) {
		ipa_wr(ENDP_INIT_NAT(ep), 0);           /* NAT bypassed */
	}
	value = 0;
	if (ch->qmap) {
		value = 4 /* HDR_LEN */ | 1u << 6 /* HDR_OFST_METADATA_VALID, at 0 */;
		if (!ch->to_ipa) {
			/* where the IPA writes the mux id (byte 1) and the packet's length (bytes 2-3) of the header it makes */
			value |= 1u << 7 /* HDR_OFST_METADATA */ | 1u << 19 /* HDR_OFST_PKT_SIZE_VALID */ | 2u << 20 /* HDR_OFST_PKT_SIZE */;
		}
	}
	ipa_wr(ENDP_INIT_HDR(ep), value);
	value = 0;
	if (ch->qmap) {
		value = 1 /* HDR_ENDIANNESS: big */;
		if (!ch->to_ipa) {
			value |= 1u << 1 /* HDR_TOTAL_LEN_OR_PAD_VALID (pad) */ | 1u << 3 /* HDR_PAYLOAD_LEN_INC_PADDING */;
		}
	}
	if (!ch->to_ipa) {
		value |= (uint32_t)ch->pad_align << 10; /* HDR_PAD_TO_ALIGNMENT */
	}
	ipa_wr(ENDP_INIT_HDR_EXT(ep), value);
	if (!ch->to_ipa) {
		ipa_wr(ENDP_INIT_HDR_METADATA_MASK(ep), ch->qmap ? 0xff000000 : 0);   /* the metadata's low byte, big-endian */
	} else {
		/* the command endpoint moves its data to the LAN receive endpoint (DMA mode); the rest is basic */
		ipa_wr(ENDP_INIT_MODE(ep), ch->command ? 3 | (uint32_t)lan_rx.endpoint << 4 : 0);
	}
	if (!ch->to_ipa && ch->aggregate) {
		value = 1 /* AGGR_EN: aggregate */ | 4u << 2 /* AGGR_TYPE: generic */ | AGGR_LIMIT_KB << 5 | AGGR_TIME_TICKS << 12 |
		    (ch->close_eof ? 1u << 23 /* SW_EOF_ACTIVE */ : 0);
	} else if (ch->to_ipa && ch->qmap) {
		value = 2 /* AGGR_EN: deaggregate */ | 6u << 2 /* AGGR_TYPE: QCMAP */;
	} else {
		value = 0;
	}
	ipa_wr(ENDP_INIT_AGGR(ep), value);
	if (!ch->to_ipa) {
		ipa_wr(ENDP_INIT_HOL_BLOCK_EN(ep), 0);  /* no head-of-line blocking drops */
	} else {
		ipa_wr(ENDP_INIT_DEAGGR(ep), 0);
	}
	ipa_wr(ENDP_INIT_RSRC_GRP(ep), ch->group);
	if (ch->to_ipa) {
		ipa_wr(ENDP_INIT_SEQ(ep), ch->command ? 0 /* DMA */ : 4 /* 2_PASS_SKIP_LAST_UC */);
	}
	ipa_wr(ENDP_STATUS(ep), 0);                     /* no status packets */
	printf("ipa: endpoint %u (%s, %s) programmed: CFG %#x HDR %#x HDR_EXT %#x %s %#x AGGR %#x RSRC_GRP %#x SEQ %#x STATUS %#x\n", ep,
	    ch->name, ch->to_ipa ? "to the IPA" : "from the IPA", ipa_rd(ENDP_INIT_CFG(ep)), ipa_rd(ENDP_INIT_HDR(ep)),
	    ipa_rd(ENDP_INIT_HDR_EXT(ep)), ch->to_ipa ? "MODE" : "METADATA_MASK",
	    ipa_rd(ch->to_ipa ? ENDP_INIT_MODE(ep) : ENDP_INIT_HDR_METADATA_MASK(ep)), ipa_rd(ENDP_INIT_AGGR(ep)),
	    ipa_rd(ENDP_INIT_RSRC_GRP(ep)), ipa_rd(ENDP_INIT_SEQ(ep)), ipa_rd(ENDP_STATUS(ep)));
}

/* The TRE at `index` of the channel's ring, filled and written out; the doorbell is rung separately. */
static void
tre_fill(struct channel *ch, uint32_t index, uint64_t addr, uint16_t len_opcode, uint32_t flags)
{
	struct tre *t = tre_at(ch, index);

	t->addr = addr;
	t->len_opcode = len_opcode;
	t->reserved = 0;
	t->flags = flags;
	dma_sync((vm_offset_t)t, sizeof(*t));
}

static inline void
channel_doorbell(struct channel *ch)
{
	gsi_wr(CH_DOORBELL(ch->id), (uint32_t)(ch->tre_ring.phys + (ch->next % ch->slots) * ELEMENT_SIZE));
}

/*
 * The events the hardware has written since last time, each to `each`. The event ring's write
 * pointer is in its context register 4; the doorbell afterwards tells the hardware the last event
 * read, which makes room.
 */
static unsigned
channel_events(struct channel *ch, void (*each)(struct channel *, const struct event *, void *), void *context)
{
	uint32_t wp = gsi_rd(EV_CNTXT(ch->ring_id, 4)), base = (uint32_t)ch->event_ring.phys, index;
	unsigned n = 0;

	if (wp < base || wp >= base + ch->slots * ELEMENT_SIZE) {
		if (ch->errors++ == 0) {
			printf("ipa: %s: THE EVENT RING'S WRITE POINTER %#x IS OUTSIDE THE RING at %#x\n", ch->name, wp, base);
		}
		return 0;
	}
	index = (wp - base) / ELEMENT_SIZE;
	while (ch->events % ch->slots != index) {
		struct event *e = (struct event *)(ch->event_ring.virt + (ch->events % ch->slots) * ELEMENT_SIZE);

		dma_sync((vm_offset_t)e, sizeof(*e));
		each(ch, e, context);
		ch->events++;
		n++;
	}
	if (n != 0) {
		gsi_wr(EV_DOORBELL(ch->ring_id), (uint32_t)(ch->event_ring.phys + ((ch->events - 1) % ch->slots) * ELEMENT_SIZE));
	}
	return n;
}

/* A channel started; its state afterwards is in the log (gsi_command()). A receive channel gets its buffers. */
static bool
channel_start(struct channel *ch)
{
	if (!gsi_command(ch, false, GSI_CH_START, CH_STATE_STARTED)) {
		failed("channel %u (%s) does not start", ch->id, ch->name);
		return false;
	}
	ch->started = true;
	if (!ch->to_ipa) {
		/* one short of the ring: the doorbell is the write pointer, and writing it level with
		 * the read pointer (base) would mean "empty", not "full" */
		for (uint32_t slot = 0; slot + 1 < ch->slots; slot++) {
			tre_fill(ch, ch->next, slot_phys(ch, slot), (uint16_t)(ch->buffer_size > 0xffff ? 0xffff : ch->buffer_size),
			    TRE_TYPE_XFER | TRE_IEOT);
			ch->next++;
		}
		channel_doorbell(ch);
		printf("ipa: gsi: %s: %u receive buffers of %u bytes handed over (doorbell %#x)\n", ch->name, ch->slots - 1, ch->buffer_size,
		    (uint32_t)(ch->tre_ring.phys + (ch->next % ch->slots) * ELEMENT_SIZE));
	}
	return true;
}

/* ---- immediate commands ------------------------------------------------------------------------- */

#define IPA_CMD_IP_V4_FILTER_INIT 0x3
#define IPA_CMD_IP_V6_FILTER_INIT 0x4
#define IPA_CMD_IP_V4_ROUTING_INIT 0x7
#define IPA_CMD_IP_V6_ROUTING_INIT 0x8
#define IPA_CMD_HDR_INIT_LOCAL  0x9
#define IPA_CMD_DMA_SHARED_MEM  0x13

struct command_done {
	uint64_t        tre;
	bool            done;
	uint8_t         code;
};

static void
command_event(__unused struct channel *ch, const struct event *e, void *context)
{
	struct command_done *waiting = context;

	if (e->xfer == waiting->tre) {
		waiting->done = true;
		waiting->code = e->code;
	}
	command.done++;
}

/*
 * One immediate command, waited for (up to 200 ms). The payload is copied into the slot's buffer.
 * How it ended goes to the log: the event's code is the GSI's (Android's gsi.h: 1 success, 2 end
 * of transfer; anything else is said in capitals but not taken for a failure, since Linux does
 * not look at it).
 */
static bool
ipa_command(const char *what, uint8_t opcode, const void *payload, size_t size)
{
	uint32_t slot = command.next % command.slots;
	struct command_done waiting = { .tre = command.tre_ring.phys + slot * ELEMENT_SIZE };
	unsigned us;

	if (size > command.buffer_size || command.next - command.done >= command.slots - 1) {
		failed("the command %s (opcode %#x) cannot be queued (%u outstanding)", what, opcode, command.next - command.done);
		return false;
	}
	memcpy((void *)slot_virt(&command, slot), payload, size);
	dma_sync(slot_virt(&command, slot), size);
	tre_fill(&command, command.next, slot_phys(&command, slot), opcode, TRE_TYPE_COMMAND | TRE_IEOT);
	command.next++;
	channel_doorbell(&command);
	for (us = 0; us < 200000 && !waiting.done; us += 200) {
		channel_events(&command, command_event, &waiting);
		if (!waiting.done) {
			delay_for_interval(200, NSEC_PER_USEC);
		}
	}
	if (!waiting.done) {
		printf("ipa: command: %s (opcode %#x): NOT ANSWERED IN 200 ms; channel state %u, event ring write pointer %#x (ring at %#llx)\n",
		    what, opcode, channel_state(&command), gsi_rd(EV_CNTXT(command.ring_id, 4)), command.event_ring.phys);
		gsi_error(what);
		failed("the IPA does not answer the command %s (opcode %#x)", what, opcode);
		return false;
	}
	printf("ipa: command: %s (opcode %#x): done after %u us, event code %u%s\n", what, opcode, us, waiting.code,
	    waiting.code == 1 || waiting.code == 2 ? "" : " (AN UNEXPECTED CODE)");
	return true;
}

/*
 * The IPA's local memory as IPA 5.2 lays it out (Linux: data/ipa_data-v5.2.c, ipa_mem_local_data;
 * Android's ipa_5_2 memory table has the same numbers), offsets from the start of the local memory
 * (SHARED_MEM_SIZE says where that is). The route tables have 19 entries (0x98 bytes: the modem's
 * are 0 to 10), the filter tables 15 slots (0x78 bytes).
 */
#define MEM_V4_FILTER_HASHED    0x0288
#define MEM_V4_FILTER           0x0308
#define MEM_V6_FILTER_HASHED    0x0388
#define MEM_V6_FILTER           0x0408
#define MEM_FILTER_SIZE         0x0078
#define MEM_V4_ROUTE_HASHED     0x0488
#define MEM_V4_ROUTE            0x0528
#define MEM_V6_ROUTE_HASHED     0x05c8
#define MEM_V6_ROUTE            0x0668
#define MEM_HEADERS             0x0708          /* the modem's headers (0x240 bytes), then ours (0x1e0) */
#define MEM_HEADERS_SIZE        (0x0240 + 0x01e0)
#define MEM_MODEM_PROC_CTX      0x0b40
#define MEM_MODEM_PROC_CTX_SIZE 0x0b20
#define MEM_AP_PROC_CTX         0x1660
#define MEM_AP_PROC_CTX_SIZE    0x0200
#define MEM_MODEM               0x2898
#define MEM_MODEM_SIZE          0x0d48
#define MEM_END                 0x3fe8          /* the end of the last region (PDN_CONFIG) */
#define IPA_SRAM_SIZE           0x5000          /* what there is of the local memory (Linux's dt: "ipa-shared", 0x5000 bytes) */

/*
 * The tables, as Linux's ipa_table_init() lays them out in one buffer: a "zero rule" (8 bytes of
 * zero, which every table entry points at until somebody installs a rule), then the filter
 * table's bitmap of the endpoints that filter -- from IPA 5.0 bit n for endpoint n, with no
 * "global" bit 0 before them: 2, 7 and 10, our modem transmit endpoint and two of the modem's --
 * then as many entries as the longest table (19: the route tables' size, 0x98 bytes), each the
 * zero rule's address. The filter table is the bitmap and 3 entries from offset 8; a route table
 * the 19 entries from offset 16.
 */
#define FILTERED_ENDPOINTS      ((1ull << 2) | (1ull << 7) | (1ull << 10))
#define FILTER_ENTRIES          4               /* the bitmap and one per filtering endpoint */
#define ROUTE_ENTRIES           19

/* A stretch of the IPA's local memory zeroed by the IPA itself, from our page of zeros (Linux: ipa_cmd_dma_shared_mem_add()). */
static bool
memory_zero(const char *what, uint32_t offset, uint32_t size)
{
	struct {
		uint16_t clear_after_read, size, local_addr, flags;     /* flags bit 0: 1 would read from the IPA */
		uint64_t system_addr;
	} payload;

	if (size == 0 || size > PAGE_SIZE || mem_offset + offset + size > 0xffff) {
		failed("%s: %#x bytes at %#x of the local memory is not something a command can zero", what, size, mem_offset + offset);
		return false;
	}
	payload.clear_after_read = 0;
	payload.size = (uint16_t)size;
	payload.local_addr = (uint16_t)(mem_offset + offset);
	payload.flags = 0;
	payload.system_addr = zero.phys;
	return ipa_command(what, IPA_CMD_DMA_SHARED_MEM, &payload, sizeof(payload));
}

/*
 * As Linux's ipa_mem_setup(): the header table (the modem's and ours, one stretch) defined and
 * filled from a buffer of zeros, and the modem's processing contexts, ours and the modem's own
 * region zeroed. (The register that says where the processing contexts are is the modem
 * driver's to write: hakios.mpss.c, ipa_memory_init().)
 */
static bool
memory_setup(void)
{
	struct {
		uint64_t hdr_table_addr;
		uint32_t flags, reserved;                       /* flags: the size [11:0], where [27:12] */
	} headers;

	headers.hdr_table_addr = zero.phys;
	headers.flags = MEM_HEADERS_SIZE | (mem_offset + MEM_HEADERS) << 12;
	headers.reserved = 0;
	return ipa_command("header table init", IPA_CMD_HDR_INIT_LOCAL, &headers, sizeof(headers)) &&
	       memory_zero("zero the modem's processing contexts", MEM_MODEM_PROC_CTX, MEM_MODEM_PROC_CTX_SIZE) &&
	       memory_zero("zero our processing contexts", MEM_AP_PROC_CTX, MEM_AP_PROC_CTX_SIZE) &&
	       memory_zero("zero the modem's region", MEM_MODEM, MEM_MODEM_SIZE);
}

/*
 * One table pair (hashed and not) told to the IPA, as Linux's ipa_table_init_add(): the same
 * source for both; for a filter table the slots after the first four are then zeroed.
 */
static bool
table_init(const char *what, uint8_t opcode, bool filter, uint32_t hashed_offset, uint32_t offset)
{
	struct {
		uint64_t hash_rules_addr, flags, nhash_rules_addr;
	} payload;
	uint32_t entries = filter ? FILTER_ENTRIES : ROUTE_ENTRIES, size = entries * 8;
	uint64_t source = table.phys + (filter ? 8 : 16);

	payload.hash_rules_addr = source;
	payload.nhash_rules_addr = source;
	/* flags: the hashed table's size [11:0] and offset [27:12], the other's size [39:28] and offset [55:40] */
	payload.flags = (uint64_t)size | (uint64_t)(mem_offset + hashed_offset) << 12 | (uint64_t)size << 28 |
	    (uint64_t)(mem_offset + offset) << 40;
	if (!ipa_command(what, opcode, &payload, sizeof(payload))) {
		return false;
	}
	if (!filter) {
		return true;
	}
	return memory_zero("zero the rest of that filter table", offset + size, MEM_FILTER_SIZE - size) &&
	       memory_zero("zero the rest of its hashed twin", hashed_offset + size, MEM_FILTER_SIZE - size);
}

static bool
tables_setup(void)
{
	uint64_t *t = (uint64_t *)table.virt;

	t[0] = 0;                                       /* the zero rule */
	t[1] = FILTERED_ENDPOINTS;                      /* the filter bitmap */
	for (unsigned n = 0; n < ROUTE_ENTRIES; n++) {
		t[2 + n] = table.phys;
	}
	dma_sync(table.virt, (2 + ROUTE_ENTRIES) * 8);
	return table_init("IPv4 route table init", IPA_CMD_IP_V4_ROUTING_INIT, false, MEM_V4_ROUTE_HASHED, MEM_V4_ROUTE) &&
	       table_init("IPv6 route table init", IPA_CMD_IP_V6_ROUTING_INIT, false, MEM_V6_ROUTE_HASHED, MEM_V6_ROUTE) &&
	       table_init("IPv4 filter table init", IPA_CMD_IP_V4_FILTER_INIT, true, MEM_V4_FILTER_HASHED, MEM_V4_FILTER) &&
	       table_init("IPv6 filter table init", IPA_CMD_IP_V6_FILTER_INIT, true, MEM_V6_FILTER_HASHED, MEM_V6_FILTER);
}

/* ---- receiving and sending ---------------------------------------------------------------------- */

/* A receive slot handed to the hardware (one TRE for the whole buffer). */
static void
receive_post(struct channel *ch, uint32_t slot)
{
	tre_fill(ch, ch->next, slot_phys(ch, slot), (uint16_t)(ch->buffer_size > 0xffff ? 0xffff : ch->buffer_size), TRE_TYPE_XFER | TRE_IEOT);
	ch->next++;
}

struct receive_context {
	void            (*receive)(uint8_t mux, const uint8_t *packet, size_t length);
	unsigned        packets;
};

/* A receive buffer is full (or closed): the QMAP packets in it, one by one; then the slot is posted again. */
static void
receive_event(struct channel *ch, const struct event *e, void *context)
{
	struct receive_context *rc = context;
	uint32_t slot = (uint32_t)((e->xfer - ch->tre_ring.phys) / ELEMENT_SIZE), length = e->len;
	const uint8_t *buffer;

	if (slot >= ch->slots || e->xfer < ch->tre_ring.phys) {
		if (ch->errors++ < 4) {
			printf("ipa: %s: an event for TRE %#llx, outside the ring at %#llx\n", ch->name, e->xfer, ch->tre_ring.phys);
		}
		return;
	}
	/* receives complete in order; ch->done is the next expected slot, ch->next the next to post (slots-1 ahead) */
	if (slot != ch->done % ch->slots) {
		if (ch->errors++ < 4) {
			printf("ipa: %s: slot %u finished while slot %u was expected\n", ch->name, slot, ch->done % ch->slots);
		}
	}
	ch->done++;
	buffer = (const uint8_t *)slot_virt(ch, slot);
	if (length > ch->buffer_size) {
		length = ch->buffer_size;
	}
	dma_sync((vm_offset_t)buffer, length);
	ch->bytes += length;
	if (trace || (!ch->qmap && ch->packets < 4)) {
		/* (the first few buffers of the LAN endpoint are shown without being asked: nothing is expected there) */
		printf("ipa: %s: slot %u: %u bytes (code %u): %02x %02x %02x %02x %02x %02x %02x %02x ...\n", ch->name, slot, length, e->code,
		    buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]);
	}
	if (ch->qmap) {
		for (uint32_t at = 0; at + 4 <= length;) {
			uint32_t pad = buffer[at] & 0x3f, cmd = buffer[at] & 0x80, mux = buffer[at + 1];
			uint32_t packet = (uint32_t)buffer[at + 2] << 8 | buffer[at + 3];

			if (packet == 0 || at + 4 + packet > length || pad > packet) {
				break;
			}
			if (cmd == 0) {
				ch->packets++;
				rc->packets++;
				if (rc->receive != NULL) {
					rc->receive((uint8_t)mux, buffer + at + 4, packet - pad);
				}
			} else if (trace) {
				printf("ipa: %s: a QMAP command (%u bytes) on mux %u\n", ch->name, packet, mux);
			}
			at += 4 + packet;
		}
	} else {
		ch->packets++;                          /* the LAN endpoint: looked at, dropped */
	}
	/* the slot posted again -- the one at the ring's next TRE, which is this one while the hardware finishes in order */
	receive_post(ch, ch->next % ch->slots);
	channel_doorbell(ch);
}

static void
transmit_event(struct channel *ch, const struct event *e, __unused void *context)
{
	uint32_t slot = (uint32_t)((e->xfer - ch->tre_ring.phys) / ELEMENT_SIZE);

	if (slot != ch->done % ch->slots && ch->errors++ < 4) {
		printf("ipa: %s: slot %u finished while slot %u was expected (code %u)\n", ch->name, slot, ch->done % ch->slots, e->code);
	}
	ch->done++;
}

bool
hakios_ipa_transmit(uint8_t mux, const void *packet, size_t length)
{
	struct channel *ch = &modem_tx;
	uint32_t slot;
	uint8_t *buffer;

	if (!ready || length == 0 || length + 4 > ch->buffer_size) {
		return false;
	}
	lck_mtx_lock(&ipa_lock);
	if (!ready) {                                   /* (taken down while this waited for the lock) */
		lck_mtx_unlock(&ipa_lock);
		return false;
	}
	channel_events(ch, transmit_event, NULL);
	if (ch->next - ch->done >= ch->slots - 1) {     /* one slot always left free: write==read means empty */
		ch->dropped++;
		lck_mtx_unlock(&ipa_lock);
		return false;
	}
	slot = ch->next % ch->slots;
	buffer = (uint8_t *)slot_virt(ch, slot);
	buffer[0] = 0;                                  /* no pad, not a command */
	buffer[1] = mux;
	buffer[2] = (uint8_t)(length >> 8);
	buffer[3] = (uint8_t)length;
	memcpy(buffer + 4, packet, length);
	dma_sync((vm_offset_t)buffer, length + 4);
	tre_fill(ch, ch->next, slot_phys(ch, slot), (uint16_t)(length + 4), TRE_TYPE_XFER | TRE_IEOT | TRE_BEI);
	ch->next++;
	channel_doorbell(ch);
	ch->packets++;
	ch->bytes += length;
	if (trace) {
		printf("ipa: %s: slot %u: %zu bytes on mux %u\n", ch->name, slot, length, mux);
	}
	lck_mtx_unlock(&ipa_lock);
	return true;
}

unsigned
hakios_ipa_poll(void (*receive)(uint8_t mux, const uint8_t *packet, size_t length))
{
	struct receive_context rc = { receive, 0 };

	if (!ready) {
		return 0;
	}
	lck_mtx_lock(&ipa_lock);
	if (ready) {                                    /* (not taken down while this waited for the lock) */
		channel_events(&modem_rx, receive_event, &rc);
		channel_events(&lan_rx, receive_event, &rc);
		channel_events(&modem_tx, transmit_event, NULL);
	}
	lck_mtx_unlock(&ipa_lock);
	return rc.packets;
}

void
hakios_ipa_modem_endpoints(uint32_t *to_modem, uint32_t *from_modem)
{
	*to_modem = modem_tx.endpoint;          /* 2: what Linux exports as modem/tx_endpoint_id */
	*from_modem = modem_rx.endpoint;        /* 18: modem/rx_endpoint_id */
}

bool
hakios_ipa_data_ready(void)
{
	return ready;
}

const char *
hakios_ipa_data_failure(void)
{
	return failure;
}

/* The counts, and the channels' states if the registers may be read (`live`: the data path is up, or being set up). */
static void
status_line(char *to, size_t size, bool live)
{
	uint32_t states[CHANNELS] = { 0 };

	for (unsigned n = 0; n < CHANNELS && live; n++) {
		states[n] = channel_state(channels[n]);
	}
	snprintf(to, size, "to the modem %llu packets (%llu bytes, %llu dropped), from it %llu (%llu bytes), %llu to the LAN endpoint; "
	    "errors %llu/%llu/%llu; channels %u/%u/%u/%u%s", modem_tx.packets, modem_tx.bytes, modem_tx.dropped, modem_rx.packets, modem_rx.bytes,
	    lan_rx.packets, modem_tx.errors, modem_rx.errors, lan_rx.errors, states[0], states[1], states[2], states[3],
	    live ? "" : " (down: not read)");
}

void
hakios_ipa_status(char *to, size_t size)
{
	lck_mtx_lock(&ipa_lock);
	status_line(to, size, ready);
	lck_mtx_unlock(&ipa_lock);
}

/* ---- setting up, and taking down ------------------------------------------------------------------ */

/*
 * What was set up, undone: the default route off our endpoint (as Linux's
 * ipa_endpoint_default_route_clear()), the channels and event rings given back to the GSI. The
 * IPA and the GSI must still be clocked and their firmware running.
 */
static void
data_teardown(const char *why)
{
	printf("ipa: taking this processor's data path down (%s)\n", why);
	if (route_set) {
		ipa_wr(IPA_ROUTE, ROUTE_VALUE(0));
		route_set = false;
	}
	for (unsigned n = CHANNELS; n-- > 0;) {
		channel_teardown(channels[n]);
	}
	printf("ipa: the data path is down: channel states %u/%u/%u/%u, event ring states %u/%u/%u/%u (0: given back), route %#x\n",
	    channel_state(&command), channel_state(&lan_rx), channel_state(&modem_tx), channel_state(&modem_rx),
	    event_ring_state(&command), event_ring_state(&lan_rx), event_ring_state(&modem_tx), event_ring_state(&modem_rx),
	    ipa_rd(IPA_ROUTE));
}

void
hakios_ipa_data_stop(void)
{
	bool was;

	lck_mtx_lock(&ipa_lock);
	was = ready;
	ready = false;
	if (was) {
		data_teardown("the session ends");
	}
	lck_mtx_unlock(&ipa_lock);
}

/*
 * What the hardware says of itself before anything is written, as Linux checks it (gsi_setup(),
 * gsi_ring_setup(), ipa_endpoint_config(), ipa_mem_config()): the GSI enabled, enough channels
 * and event rings for the numbers above, our endpoints within what FLAVOR_0 says exists and each
 * on its side of it (ours that send are "consumers" of the IPA, below MAX_CONS_PIPES; ours that
 * receive "producers", from PROD_LOWEST), and the local memory large enough for the 5.2 layout.
 */
static bool
hardware_check(void)
{
	uint32_t status = gsi_rd(GSI_STATUS), param2 = gsi_rd(GSI_HW_PARAM_2), param4 = gsi_rd(GSI_HW_PARAM_4);
	uint32_t flavor = ipa_rd(IPA_FLAVOR_0), shared = ipa_rd(IPA_SHARED_MEM_SIZE);
	uint32_t gsi_channels = param2 & 0xff, gsi_rings = param4 & 0xff;
	uint32_t tx_count = (flavor >> 8) & 0xff, rx_count = (flavor >> 16) & 0xff, rx_base = flavor >> 24;
	uint32_t mem_size = 8 * (shared & 0xffff), limit;
	bool ok = true;

	mem_offset = 8 * (shared >> 16);
	printf("ipa: hardware: GSI_STATUS %#x (%s), GSI software version %#x, HW_PARAM_2 %#x (%u channels per EE), HW_PARAM_4 %#x (%u event "
	    "rings per EE)\n", status, status & 1 ? "enabled" : "NOT ENABLED", gsi_rd(GSI_SW_VERSION), param2, gsi_channels, param4, gsi_rings);
	printf("ipa: hardware: IPA version %#x, component version %#x, FLAVOR_0 %#x (%u pipes: %u that we send on from 0, %u that we "
	    "receive on from %u), SHARED_MEM_SIZE %#x (local memory at %#x, %#x bytes), route %#x\n", ipa_rd(IPA_VERSION),
	    ipa_rd(IPA_COMP_HW_VERSION), flavor, flavor & 0xff, tx_count, rx_count, rx_base, shared, mem_offset, mem_size, ipa_rd(IPA_ROUTE));
	if ((status & 1) == 0) {
		failed("the GSI is not enabled (status %#x)", status);
		return false;
	}
	for (unsigned n = 0; n < CHANNELS; n++) {
		const struct channel *ch = channels[n];

		if (ch->id >= gsi_channels || ch->ring_id >= gsi_rings) {
			failed("the GSI has %u channels and %u event rings per EE: none numbered %u and %u (%s)", gsi_channels, gsi_rings,
			    ch->id, ch->ring_id, ch->name);
			ok = false;
		}
		if (ch->to_ipa ? ch->endpoint >= tx_count : (ch->endpoint < rx_base || ch->endpoint >= rx_base + rx_count)) {
			failed("endpoint %u (%s) is not one the IPA has for %s (FLAVOR_0 %#x)", ch->endpoint, ch->name,
			    ch->to_ipa ? "sending to it" : "receiving from it", flavor);
			ok = false;
		}
	}
	/* (Linux's ipa_mem_config(): the smaller of what the register says and what is mapped) */
	limit = mem_offset + mem_size < IPA_SRAM_SIZE ? mem_size : IPA_SRAM_SIZE;
	if (MEM_END > limit || mem_offset + MEM_END > IPA_SRAM_SIZE) {
		failed("the IPA's local memory (SHARED_MEM_SIZE %#x: %#x bytes at %#x) does not hold the 5.2 layout (%#x bytes)", shared,
		    mem_size, mem_offset, MEM_END);
		ok = false;
	}
	return ok;
}

bool
hakios_ipa_data_setup(void)
{
	char line[256];

	if (ready) {
		return true;
	}
	failure[0] = 0;
	{
		uint32_t off = 0;

		if (PE_parse_boot_argn("modem_ipa_data_off", &off, sizeof(off)) && off != 0) {
			printf("ipa: no data path (modem_ipa_data_off)\n");
			strlcpy(failure, "modem_ipa_data_off", sizeof(failure));
			return false;
		}
		PE_parse_boot_argn("modem_ipa_data_trace", &off, sizeof(off));
		trace = off != 0;
	}
	if (PAGE_SIZE != RECEIVE_BUFFER) {
		failed("pages of %lu bytes, not %u", (unsigned long)PAGE_SIZE, RECEIVE_BUFFER);
		return false;
	}
	if (ipa_regs == 0) {
		PE_parse_boot_argn("soc_ipa_base", &ipa_regs_phys, sizeof(ipa_regs_phys));
		PE_parse_boot_argn("soc_gsi_base", &gsi_phys, sizeof(gsi_phys));
		ipa_regs = ml_io_map(ipa_regs_phys, IPA_REGS_SIZE);
		gsi_regs = ml_io_map(gsi_phys + GSI_FROM, GSI_SIZE);
	}
	if (ipa_regs == 0 || gsi_regs == 0) {
		failed("the registers cannot be mapped");
		return false;
	}
	printf("ipa: setting up this processor's data path for IPA 5.2 / GSI 3.0: IPA registers at %#llx (%#x bytes), GSI at %#llx (its "
	    "%#x..%#x)\n", ipa_regs_phys, IPA_REGS_SIZE, gsi_phys, GSI_FROM, GSI_FROM + GSI_SIZE);
	if (!hardware_check()) {
		return false;
	}
	if ((table.virt == 0 && !dma_page(&table)) || (zero.virt == 0 && !dma_page(&zero))) {
		failed("no memory for the tables");
		return false;
	}
	for (unsigned n = 0; n < CHANNELS; n++) {
		if (!channel_memory(channels[n])) {
			failed("no memory for the %s channel", channels[n]->name);
			return false;
		}
	}
	printf("ipa: rings and buffers in DDR from %#llx (the tables at %#llx, the zeros at %#llx)\n", modem_rx.pages[0].phys, table.phys,
	    zero.phys);

	/* Linux's gsi_setup(): interrupts off, then every event ring and channel allocated and programmed */
	gsi_interrupts_off();
	for (unsigned n = 0; n < CHANNELS; n++) {
		if (!channel_setup(channels[n])) {
			goto undo;
		}
	}
	/* ipa_endpoint_setup(): the four endpoints */
	for (unsigned n = 0; n < CHANNELS; n++) {
		endpoint_program(channels[n]);
	}
	/* the command channel, and what needs it: ipa_mem_setup() and ipa_table_setup() */
	if (!channel_start(&command)) {
		goto undo;
	}
	if (!memory_setup()) {
		failed("the IPA does not take its header table and the zeroing of its regions");
		goto undo;
	}
	if (!tables_setup()) {
		failed("the IPA does not take its tables");
		goto undo;
	}
	printf("ipa: the header table and the four tables are set (local memory at %#x: routes %#x/%#x %#x/%#x, filters %#x/%#x %#x/%#x, "
	    "headers %#x, filter bitmap %#llx)\n", mem_offset, MEM_V4_ROUTE_HASHED, MEM_V4_ROUTE, MEM_V6_ROUTE_HASHED, MEM_V6_ROUTE,
	    MEM_V4_FILTER_HASHED, MEM_V4_FILTER, MEM_V6_FILTER_HASHED, MEM_V6_FILTER, MEM_HEADERS, FILTERED_ENDPOINTS);
	/* the exception endpoint, and the default route to it */
	if (!channel_start(&lan_rx)) {
		goto undo;
	}
	ipa_wr(IPA_ROUTE, ROUTE_VALUE(lan_rx.endpoint));
	route_set = true;
	printf("ipa: the default route: ROUTE %#x (wanted %#x: endpoint %u)\n", ipa_rd(IPA_ROUTE), ROUTE_VALUE(lan_rx.endpoint),
	    lan_rx.endpoint);
	if (ipa_rd(IPA_ROUTE) != ROUTE_VALUE(lan_rx.endpoint)) {
		failed("the default route does not read back as written (%#x, not %#x)", ipa_rd(IPA_ROUTE), ROUTE_VALUE(lan_rx.endpoint));
		goto undo;
	}
	/* the modem's two (Linux starts them when its network device is opened; here they are open from now on) */
	if (!channel_start(&modem_tx) || !channel_start(&modem_rx)) {
		goto undo;
	}
	gsi_error("after the setup");
	lck_mtx_lock(&ipa_lock);
	ready = true;
	lck_mtx_unlock(&ipa_lock);
	status_line(line, sizeof(line), true);
	printf("ipa: THE DATA PATH IS UP: endpoints %u (commands), %u (LAN in), %u (to the modem), %u (from the modem); the tables set; %s\n",
	    command.endpoint, lan_rx.endpoint, modem_tx.endpoint, modem_rx.endpoint, line);
	return true;
undo:
	data_teardown("a stage of the setup failed");
	return false;
}
