/*
 * Infineon AURIX TC4Dx SoC: six TriCore 1.8 cores, their scratchpad
 * RAMs, the LMU and the flashes, and the on-chip peripherals on top of a
 * sparse SFR register file.
 *
 * Sources, all public:
 *   - register addresses, bit fields and access types: the iLLD SFR
 *     headers (Libraries/Infra/Sfr/TC4DA/_Reg) bundled with the TC4D7
 *     projects of https://github.com/Infineon/AURIX_code_examples;
 *   - the status the model completes at once: the iLLD drivers of those
 *     projects that wait for it, named where it is modelled;
 *   - the CPU registers (BOOTCON, CORE_ID): TriCore TC1.8 architecture
 *     manual, volume 1 (V1.0.0, 2024-02-14), section 3.4.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/seqlock.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "system/runstate.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/hostmem.h"
#include "migration/vmstate.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "trace.h"
#include "hw/core/irq.h"
#include "hw/net/tc4x_geth.h"
#include "hw/tricore/tc4x_board.h"
#include "hw/tricore/tc4x_dma.h"
#include "hw/tricore/tc4x_flash.h"
#include "hw/tricore/tc4x_pacer.h"
#include "hw/tricore/tc4x_regs.h"
#include "hw/tricore/tc4x_sfr_store.h"
#include "hw/tricore/tc4x_ir.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_timebase.h"
#include "tc4x_soc.h"

#define TC4X_PSPR_OFFSET    (1 * MiB)
/* CPUcs, core 6, lands in segment 1 */
#define TC4X_CORE_RAM_BASE(c) (0x70000000u - (c) * 0x10000000u)
#define TC4X_LMU_BASE       0x90000000u
#define TC4X_LMU_NC_BASE    0xB0000000u
#define TC4X_LOCAL_DSPR     0xD0000000u
#define TC4X_LOCAL_PSPR     0xC0000000u

#define CPU_SFR_BASE        0xF8800000u
#define CPU_SFR_STRIDE      0x40000u
#define CPU_HRA_PC          0x1FE08u
#define CPU_HRA_CORE_ID     0x1FE1Cu
#define CPU_HRA_BOOTCON     0x1FE60u
#define CPU_HRHV_CORE_ID    0x3FE1Cu

#define SRC_BASE            0xF4432000u
/* General purpose service requests: INT_GPSRGx_SWCy, SRC_GPSRx_SRy */
#define INT_GPSRG_SWC0      0xF4430720u
#define INT_GPSRG_STRIDE    0x40u
#define INT_GPSRG_GROUPS    8
#define INT_GPSRG_SWC_COUNT 8
#define INT_GPSRG_SWC_SETR  BIT(29)
#define SRC_GPSR0_SR0       0xF4433460u
#define SRC_GPSR_STRIDE     0x20u
#define SRC_ASCLIN_TX(n)    (SRC_BASE + 0x2B0u + (n) * 12u)
#define SRC_ASCLIN_RX(n)    (SRC_BASE + 0x2B4u + (n) * 12u)

#define ASCLIN_BASE         0xF46C0000u
#define ASCLIN_STRIDE       0x200u
#define ASCLIN_COUNT        28u
#define ASCLIN_END          (ASCLIN_BASE + ASCLIN_COUNT * ASCLIN_STRIDE)
#define ASCLIN_TXFIFOCON    0x104u
#define ASCLIN_RXFIFOCON    0x108u
#define ASCLIN_FLAGS        0x12Cu
#define ASCLIN_CSR          0x13Cu
#define ASCLIN_CSR_CLKSEL   0x1Fu
#define ASCLIN_CSR_CON      BIT(31)     /* IfxAsclin_getClockStatus() */
#define ASCLIN_FLAGSSET     0x130u
#define ASCLIN_FLAGSCLEAR   0x134u
#define ASCLIN_TXDATA       0x140u
#define ASCLIN_RXDATA       0x160u
#define ASCLIN_FIFO_END     0x180u
#define ASCLIN_FILL_SHIFT   16
#define ASCLIN_FLAGS_TFL    BIT(31)
#define ASCLIN_FLAGS_RFL    BIT(28)
#define ASCLIN_RXFIFO_DEPTH 16

/*
 * DMA0/DMA1 move engines.  Channels are numbered 0..127 on DMA0 and
 * 128..255 on DMA1 (bit 7 selects the instance).
 */
#define DMA0_BASE           0xF4410000u
#define DMA_INST_STRIDE     0x4000u
#define DMA_NUM_INST        2u
#define DMA_TSR_OFF         0x1E00u
#define DMA_CH_OFF          0x2000u
#define DMA_CH_STRIDE       0x20u
#define DMA_CH_PER_INST     128u
#define DMA_NUM_CH          (DMA_NUM_INST * DMA_CH_PER_INST)
#define SRC_DMA0_CH_BASE    (SRC_BASE + 0x730u)
#define SRC_DMA1_CH_BASE    (SRC_BASE + 0x980u)
/* one lap of a circular hardware-request list */
#define DMA_HW_MAX_TCS      64u
#define DMA_RDCRCR          0x00u
#define DMA_SDCRCR          0x04u
#define DMA_SADR            0x08u
#define DMA_DADR            0x0Cu
#define DMA_ADICR           0x10u
#define DMA_CHCFGR          0x14u
#define DMA_SHADR           0x18u
#define DMA_CHCSR           0x1Cu
#define DMA_CHCSR_ICH       BIT(18)
#define DMA_CHCSR_CICH      BIT(26)
#define DMA_CHCSR_SIT       BIT(27)
#define DMA_CHCSR_SCH       BIT(31)
#define DMA_TSR_RST         BIT(0)
#define DMA_TSR_HTRE        BIT(1)
#define DMA_TSR_ECH         BIT(16)
#define DMA_TSR_DCH         BIT(17)
#define DMA_TSR_HLTREQ      BIT(8)
#define DMA_TSR_HLTACK      BIT(9)
#define DMA_TSR_HLTCLR      BIT(24)
#define DMA_SHCT_LINKED     0xCu
#define DMA_MAX_CHAIN       4096u
/*
 * Resource partitions (iLLD IfxDma_regdef.h, Ifx_DMA_RP): HRRc.HRP assigns
 * channel c to one; its ERRSR reports bus errors (LEC: the channel), CLRE
 * clears them, EER enables the partition's error service request
 * SRC_DMAx_ERRn (IfxDma_getErrPointer())
 */
#define DMA_RP_OFF          0x1000u
#define DMA_RP_STRIDE       0x80u
#define DMA_RP_EER          0x44u
#define DMA_RP_ERRSR        0x48u
#define DMA_RP_CLRE         0x4Cu
#define DMA_HRR_OFF         0x1800u
#define DMA_ERR_CLEARABLE   0xFF0F0000u
#define SRC_DMA0_ERR_BASE   (SRC_BASE + 0x6E0u)
#define SRC_DMA1_ERR_BASE   (SRC_BASE + 0x930u)

/* eGTM TOM channels: CN0 counts up at the CMU clock and wraps at CM0 */
#define EGTM_TOM_BASE       0xF9081000u
#define EGTM_CLS_STRIDE     0x20000u
#define EGTM_CLS_COUNT      8u
#define EGTM_TOM_CH_STRIDE  0x40u
#define EGTM_TOM_CH_COUNT   16u
#define EGTM_TOM_SR0        0x04u
#define EGTM_TOM_SR1        0x08u
#define EGTM_TOM_CM0        0x0Cu
#define EGTM_TOM_CM1        0x10u
#define EGTM_TOM_TGC_GLB_CTRL(tgc)  (0x430u + (tgc) * 0x80u)
#define EGTM_TOM_TGC_CHANNELS       8u
#define EGTM_TOM_UPEN_SHIFT         16      /* 2 bits per channel */
#define EGTM_TOM_UPEN_ENABLE        2u
#define EGTM_TOM_CN0        0x14u
#define EGTM_TOM_CN0_MASK   0xFFFFFFu
#define EGTM_CMU_HZ         100000000ULL


/*
 * Start-up status reported as done: CLOCK_RAMPSTAT.ACTIVE (IfxClock.c),
 * VMTn_MEMDONE(CS) (the memory test sequence of IfxVmt.h).
 * PLLs: STAT.PWRSTAT and STAT.PLLLOCK follow CON0.PLLPWR (lock at once);
 * IfxClock.c waits for both while it powers the PLLs up.
 */
#define VMT_STRIDE          (A_VMT1_MEMDONE - A_VMT0_MEMDONE)
#define VMT_COUNT           7u

typedef struct Tc4xTomCounter {
    uint32_t start;     /* CN0 value written by software */
    int64_t t0_ns;      /* guest time of that write */
} Tc4xTomCounter;

typedef struct Tc4xSimCommand {
    const char *prefix;
    Tc4xSimCommandFn fn;
    void *opaque;
    bool qspi_lock;
} Tc4xSimCommand;

struct Tc4xSoc {
    DeviceState parent_obj;

    char *cpu_type;

    TriCoreCPU *cpu[TC4X_NUM_CORES];
    MemoryRegion cpu_root[TC4X_NUM_CORES];
    MemoryRegion cpu_sys_alias[TC4X_NUM_CORES];
    MemoryRegion cpu_local_dspr[TC4X_NUM_CORES];
    MemoryRegion cpu_local_pspr[TC4X_NUM_CORES];
    /* the core and LMU RAMs: own RAMs, or those of the memory backends */
    MemoryRegion *core_ram[TC4X_NUM_CORES];
    MemoryRegion *lmu;
    MemoryRegion own_core_ram[TC4X_NUM_CORES];
    MemoryRegion own_lmu;
    HostMemoryBackend *core_memdev[TC4X_NUM_CORES];
    HostMemoryBackend *lmu_memdev;
    Tc4xFlashState flash;
    MemoryRegion lmu_alias;

    Tc4xSfrStore *sfr;
    GHashTable *tom_cn0;   /* TOM CN0 addr -> Tc4xTomCounter */
    DeviceState *geth;
    Tc4xIr *ir;
    Object *dma_policy;     /* TYPE_TC4X_DMA_POLICY */

    Tc4xTimebase *timebase;
    Tc4xTimeRequest *bridge_time_request;
    MemTxAttrs bridge_attrs;    /* the bus master of bridge r and w */

    uint32_t console;       /* ASCLIN instance of the console chardev */
    CharFrontend uart;
    uint8_t rx_fifo[ASCLIN_RXFIFO_DEPTH];
    unsigned rx_head, rx_count;

    CharFrontend sim;
    GString *sim_line;
    GArray *sim_commands;   /* Tc4xSimCommand */

    /*
     * Channels with TSR.HTRE (hardware requests enabled): changed under the
     * BQL, read without it (Tc4xQspiDmaOps.armed)
     */
    unsigned long dma_htre[BITS_TO_LONGS(DMA_NUM_CH)];

    Tc4xPorts *ports;
    Tc4xTmadc *tmadc;
    Tc4xI2c *i2c[TC4X_I2C_COUNT];
    Tc4xQspi *qspi[TC4X_QSPI_COUNT];

    uint32_t boot_pc_board;     /* this reset's start address from the board */
    bool boot_hold;             /* this reset leaves core 0 halted */
    bool imprecise_io;      /* SFR accesses skip the TCG I/O rewind */
    uint32_t num_port_straps;
    uint32_t *port_straps;  /* TC4X_PORTS_STRAP() entries */
};

Tc4xPorts *tc4x_soc_ports(Tc4xSoc *s)
{
    return s->ports;
}

Tc4xTimebase *tc4x_soc_timebase(Tc4xSoc *s)
{
    return s->timebase;
}

Tc4xTmadc *tc4x_soc_tmadc(Tc4xSoc *s)
{
    return s->tmadc;
}

Tc4xI2c *tc4x_soc_i2c(Tc4xSoc *s, unsigned n)
{
    return s->i2c[n];
}

Tc4xQspi *tc4x_soc_qspi(Tc4xSoc *s, unsigned n)
{
    return s->qspi[n];
}

static uint32_t reg_get(Tc4xSoc *s, uint32_t addr)
{
    return tc4x_sfr_store_get(s->sfr, addr);
}

static void reg_set(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    tc4x_sfr_store_set(s->sfr, addr, val);
}

uint32_t tc4x_reg_get(Tc4xSoc *s, uint32_t addr)
{
    return tc4x_ir_is_src(addr) ? tc4x_ir_src_get(s->ir, addr) :
                                  reg_get(s, addr);
}

void tc4x_reg_set(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    if (tc4x_ir_is_src(addr)) {
        tc4x_ir_src_set(s->ir, addr, val);
    } else {
        reg_set(s, addr, val);
    }
}

uint32_t tc4x_reg_cmpxchg(Tc4xSoc *s, uint32_t addr, uint32_t old,
                          uint32_t val)
{
    return tc4x_sfr_store_cmpxchg(s->sfr, addr, old, val);
}

static void G_GNUC_PRINTF(2, 3) sim_printf(Tc4xSoc *s, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *line = NULL;

    if (!qemu_chr_fe_backend_connected(&s->sim)) {
        return;
    }
    va_start(ap, fmt);
    line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_chr_fe_write_all(&s->sim, (const uint8_t *)line, strlen(line));
}

/* A hardware service request of the node at @src (any thread) */
static void src_raise(Tc4xSoc *s, uint32_t src)
{
    qemu_irq_raise(tc4x_ir_src(s->ir, src));
}

void tc4x_irq_raise(Tc4xSoc *s, uint32_t src)
{
    qemu_irq_pulse(tc4x_ir_src(s->ir, src));
}

int64_t tc4x_guest_ns(Tc4xSoc *s)
{
    return tc4x_timebase_ns(s->timebase);
}

int64_t tc4x_host_ns_for_guest(Tc4xSoc *s, int64_t guest_ns)
{
    return tc4x_timebase_host_ns(s->timebase, guest_ns);
}

static uint32_t asclin_reg(unsigned n, uint32_t off)
{
    return ASCLIN_BASE + n * ASCLIN_STRIDE + off;
}

static int uart_can_receive(void *opaque)
{
    Tc4xSoc *s = opaque;

    return ASCLIN_RXFIFO_DEPTH - s->rx_count;
}

static void uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Tc4xSoc *s = opaque;

    for (int i = 0; i < size && s->rx_count < ASCLIN_RXFIFO_DEPTH; i++) {
        s->rx_fifo[(s->rx_head + s->rx_count) % ASCLIN_RXFIFO_DEPTH] = buf[i];
        s->rx_count++;
    }
    reg_set(s, asclin_reg(s->console, ASCLIN_FLAGS),
            reg_get(s, asclin_reg(s->console, ASCLIN_FLAGS)) |
            ASCLIN_FLAGS_RFL);
    src_raise(s, SRC_ASCLIN_RX(s->console));
}

static uint32_t uart_rx_pop(Tc4xSoc *s)
{
    uint32_t c;

    if (s->rx_count == 0) {
        return 0;
    }
    c = s->rx_fifo[s->rx_head];
    s->rx_head = (s->rx_head + 1) % ASCLIN_RXFIFO_DEPTH;
    s->rx_count--;
    qemu_chr_fe_accept_input(&s->uart);
    return c;
}

/*
 * Every instance transmits at once with an empty TX FIFO; only the console
 * instance has a character backend and receives.
 */
static bool asclin_read(Tc4xSoc *s, uint32_t addr, uint32_t *v)
{
    unsigned n = (addr - ASCLIN_BASE) / ASCLIN_STRIDE;
    uint32_t off = (addr - ASCLIN_BASE) % ASCLIN_STRIDE;
    bool console = n == s->console;

    if (addr < ASCLIN_BASE || addr >= ASCLIN_END) {
        return false;
    }
    if (off == ASCLIN_TXFIFOCON) {
        *v &= ~(0x1Fu << ASCLIN_FILL_SHIFT);
    } else if (off == ASCLIN_RXFIFOCON) {
        *v = (*v & ~(0x1Fu << ASCLIN_FILL_SHIFT)) |
             ((console ? s->rx_count : 0) << ASCLIN_FILL_SHIFT);
    } else if (off == ASCLIN_FLAGS) {
        *v |= ASCLIN_FLAGS_TFL;
    } else if (off == ASCLIN_CSR) {
        /* the selected clock is on at once: CON follows CLKSEL */
        *v = (*v & ~ASCLIN_CSR_CON) |
             ((*v & ASCLIN_CSR_CLKSEL) ? ASCLIN_CSR_CON : 0);
    } else if (off >= ASCLIN_RXDATA && off < ASCLIN_FIFO_END) {
        *v = console ? uart_rx_pop(s) : 0;
    } else {
        return false;
    }
    return true;
}

static bool asclin_write(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    unsigned n = (addr - ASCLIN_BASE) / ASCLIN_STRIDE;
    uint32_t off = (addr - ASCLIN_BASE) % ASCLIN_STRIDE;
    uint32_t flags = asclin_reg(n, ASCLIN_FLAGS);

    if (addr < ASCLIN_BASE || addr >= ASCLIN_END) {
        return false;
    }
    if (off >= ASCLIN_TXDATA && off < ASCLIN_RXDATA) {
        uint8_t ch = val;

        if (n == s->console) {
            qemu_chr_fe_write_all(&s->uart, &ch, 1);
        }
        src_raise(s, SRC_ASCLIN_TX(n));
    } else if (off == ASCLIN_FLAGSCLEAR) {
        reg_set(s, flags, reg_get(s, flags) & ~val);
    } else if (off == ASCLIN_FLAGSSET) {
        reg_set(s, flags, reg_get(s, flags) | val);
    } else {
        return false;
    }
    return true;
}

static uint32_t dma_base(uint32_t ch)
{
    return DMA0_BASE + (ch / DMA_CH_PER_INST) * DMA_INST_STRIDE;
}

static uint32_t dma_reg(uint32_t ch, uint32_t off)
{
    return dma_base(ch) + DMA_CH_OFF +
           (ch % DMA_CH_PER_INST) * DMA_CH_STRIDE + off;
}

static uint32_t dma_tsr(uint32_t ch)
{
    return dma_base(ch) + DMA_TSR_OFF + (ch % DMA_CH_PER_INST) * 4;
}

static uint32_t dma_src(uint32_t ch)
{
    return (ch < DMA_CH_PER_INST ? SRC_DMA0_CH_BASE : SRC_DMA1_CH_BASE) +
           (ch % DMA_CH_PER_INST) * 4;
}

static bool dma_decode(uint32_t addr, uint32_t *ch, uint32_t *off, bool *tsr)
{
    uint32_t inst, rel;

    if (addr < DMA0_BASE ||
        addr >= DMA0_BASE + DMA_NUM_INST * DMA_INST_STRIDE) {
        return false;
    }
    inst = (addr - DMA0_BASE) / DMA_INST_STRIDE;
    rel = (addr - DMA0_BASE) % DMA_INST_STRIDE;
    if (rel >= DMA_TSR_OFF && rel < DMA_TSR_OFF + DMA_CH_PER_INST * 4) {
        *ch = inst * DMA_CH_PER_INST + (rel - DMA_TSR_OFF) / 4;
        *off = 0;
        *tsr = true;
        return true;
    }
    if (rel >= DMA_CH_OFF &&
        rel < DMA_CH_OFF + DMA_CH_PER_INST * DMA_CH_STRIDE) {
        *ch = inst * DMA_CH_PER_INST + (rel - DMA_CH_OFF) / DMA_CH_STRIDE;
        *off = (rel - DMA_CH_OFF) % DMA_CH_STRIDE;
        *tsr = false;
        return true;
    }
    return false;
}

/*
 * Next address of one side of a move (INCx direction, xMF factor, circular
 * buffer)
 */
static uint32_t dma_next_addr(uint32_t addr, uint32_t width, bool inc,
                              uint32_t mf, bool cb_enable, uint32_t cbl)
{
    uint32_t step = width << mf;
    uint32_t next = inc ? addr + step : addr - step;

    if (cb_enable) {
        uint32_t mask = (1u << cbl) - 1;

        next = (addr & ~mask) | (next & mask);
    }
    return next;
}

static const unsigned dma_rp_count[DMA_NUM_INST] = { 16, 8 };

static uint32_t dma_rp_reg(uint32_t ch, unsigned rp, uint32_t off)
{
    return dma_base(ch) + DMA_RP_OFF + rp * DMA_RP_STRIDE + off;
}

/*
 * Bus errors @err (TC4X_DMA_ERR_*) of channel @ch, the first at @addr: the
 * error flags and the channel in the ERRSR of the channel's resource
 * partition, and the partition's error service request when EER enables
 * one of them
 */
static void dma_bus_error(Tc4xSoc *s, uint32_t ch, uint32_t err,
                          uint32_t addr)
{
    unsigned inst = ch / DMA_CH_PER_INST;
    unsigned rp = extract32(reg_get(s, dma_base(ch) + DMA_HRR_OFF +
                                    (ch % DMA_CH_PER_INST) * 4), 0, 4);
    uint32_t errsr;

    qemu_log_mask(LOG_GUEST_ERROR, "tc4x-dma: ch %u %s bus error at "
                  "0x%08x\n", ch,
                  err & TC4X_DMA_ERR_SER ? "source" :
                  err & TC4X_DMA_ERR_DER ? "destination" : "linked list load",
                  addr);
    if (rp >= dma_rp_count[inst]) {
        qemu_log_mask(LOG_GUEST_ERROR, "tc4x-dma: ch %u is in resource "
                      "partition %u, which DMA%u does not have\n", ch, rp,
                      inst);
        return;
    }
    errsr = reg_get(s, dma_rp_reg(ch, rp, DMA_RP_ERRSR)) | err;
    reg_set(s, dma_rp_reg(ch, rp, DMA_RP_ERRSR),
            deposit32(errsr, 0, 7, ch % DMA_CH_PER_INST));
    if (reg_get(s, dma_rp_reg(ch, rp, DMA_RP_EER)) & err) {
        src_raise(s, (inst ? SRC_DMA1_ERR_BASE : SRC_DMA0_ERR_BASE) + rp * 4);
    }
}

/*
 * The policy's verdict on the failed access of channel @ch at @addr: true
 * to drop what it delivers
 */
static bool dma_policy_drops(Tc4xSoc *s, uint32_t ch, uint32_t err,
                             uint32_t addr)
{
    Tc4xDmaPolicyClass *pc;

    if (!s->dma_policy) {
        return false;
    }
    pc = TC4X_DMA_POLICY_GET_CLASS(s->dma_policy);
    return pc->bus_error &&
           pc->bus_error(TC4X_DMA_POLICY(s->dma_policy), ch, err, addr);
}

/* CLRE of a resource partition: clear the ERRSR flags written as 1 */
static bool dma_clre_write(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    uint32_t inst, rel;

    if (addr < DMA0_BASE ||
        addr >= DMA0_BASE + DMA_NUM_INST * DMA_INST_STRIDE) {
        return false;
    }
    inst = (addr - DMA0_BASE) / DMA_INST_STRIDE;
    rel = (addr - DMA0_BASE) % DMA_INST_STRIDE;
    if (rel < DMA_RP_OFF ||
        rel >= DMA_RP_OFF + dma_rp_count[inst] * DMA_RP_STRIDE ||
        (rel - DMA_RP_OFF) % DMA_RP_STRIDE != DMA_RP_CLRE) {
        return false;
    }
    addr += DMA_RP_ERRSR - DMA_RP_CLRE;
    reg_set(s, addr, reg_get(s, addr) & ~(val & DMA_ERR_CLEARABLE));
    return true;
}

/*
 * Execute one transaction of channel ch.  Returns true if the channel reloaded
 * a linked TCS from SHADR; *auto_start reports that the loaded TCS has SCH set.
 * A bus error is reported (dma_bus_error()); beyond that the channel does
 * what the board's policy says (dma_policy_drops()).
 */
static bool dma_transaction(Tc4xSoc *s, uint32_t ch, bool *auto_start)
{
    static const uint32_t blkm_moves[8] = { 1, 2, 4, 8, 16, 3, 5, 9 };
    uint32_t adicr = reg_get(s, dma_reg(ch, DMA_ADICR));
    uint32_t cfg = reg_get(s, dma_reg(ch, DMA_CHCFGR));
    uint32_t sadr = reg_get(s, dma_reg(ch, DMA_SADR));
    uint32_t dadr = reg_get(s, dma_reg(ch, DMA_DADR));
    uint32_t trel = extract32(cfg, 0, 14);
    uint32_t moves = (trel ? trel : 1) * blkm_moves[extract32(cfg, 16, 3)];
    uint32_t width = 1u << extract32(cfg, 21, 3);
    MemTxAttrs attrs = tc4x_master_attrs(TC4X_MASTER_DMA(ch), false);
    uint8_t buf[1u << 7];   /* CHDW 6 and 7 are reserved but encodable */
    uint32_t err = 0, err_addr = 0;

    *auto_start = false;
    for (uint32_t i = 0; i < moves; i++) {
        bool write = true;

        if (address_space_read(&address_space_memory, sadr, attrs, buf,
                               width) != MEMTX_OK) {
            err_addr = err ? err_addr : sadr;
            err |= TC4X_DMA_ERR_SER;
            write = !dma_policy_drops(s, ch, TC4X_DMA_ERR_SER, sadr);
        }
        if (write && address_space_write(&address_space_memory, dadr, attrs,
                                         buf, width) != MEMTX_OK) {
            err_addr = err ? err_addr : dadr;
            err |= TC4X_DMA_ERR_DER;
            dma_policy_drops(s, ch, TC4X_DMA_ERR_DER, dadr);
        }
        sadr = dma_next_addr(sadr, width, extract32(adicr, 3, 1),
                             extract32(adicr, 0, 3), extract32(adicr, 20, 1),
                             extract32(adicr, 8, 4));
        dadr = dma_next_addr(dadr, width, extract32(adicr, 7, 1),
                             extract32(adicr, 4, 3), extract32(adicr, 21, 1),
                             extract32(adicr, 12, 4));
    }
    reg_set(s, dma_reg(ch, DMA_SADR), sadr);
    reg_set(s, dma_reg(ch, DMA_DADR), dadr);
    reg_set(s, dma_reg(ch, DMA_CHCSR),
            reg_get(s, dma_reg(ch, DMA_CHCSR)) & ~(DMA_CHCSR_SCH | 0x3FFFu));
    if (err) {
        dma_bus_error(s, ch, err, err_addr);
    }

    if (extract32(adicr, 26, 2) & 2) {
        reg_set(s, dma_reg(ch, DMA_CHCSR),
                reg_get(s, dma_reg(ch, DMA_CHCSR)) | DMA_CHCSR_ICH);
        src_raise(s, dma_src(ch));
    }

    if (extract32(adicr, 16, 4) == DMA_SHCT_LINKED) {
        uint32_t shadr = reg_get(s, dma_reg(ch, DMA_SHADR));
        uint32_t tcs[8];

        if (address_space_read(&address_space_memory, shadr, attrs, tcs,
                               sizeof(tcs)) != MEMTX_OK) {
            dma_bus_error(s, ch, TC4X_DMA_ERR_DLLER, shadr);
            if (dma_policy_drops(s, ch, TC4X_DMA_ERR_DLLER, shadr)) {
                return false;
            }
        }
        for (int w = 0; w < 8; w++) {
            reg_set(s, dma_reg(ch, w * 4), le32_to_cpu(tcs[w]));
        }
        *auto_start = (le32_to_cpu(tcs[7]) & DMA_CHCSR_SCH) != 0;
        return true;
    }
    return false;
}

static void dma_trace_start(Tc4xSoc *s, uint32_t ch, bool hw)
{
    trace_tc4x_dma_start(ch, hw, reg_get(s, dma_reg(ch, DMA_SADR)),
                         reg_get(s, dma_reg(ch, DMA_DADR)),
                         reg_get(s, dma_reg(ch, DMA_CHCFGR)));
}

/* Software request (CHCSR.SCH): follow the linked list while TCSs set SCH */
static void dma_run(Tc4xSoc *s, uint32_t ch)
{
    uint32_t n = 0;
    bool more;

    dma_trace_start(s, ch, false);
    while (dma_transaction(s, ch, &more) && more && ++n < DMA_MAX_CHAIN) {
        /* follow the linked list while TCSs request continuation */
    }
    if (n >= DMA_MAX_CHAIN) {
        qemu_log_mask(LOG_GUEST_ERROR, "tc4x-dma: ch %u linked list did not "
                      "stop after %u transactions\n", ch, n);
    }
}

/*
 * Hardware requests (TSR.HTRE): the peripheral keeps requesting, so every
 * linked TCS runs until the list ends or wraps around (circular lists run
 * one lap per trigger).  Single-mode channels drop HTRE when they finish.
 */
static void dma_run_hw(Tc4xSoc *s, uint32_t ch)
{
    uint32_t seen[DMA_HW_MAX_TCS];
    uint32_t n = 0;
    bool unused;

    dma_trace_start(s, ch, true);
    for (;;) {
        uint32_t next = reg_get(s, dma_reg(ch, DMA_SHADR));
        bool lap = false;

        for (uint32_t i = 0; i < n; i++) {
            lap |= seen[i] == next;
        }
        if (!dma_transaction(s, ch, &unused) || lap || n >= DMA_HW_MAX_TCS) {
            break;
        }
        seen[n++] = next;
    }
    if (!extract32(reg_get(s, dma_reg(ch, DMA_CHCFGR)), 20, 1)) {
        reg_set(s, dma_tsr(ch), reg_get(s, dma_tsr(ch)) & ~DMA_TSR_HTRE);
        clear_bit_atomic(ch, s->dma_htre);
    }
}

/* Channels with hardware requests enabled; callers hold the BQL */
static void dma_htre_update(Tc4xSoc *s, uint32_t ch, uint32_t tsr)
{
    if (tsr & DMA_TSR_HTRE) {
        set_bit_atomic(ch, s->dma_htre);
    } else {
        clear_bit_atomic(ch, s->dma_htre);
    }
}

/* A TSR stored behind dma_write's back (sim bridge poke) */
static void dma_htre_sync(Tc4xSoc *s, uint32_t addr)
{
    uint32_t ch, off;
    bool is_tsr;

    if (dma_decode(addr, &ch, &off, &is_tsr) && is_tsr) {
        dma_htre_update(s, ch, reg_get(s, addr));
    }
}

static bool dma_write(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    uint32_t ch, off;
    bool is_tsr;

    if (!dma_decode(addr, &ch, &off, &is_tsr)) {
        return false;
    }
    if (is_tsr) {
        uint32_t tsr = reg_get(s, addr);

        if (val & DMA_TSR_ECH) {
            tsr |= DMA_TSR_HTRE;
        }
        if (val & DMA_TSR_DCH) {
            tsr &= ~DMA_TSR_HTRE;
        }
        if (val & DMA_TSR_HLTREQ) {
            tsr |= DMA_TSR_HLTREQ | DMA_TSR_HLTACK;
        }
        if (val & DMA_TSR_HLTCLR) {
            tsr &= ~(DMA_TSR_HLTREQ | DMA_TSR_HLTACK);
        }
        if (val & DMA_TSR_RST) {
            tsr = 0;
        }
        reg_set(s, addr, tsr);
        dma_htre_update(s, ch, tsr);
        return true;
    }
    if (off == DMA_CHCSR) {
        uint32_t csr = reg_get(s, addr) & ~DMA_CHCSR_SCH;

        if (val & DMA_CHCSR_CICH) {
            csr &= ~DMA_CHCSR_ICH;
        }
        if (val & DMA_CHCSR_SIT) {
            csr |= DMA_CHCSR_ICH;
        }
        reg_set(s, addr, csr | (val & DMA_CHCSR_SCH));
        if (val & DMA_CHCSR_SCH) {
            dma_run(s, ch);
        }
        return true;
    }
    return false;
}

/*
 * DMA-driven QSPI jobs (tc4x_qspi.c): the transmit channels write the
 * module's BACONENTRY/DATAENTRY, the receive channels read its RXEXIT.
 */
static bool qspi_dma_side(Tc4xSoc *s, uint32_t ch, uint32_t n, bool tx)
{
    uint32_t base = tc4x_qspi_base(n);
    uint32_t rx = base + A_QSPI0_RXEXIT0 - A_QSPI0_CLC;
    uint32_t a;

    if (!(reg_get(s, dma_tsr(ch)) & DMA_TSR_HTRE)) {
        return false;
    }
    a = reg_get(s, dma_reg(ch, tx ? DMA_DADR : DMA_SADR));
    return tx ? a >= base + A_QSPI0_BACONENTRY - A_QSPI0_CLC && a < rx
              : a >= rx && a < base + A_QSPI0_RXEXITD - A_QSPI0_CLC;
}

/* Only channels with hardware requests enabled can run (dma_htre) */
static bool soc_qspi_dma_armed(void *opaque, unsigned n)
{
    Tc4xSoc *s = opaque;

    for (uint32_t ch = find_first_bit(s->dma_htre, DMA_NUM_CH); ch < DMA_NUM_CH;
         ch = find_next_bit(s->dma_htre, DMA_NUM_CH, ch + 1)) {
        if (qspi_dma_side(s, ch, n, true) || qspi_dma_side(s, ch, n, false)) {
            return true;
        }
    }
    return false;
}

static bool soc_qspi_dma_run(void *opaque, unsigned n)
{
    Tc4xSoc *s = opaque;
    bool ran = false;

    for (uint32_t ch = 0; ch < DMA_NUM_CH; ch++) {
        if (qspi_dma_side(s, ch, n, true)) {
            dma_run_hw(s, ch);
            ran = true;
        }
    }
    if (ran) {
        for (uint32_t ch = 0; ch < DMA_NUM_CH; ch++) {
            if (qspi_dma_side(s, ch, n, false)) {
                dma_run_hw(s, ch);
            }
        }
    }
    return ran;
}

static const Tc4xQspiDmaOps soc_qspi_dma = {
    .armed = soc_qspi_dma_armed,
    .run = soc_qspi_dma_run,
};

/* Offset of @addr in its TOM cluster; false outside the TOMs */
static bool tom_offset(uint32_t addr, uint32_t *off)
{
    if (addr < EGTM_TOM_BASE ||
        addr >= EGTM_TOM_BASE + EGTM_CLS_COUNT * EGTM_CLS_STRIDE) {
        return false;
    }
    *off = (addr - EGTM_TOM_BASE) % EGTM_CLS_STRIDE;
    return true;
}

/* Offset of @addr in its TOM channel; false outside the channels */
static bool tom_ch_offset(uint32_t addr, uint32_t *off)
{
    if (!tom_offset(addr, off) ||
        *off >= EGTM_TOM_CH_COUNT * EGTM_TOM_CH_STRIDE) {
        return false;
    }
    *off %= EGTM_TOM_CH_STRIDE;
    return true;
}

static bool tom_is_cn0(uint32_t addr)
{
    uint32_t off;

    return tom_ch_offset(addr, &off) && off == EGTM_TOM_CN0;
}

static uint32_t tom_cn0_read(Tc4xSoc *s, uint32_t addr)
{
    Tc4xTomCounter *c = g_hash_table_lookup(s->tom_cn0, GUINT_TO_POINTER(addr));
    uint64_t period = reg_get(s, addr - EGTM_TOM_CN0 + EGTM_TOM_CM0) &
                      EGTM_TOM_CN0_MASK;
    int64_t now = tc4x_timebase_ns(s->timebase);
    uint64_t ticks;

    if (!c) {
        c = g_new0(Tc4xTomCounter, 1);
        c->t0_ns = now;
        g_hash_table_insert(s->tom_cn0, GUINT_TO_POINTER(addr), c);
    }
    ticks = c->start + muldiv64(now - c->t0_ns, EGTM_CMU_HZ,
                                NANOSECONDS_PER_SECOND);
    return period ? ticks % period : ticks & EGTM_TOM_CN0_MASK;
}

/*
 * TGC GLB_CTRL with UPEN_CTRLn = enable: the channel copies its shadow
 * registers SR0/SR1 to CM0/CM1 at the end of the period.  The copy happens
 * at once here.
 */
static void tom_glb_ctrl_write(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    uint32_t off = (addr - EGTM_TOM_BASE) % EGTM_CLS_STRIDE;
    uint32_t cls_base = addr - off;
    unsigned tgc = off == EGTM_TOM_TGC_GLB_CTRL(1);

    for (unsigned k = 0; k < EGTM_TOM_TGC_CHANNELS; k++) {
        uint32_t ch;

        if (extract32(val, EGTM_TOM_UPEN_SHIFT + 2 * k, 2) !=
            EGTM_TOM_UPEN_ENABLE) {
            continue;
        }
        ch = cls_base + (tgc * EGTM_TOM_TGC_CHANNELS + k) * EGTM_TOM_CH_STRIDE;
        reg_set(s, ch + EGTM_TOM_CM0, reg_get(s, ch + EGTM_TOM_SR0));
        reg_set(s, ch + EGTM_TOM_CM1, reg_get(s, ch + EGTM_TOM_SR1));
    }
}

static bool tom_is_glb_ctrl(uint32_t addr)
{
    uint32_t off;

    return tom_offset(addr, &off) &&
           (off == EGTM_TOM_TGC_GLB_CTRL(0) || off == EGTM_TOM_TGC_GLB_CTRL(1));
}

static void tom_cn0_write(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    Tc4xTomCounter *c = g_new0(Tc4xTomCounter, 1);

    c->start = val & EGTM_TOM_CN0_MASK;
    c->t0_ns = tc4x_timebase_ns(s->timebase);
    g_hash_table_insert(s->tom_cn0, GUINT_TO_POINTER(addr), c);
}

static bool sfr_is_cpu(uint32_t addr, int *core, uint32_t *off)
{
    if (addr < CPU_SFR_BASE ||
        addr >= CPU_SFR_BASE + TC4X_NUM_CORES * CPU_SFR_STRIDE) {
        return false;
    }
    *core = (addr - CPU_SFR_BASE) / CPU_SFR_STRIDE;
    *off = (addr - CPU_SFR_BASE) % CPU_SFR_STRIDE;
    return true;
}

/*
 * A read of the word at @addr served by a model, under the BQL; *@val holds
 * the stored word
 */
static bool soc_read_word(Tc4xSoc *s, uint32_t addr, uint32_t *val)
{
    uint32_t off;
    int core;

    if (sfr_is_cpu(addr, &core, &off)) {
        if (off == CPU_HRA_CORE_ID || off == CPU_HRHV_CORE_ID) {
            *val = core;
        } else if (off == CPU_HRA_BOOTCON) {
            *val = (*val & ~1u) | (s->cpu[core]->env.boot_halted ? 1 : 0);
        }
    } else if (tom_is_cn0(addr)) {
        *val = tom_cn0_read(s, addr);
    } else if (addr == A_CLOCK_SYSPLLSTAT || addr == A_CLOCK_PERPLLSTAT) {
        uint32_t con0 = reg_get(s, addr == A_CLOCK_SYSPLLSTAT ?
                                A_CLOCK_SYSPLLCON0 : A_CLOCK_PERPLLCON0);
        uint32_t on = R_CLOCK_SYSPLLSTAT_PWRSTAT_MASK |
                      R_CLOCK_SYSPLLSTAT_PLLLOCK_MASK;

        *val = (*val & ~on) |
               (FIELD_EX32(con0, CLOCK_SYSPLLCON0, PLLPWR) ? on : 0);
    } else if (!asclin_read(s, addr, val)) {
        return false;
    }
    return true;
}

static bool soc_sfr_read(void *opaque, uint32_t addr, uint32_t *val)
{
    return soc_read_word(opaque, addr, val);
}

/*
 * Reads with side effects and time-derived or CPU-specific values take
 * the BQL; the store serves the others without it
 */
static Tc4xSfrPath soc_sfr_read_lockless(void *opaque, uint32_t addr,
                                         unsigned size, uint32_t *val)
{
    uint32_t word = addr & ~3u;
    uint32_t off;
    int core;

    if (sfr_is_cpu(word, &core, &off) || tom_is_cn0(word) ||
        (word >= ASCLIN_BASE && word < ASCLIN_END)) {
        return TC4X_SFR_PATH_LOCKED;
    }
    return TC4X_SFR_PATH_STORE;
}

/* Side effects of CPU SFR writes, once stored */
static void soc_cpu_write(Tc4xSoc *s, uint32_t addr, int core, uint32_t off,
                          uint32_t val)
{
    if (off == CPU_HRA_BOOTCON && !(val & 1) &&
        s->cpu[core]->env.boot_halted) {
        uint32_t pc = reg_get(s, addr - CPU_HRA_BOOTCON + CPU_HRA_PC) & ~1u;

        tricore_cpu_release_boot_halt(s->cpu[core], pc);
    } else {
        tc4x_timebase_cpu_sfr_written(s->timebase, addr, val);
    }
}

static bool soc_sfr_write(void *opaque, uint32_t addr, uint32_t *val)
{
    Tc4xSoc *s = opaque;
    uint32_t off;
    int core;

    if (asclin_write(s, addr, *val) || dma_write(s, addr, *val) ||
        dma_clre_write(s, addr, *val)) {
        return true;
    }
    if (addr >= INT_GPSRG_SWC0 &&
        addr < INT_GPSRG_SWC0 + INT_GPSRG_GROUPS * INT_GPSRG_STRIDE &&
        (addr - INT_GPSRG_SWC0) % INT_GPSRG_STRIDE <
        INT_GPSRG_SWC_COUNT * 4 && (*val & INT_GPSRG_SWC_SETR)) {
        /* SETR: software trigger of SRC_GPSRx_SRy */
        uint32_t x = (addr - INT_GPSRG_SWC0) / INT_GPSRG_STRIDE;
        uint32_t y = (addr - INT_GPSRG_SWC0) % INT_GPSRG_STRIDE / 4;

        src_raise(s, SRC_GPSR0_SR0 + x * SRC_GPSR_STRIDE + 4 * y);
        *val &= ~INT_GPSRG_SWC_SETR;
    }
    if (tom_is_cn0(addr)) {
        tom_cn0_write(s, addr, *val);
        return true;
    }
    if (tom_is_glb_ctrl(addr)) {
        tom_glb_ctrl_write(s, addr, *val);      /* then stored as written */
    }
    if (tc4x_flash_dmu_write(&s->flash, addr, *val)) {
        return true;
    }
    if (sfr_is_cpu(addr, &core, &off)) {
        tc4x_sfr_store_write(s->sfr, addr, *val);
        soc_cpu_write(s, addr, core, off, *val);
        return true;
    }
    return false;
}

static void soc_sfr_watched(void *opaque, uint32_t addr, uint32_t val,
                            unsigned size)
{
    sim_printf(opaque, "ev w 0x%08x 0x%08x %u %d\n", addr, val, size,
               current_cpu ? current_cpu->cpu_index : -1);
}

static const Tc4xSfrModelOps soc_sfr_models = {
    .read_lockless = soc_sfr_read_lockless,
    .read = soc_sfr_read,
    .write = soc_sfr_write,
    .watched = soc_sfr_watched,
};

void tc4x_sfr_add_write_hook(Tc4xSoc *s, uint32_t addr, Tc4xSfrWriteFn fn,
                             void *opaque)
{
    tc4x_sfr_store_add_write_hook(s->sfr, addr, fn, opaque);
}

void tc4x_sfr_add_read_hook(Tc4xSoc *s, uint32_t addr, Tc4xSfrReadFn fn,
                            void *opaque)
{
    tc4x_sfr_store_add_read_hook(s->sfr, addr, fn, opaque);
}

void tc4x_time_add_retime_notifier(Tc4xSoc *s, Tc4xRetimeFn fn, void *opaque)
{
    tc4x_timebase_add_retime_notifier(s->timebase, fn, opaque);
}

Tc4xPacer *tc4x_pacer_new(Tc4xSoc *s, const Tc4xPacerConfig *cfg,
                          const Tc4xPacerOps *ops, void *opaque)
{
    return tc4x_timebase_new_pacer(s->timebase, cfg, ops, opaque);
}

void tc4x_sim_add_command(Tc4xSoc *s, const char *prefix, Tc4xSimCommandFn fn,
                          void *opaque, bool qspi_lock)
{
    Tc4xSimCommand c = {
        .prefix = prefix, .fn = fn, .opaque = opaque, .qspi_lock = qspi_lock,
    };

    g_array_append_val(s->sim_commands, c);
}

/*
 * Only commands registered with qspi_lock take the QSPI lock: vmstop waits
 * for vCPUs, which may wait for it.
 */
static bool sim_needs_qspi_lock(Tc4xSoc *s, const char *name)
{
    for (guint i = 0; i < s->sim_commands->len; i++) {
        Tc4xSimCommand *c = &g_array_index(s->sim_commands, Tc4xSimCommand, i);

        if (c->qspi_lock && g_str_has_prefix(name, c->prefix)) {
            return true;
        }
    }
    return false;
}

/* Register commands act on the store only: else reply err */
static bool sim_store_backed(Tc4xSoc *s, uint32_t addr)
{
    Object *owner = tc4x_sfr_store_owner(s->sfr, addr & ~3u);
    g_autofree char *path = NULL;

    if (!owner) {
        return true;
    }
    path = object_get_canonical_path(owner);
    sim_printf(s, "err 0x%08x owned by %s\n", addr, path ? path : "?");
    return false;
}

/* master [debug | cpu N [safe] | dma CH]: the bus master of r and w */
static void sim_master(Tc4xSoc *s, int argc, char **argv)
{
    bool safe = argc == 4 && !strcmp(argv[3], "safe");
    unsigned n, m;
    int core;

    if (argc == 2 && !strcmp(argv[1], "debug")) {
        s->bridge_attrs = tc4x_master_attrs(TC4X_MASTER_DEBUG, false);
    } else if ((argc == 3 || safe) && !strcmp(argv[1], "cpu") &&
               !qemu_strtoui(argv[2], NULL, 0, &n) && n < TC4X_NUM_CORES) {
        s->bridge_attrs = tc4x_master_attrs(TC4X_MASTER_CPU(n), safe);
    } else if (argc == 3 && !strcmp(argv[1], "dma") &&
               !qemu_strtoui(argv[2], NULL, 0, &n) && n < DMA_NUM_CH) {
        s->bridge_attrs = tc4x_master_attrs(TC4X_MASTER_DMA(n), false);
    } else if (argc != 1) {
        sim_printf(s, "err usage: master [debug | cpu N [safe] | dma CH]\n");
        return;
    }
    m = TC4X_REQUESTER_MASTER(s->bridge_attrs.requester_id);
    core = tc4x_master_core(s->bridge_attrs);
    if (m == TC4X_MASTER_DEBUG) {
        sim_printf(s, "ok debug\n");
    } else if (core >= 0) {
        sim_printf(s, "ok cpu %d%s\n", core,
                   TC4X_REQUESTER_SAFE(s->bridge_attrs.requester_id) ?
                   " safe" : "");
    } else {
        sim_printf(s, "ok dma %u\n", m - TC4X_MASTER_DMA(0));
    }
}

static void sim_exec_argv(Tc4xSoc *s, int argc, char **argv)
{
    uint64_t a[4] = { 0 };

    for (int i = 1; i < argc && i <= 3; i++) {
        a[i] = g_ascii_strtoull(argv[i], NULL, 0);
    }

    if (!strcmp(argv[0], "r") && argc >= 2) {
        uint32_t size = argc >= 3 ? a[2] : 4;
        uint32_t v = 0;

        if (size != 1 && size != 2 && size != 4) {
            sim_printf(s, "err size must be 1, 2 or 4\n");
            return;
        }
        address_space_read(&address_space_memory, a[1], s->bridge_attrs, &v,
                           size);
        sim_printf(s, "ok 0x%08x\n", le32_to_cpu(v));
    } else if (!strcmp(argv[0], "w") && argc >= 3) {
        uint32_t size = argc >= 4 ? a[3] : 4;
        uint32_t v = cpu_to_le32(a[2]);

        if (size != 1 && size != 2 && size != 4) {
            sim_printf(s, "err size must be 1, 2 or 4\n");
            return;
        }
        /* as a debugger: it patches flash, which refuses guest stores */
        if (!tc4x_flash_patch(&s->flash, a[1], &v, size)) {
            address_space_write(&address_space_memory, a[1], s->bridge_attrs,
                                &v, size);
        }
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "poke") && argc >= 3) {
        if (sim_store_backed(s, a[1])) {
            reg_set(s, a[1] & ~3u, a[2]);
            dma_htre_sync(s, a[1] & ~3u);
            sim_printf(s, "ok\n");
        }
    } else if (!strcmp(argv[0], "force") && argc >= 3) {
        if (sim_store_backed(s, a[1])) {
            tc4x_sfr_store_force(s->sfr, a[1] & ~3u, a[2],
                                 argc >= 4 ? a[3] : 0xFFFFFFFFu);
            sim_printf(s, "ok\n");
        }
    } else if (!strcmp(argv[0], "unforce") && argc >= 2) {
        if (sim_store_backed(s, a[1])) {
            tc4x_sfr_store_force(s->sfr, a[1] & ~3u, 0, 0);
            sim_printf(s, "ok\n");
        }
    } else if (!strcmp(argv[0], "irq") && argc >= 2) {
        if (tc4x_ir_is_src(a[1])) {
            tc4x_irq_raise(s, a[1] & ~3u);
            sim_printf(s, "ok\n");
        } else {
            sim_printf(s, "err 0x%08x is not a service request node\n",
                       (uint32_t)a[1]);
        }
    } else if ((!strcmp(argv[0], "watch") || !strcmp(argv[0], "unwatch")) &&
               argc >= 2) {
        bool on = !strcmp(argv[0], "watch");

        if (tc4x_ir_is_src(a[1])) {
            tc4x_ir_watch(s->ir, a[1] & ~3u, on);
            sim_printf(s, "ok\n");
        } else if (sim_store_backed(s, a[1])) {
            tc4x_sfr_store_watch(s->sfr, a[1] & ~3u, on);
            sim_printf(s, "ok\n");
        }
    } else if (!strcmp(argv[0], "cores")) {
        for (int c = 0; c < TC4X_NUM_CORES; c++) {
            CPUTriCoreState *env = &s->cpu[c]->env;

            sim_printf(s, "core %d pc 0x%08x %s icr 0x%08x prio %u "
                       "d4 0x%08x d5 0x%08x d6 0x%08x a11 0x%08x\n",
                       c, env->PC,
                       env->boot_halted ? "bhalt" :
                       CPU(s->cpu[c])->halted ? "wait" : "run",
                       env->ICR, env->pending_irq_prio, env->gpr_d[4],
                       env->gpr_d[5], env->gpr_d[6], env->gpr_a[11]);
        }
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "hot")) {
        g_autoptr(GString) out = g_string_new(NULL);

        tc4x_sfr_store_hot(s->sfr, argc >= 2 ? a[1] : 100, out);
        sim_printf(s, "%sok\n", out->str);
    } else if (!strcmp(argv[0], "vmstop")) {
        vm_stop(RUN_STATE_PAUSED);
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "vmcont")) {
        vm_start();
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "master")) {
        sim_master(s, argc, argv);
    } else if (!strcmp(argv[0], "timescale") && argc >= 2) {
        tc4x_timebase_request(s->timebase, s->bridge_time_request, a[1]);
        sim_printf(s, "ok %u\n", tc4x_timebase_scale(s->timebase));
    } else {
        g_autoptr(GString) out = g_string_new(NULL);

        for (guint i = 0; i < s->sim_commands->len; i++) {
            Tc4xSimCommand *c = &g_array_index(s->sim_commands,
                                               Tc4xSimCommand, i);

            if (g_str_has_prefix(argv[0], c->prefix) &&
                c->fn(c->opaque, argc, argv, out)) {
                sim_printf(s, "%s", out->str);
                return;
            }
        }
        sim_printf(s, "err unknown command '%s'\n", argv[0]);
    }
}

void tc4x_sim_exec(Tc4xSoc *s, char *line)
{
    g_auto(GStrv) argv = g_strsplit_set(g_strstrip(line), " \t", -1);
    int argc = g_strv_length(argv);

    if (argc == 0 || argv[0][0] == '\0' || argv[0][0] == '#') {
        return;
    }
    if (sim_needs_qspi_lock(s, argv[0])) {
        tc4x_qspi_lock(s);
        sim_exec_argv(s, argc, argv);
        tc4x_qspi_unlock(s);
        return;
    }
    sim_exec_argv(s, argc, argv);
}

static int sim_can_receive(void *opaque)
{
    return 256;
}

static void sim_receive(void *opaque, const uint8_t *buf, int size)
{
    Tc4xSoc *s = opaque;

    for (int i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            tc4x_sim_exec(s, s->sim_line->str);
            g_string_truncate(s->sim_line, 0);
        } else if (buf[i] != '\r') {
            g_string_append_c(s->sim_line, buf[i]);
        }
    }
}

/*
 * A RAM of the SoC at @base: the memory of the backend @memdev when it is
 * set, else @own as a plain RAM
 */
static MemoryRegion *add_ram(Tc4xSoc *s, MemoryRegion *own,
                             HostMemoryBackend *memdev, const char *name,
                             hwaddr base, uint64_t size, Error **errp)
{
    MemoryRegion *mr = own;

    if (memdev) {
        mr = host_memory_backend_get_memory(memdev);
        if (host_memory_backend_is_mapped(memdev)) {
            error_setg(errp, "memory backend '%s' of %s is already in use",
                       object_get_canonical_path_component(OBJECT(memdev)),
                       name);
            return NULL;
        }
        if (memory_region_size(mr) != size) {
            error_setg(errp, "memory backend '%s' of %s must have %" PRIu64
                       " bytes",
                       object_get_canonical_path_component(OBJECT(memdev)),
                       name, size);
            return NULL;
        }
        host_memory_backend_set_mapped(memdev, true);
        vmstate_register_ram(mr, DEVICE(s));
    } else if (!memory_region_init_ram(own, OBJECT(s), name, size, errp)) {
        return NULL;
    }
    memory_region_add_subregion(get_system_memory(), base, mr);
    return mr;
}

Tc4xFlashState *tc4x_soc_flash(Tc4xSoc *s)
{
    return &s->flash;
}

bool tc4x_flash_kept(Tc4xSoc *s)
{
    return tc4x_flash_is_kept(&s->flash);
}

void tc4x_flash_read(Tc4xSoc *s, Tc4xFlash flash, uint64_t off, void *buf,
                     uint64_t len)
{
    tc4x_flash_storage_read(&s->flash, flash, off, buf, len);
}

void tc4x_flash_write(Tc4xSoc *s, Tc4xFlash flash, uint64_t off,
                      const void *buf, uint64_t len)
{
    tc4x_flash_storage_write(&s->flash, flash, off, buf, len);
}

void tc4x_flash_fill(Tc4xSoc *s, Tc4xFlash flash, uint64_t off,
                     uint8_t value, uint64_t len)
{
    tc4x_flash_storage_fill(&s->flash, flash, off, value, len);
}

void tc4x_flash_set_locate(Tc4xSoc *s, Tc4xFlashLocateFn fn, void *opaque)
{
    tc4x_flash_set_locate_fn(&s->flash, fn, opaque);
}

void tc4x_set_boot_pc(Tc4xSoc *s, uint32_t pc)
{
    s->boot_pc_board = pc;
}

void tc4x_hold_boot(Tc4xSoc *s)
{
    s->boot_hold = true;
}

void tc4x_start_core(Tc4xSoc *s, int core, uint32_t pc)
{
    tricore_cpu_release_boot_halt(s->cpu[core], pc);
}

void tc4x_release_boot(Tc4xSoc *s)
{
    tricore_cpu_release_boot_halt(s->cpu[0], s->cpu[0]->env.PC);
}

void tc4x_nmi(Tc4xSoc *s, int core)
{
    qemu_irq_pulse(qdev_get_gpio_in_named(DEVICE(s->ir), "nmi", core));
}

int tc4x_access_core(Tc4xSoc *s)
{
    MemTxAttrs attrs = tc4x_sfr_store_hook_attrs(s->sfr);

    if (!attrs.unspecified) {
        return tc4x_master_core(attrs);
    }
    for (int c = 0; current_cpu && c < TC4X_NUM_CORES; c++) {
        if (current_cpu == CPU(s->cpu[c])) {
            return c;
        }
    }
    return -1;
}

void tc4x_dmu_set_hook(Tc4xSoc *s, Tc4xDmuHookFn fn, void *opaque)
{
    tc4x_flash_set_hook_fn(&s->flash, fn, opaque);
}

void tc4x_dmu_error(Tc4xSoc *s, uint32_t err)
{
    tc4x_flash_dmu_error_running(&s->flash, err);
}

void tc4x_pflash_set_maps(Tc4xSoc *s, const Tc4xPflashMap *maps, unsigned n)
{
    tc4x_flash_set_maps(&s->flash, maps, n);
}

static void tc4x_soc_reset_hold(Object *obj, ResetType type)
{
    Tc4xSoc *s = TC4X_SOC(obj);

    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        cpu_reset(CPU(s->cpu[c]));
    }
    /* state that mirrors registers the store resets */
    g_hash_table_remove_all(s->tom_cn0);
    bitmap_zero(s->dma_htre, DMA_NUM_CH);
    s->boot_pc_board = 0;
    s->boot_hold = false;
}

void tc4x_soc_boot(Tc4xSoc *s, uint32_t entry)
{
    s->cpu[0]->env.PC = s->boot_pc_board ? s->boot_pc_board : entry;
    if (s->boot_hold) {
        s->cpu[0]->env.boot_halted = true;
        CPU(s->cpu[0])->halted = 1;
    }
    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        reg_set(s, CPU_SFR_BASE + c * CPU_SFR_STRIDE + CPU_HRA_BOOTCON,
                c == 0 ? 0 : 1);
    }
}

/* GETH0 lines 0..15 -> SRC_GETH0_DMA0..15, 16 -> SRC_GETH0_INTR */
static void tc4x_geth_init(Tc4xSoc *s, MemoryRegion *sysmem)
{
    s->geth = tc4x_geth_create(sysmem);
    for (int i = 0; i < TC4X_GETH_NUM_IRQ; i++) {
        uint32_t src = i == TC4X_GETH_IRQ_MAC ? A_SRC_GETH0_INTR :
                       A_SRC_GETH0_DMA0 + 4 * i;

        sysbus_connect_irq(SYS_BUS_DEVICE(s->geth), i,
                           tc4x_ir_src(s->ir, src));
    }
}

void tc4x_geth_attach(Tc4xSoc *s, unsigned port, uint8_t addr,
                      const Tc4xGethPhyOps *ops, void *opaque)
{
    tc4x_geth_attach_phy(s->geth, port, addr, ops, opaque);
}

/* Register write semantics and kernel resets; later entries replace earlier */
void tc4x_soc_add_semantics(Tc4xSoc *s, const Tc4xRegSemantics *sem,
                            size_t n_sem, const Tc4xKernelReset *kr,
                            size_t n_kr)
{
    tc4x_sfr_store_add_semantics(s->sfr, sem, n_sem, kr, n_kr);
}

static void tc4x_force_soc_status(Tc4xSoc *s)
{
    tc4x_sfr_store_force(s->sfr, A_CLOCK_RAMPSTAT,
                         R_CLOCK_RAMPSTAT_ACTIVE_MASK,
                         R_CLOCK_RAMPSTAT_ACTIVE_MASK);
    for (unsigned n = 0; n < VMT_COUNT; n++) {
        tc4x_sfr_store_force(s->sfr, A_VMT0_MEMDONE + n * VMT_STRIDE,
                             UINT32_MAX, UINT32_MAX);
        tc4x_sfr_store_force(s->sfr, A_VMT0_MEMDONECS + n * VMT_STRIDE,
                             UINT32_MAX, UINT32_MAX);
    }
}

/* Core @c and its view of memory (local DSPR and PSPR) */
static bool tc4x_soc_add_cpu(Tc4xSoc *s, int c, Error **errp)
{
    g_autofree char *name = g_strdup_printf("cpu[%d]", c);
    g_autofree char *root = g_strdup_printf("core%d.view", c);
    Object *cpuobj = object_new(s->cpu_type);

    object_property_add_child(OBJECT(s), name, cpuobj);
    object_unref(cpuobj);
    memory_region_init(&s->cpu_root[c], OBJECT(s), root, UINT64_MAX);
    memory_region_init_alias(&s->cpu_sys_alias[c], OBJECT(s), "sys",
                             get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&s->cpu_root[c], 0,
                                        &s->cpu_sys_alias[c], 0);
    memory_region_init_alias(&s->cpu_local_dspr[c], OBJECT(s), "dspr.local",
                             s->core_ram[c], 0, TC4X_PSPR_OFFSET);
    memory_region_add_subregion_overlap(&s->cpu_root[c], TC4X_LOCAL_DSPR,
                                        &s->cpu_local_dspr[c], 1);
    memory_region_init_alias(&s->cpu_local_pspr[c], OBJECT(s), "pspr.local",
                             s->core_ram[c], TC4X_PSPR_OFFSET,
                             TC4X_CORE_RAM_SIZE - TC4X_PSPR_OFFSET);
    memory_region_add_subregion_overlap(&s->cpu_root[c], TC4X_LOCAL_PSPR,
                                        &s->cpu_local_pspr[c], 1);

    if (!object_property_set_uint(cpuobj, "core-id", c, errp) ||
        !object_property_set_uint(cpuobj, "requester-id",
                                  TC4X_REQUESTER_ID(TC4X_MASTER_CPU(c), false),
                                  errp) ||
        !object_property_set_uint(cpuobj, "requester-id-safe",
                                  TC4X_REQUESTER_ID(TC4X_MASTER_CPU(c), true),
                                  errp) ||
        !object_property_set_bool(cpuobj, "start-halted", c != 0, errp) ||
        !object_property_set_link(cpuobj, "memory", OBJECT(&s->cpu_root[c]),
                                  errp) ||
        !qdev_realize(DEVICE(cpuobj), NULL, errp)) {
        return false;
    }
    s->cpu[c] = TRICORE_CPU(cpuobj);
    return true;
}

/* The interrupt router, over the store at the SRC registers */
static bool tc4x_soc_add_ir(Tc4xSoc *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_TC4X_IR);

    object_property_add_child(OBJECT(s), "ir", OBJECT(dev));
    qdev_prop_set_bit(dev, "imprecise-io", s->imprecise_io);
    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        g_autofree char *name = g_strdup_printf("cpu%d", c);

        object_property_set_link(OBJECT(dev), name, OBJECT(s->cpu[c]),
                                 &error_abort);
    }
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), errp)) {
        return false;
    }
    s->ir = TC4X_IR(dev);
    tc4x_ir_set_watch_fn(s->ir, soc_sfr_watched, s);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, TC4X_IR_SRC_BASE);
    return true;
}

/*
 * The time base: STM and TBU pages over the store, the STM service
 * requests on their router lines
 */
static bool tc4x_soc_add_timebase(Tc4xSoc *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_TC4X_TIMEBASE);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    uint32_t src_stride = A_SRC_STMCPU1_SR0 - A_SRC_STMCPU0_SR0;

    object_property_add_child(OBJECT(s), "timebase", OBJECT(dev));
    qdev_prop_set_bit(dev, "imprecise-io", s->imprecise_io);
    object_property_set_link(OBJECT(dev), "sfr-store", OBJECT(s->sfr),
                             &error_abort);
    object_property_set_link(OBJECT(dev), "ir", OBJECT(s->ir), &error_abort);
    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        g_autofree char *name = g_strdup_printf("cpu%d", c);

        object_property_set_link(OBJECT(dev), name, OBJECT(s->cpu[c]),
                                 &error_abort);
    }
    if (!sysbus_realize_and_unref(sbd, errp)) {
        return false;
    }
    s->timebase = TC4X_TIMEBASE(dev);
    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        sysbus_mmio_map_overlap(sbd, c, CPU_SFR_BASE + c * CPU_SFR_STRIDE, 0);
        for (int k = 0; k < TC4X_STM_IRQS_PER_CPU; k++) {
            qdev_connect_gpio_out_named(dev, "stm",
                                        c * TC4X_STM_IRQS_PER_CPU + k,
                                        tc4x_ir_src(s->ir,
                                                    A_SRC_STMCPU0_SR0 +
                                                    c * src_stride + 4 * k));
        }
    }
    sysbus_mmio_map_overlap(sbd, TC4X_TIMEBASE_TBU_MMIO,
                            A_EGTM_CLS0_TBU_CH0_BASE & TARGET_PAGE_MASK, 0);
    return true;
}

/* The I2C controllers over the store at their kernel registers */
static bool tc4x_soc_add_i2c(Tc4xSoc *s, Error **errp)
{
    for (unsigned n = 0; n < TC4X_I2C_COUNT; n++) {
        g_autofree char *name = g_strdup_printf("i2c[%u]", n);
        DeviceState *dev = qdev_new(TYPE_TC4X_I2C);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        qdev_prop_set_uint32(dev, "index", n);
        qdev_prop_set_bit(dev, "imprecise-io", s->imprecise_io);
        object_property_set_link(OBJECT(dev), "sfr-store", OBJECT(s->sfr),
                                 &error_abort);
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), errp)) {
            return false;
        }
        s->i2c[n] = TC4X_I2C(dev);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, tc4x_i2c_base(n), 0);
    }
    return true;
}

/*
 * The QSPI modules over the store at their registers, their service
 * requests on the router lines, their hardware requests served by the DMA
 */
static bool tc4x_soc_add_qspi(Tc4xSoc *s, Error **errp)
{
    for (unsigned n = 0; n < TC4X_QSPI_COUNT; n++) {
        g_autofree char *name = g_strdup_printf("qspi[%u]", n);
        DeviceState *dev = qdev_new(TYPE_TC4X_QSPI);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
        uint32_t src = A_SRC_QSPI0_TX + n * (A_SRC_QSPI1_TX - A_SRC_QSPI0_TX);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        qdev_prop_set_uint32(dev, "index", n);
        qdev_prop_set_bit(dev, "imprecise-io", s->imprecise_io);
        object_property_set_link(OBJECT(dev), "sfr-store", OBJECT(s->sfr),
                                 &error_abort);
        object_property_set_link(OBJECT(dev), "ir", OBJECT(s->ir),
                                 &error_abort);
        if (!sysbus_realize_and_unref(sbd, errp)) {
            return false;
        }
        s->qspi[n] = TC4X_QSPI(dev);
        tc4x_qspi_set_dma(s->qspi[n], &soc_qspi_dma, s);
        for (unsigned i = 0; i < TC4X_QSPI_IRQ_COUNT; i++) {
            sysbus_connect_irq(sbd, i, tc4x_ir_src(s->ir, src + 4 * i));
        }
        sysbus_mmio_map_overlap(sbd, 0, tc4x_qspi_base(n), 0);
    }
    return true;
}

/*
 * The TMADC kernels over the store at their registers, their service
 * requests on the router lines
 */
static bool tc4x_soc_add_tmadc(Tc4xSoc *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_TC4X_TMADC);
    uint32_t stride = A_SRC_TMADC1_SR0 - A_SRC_TMADC0_SR0;

    object_property_add_child(OBJECT(s), "tmadc", OBJECT(dev));
    qdev_prop_set_bit(dev, "imprecise-io", s->imprecise_io);
    object_property_set_link(OBJECT(dev), "sfr-store", OBJECT(s->sfr),
                             &error_abort);
    object_property_set_link(OBJECT(dev), "timebase", OBJECT(s->timebase),
                             &error_abort);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), errp)) {
        return false;
    }
    s->tmadc = TC4X_TMADC(dev);
    for (unsigned k = 0; k < TC4X_TMADC_COUNT; k++) {
        for (unsigned i = 0; i < TC4X_TMADC_SR_COUNT; i++) {
            qdev_connect_gpio_out_named(dev, "sr",
                                        k * TC4X_TMADC_SR_COUNT + i,
                                        tc4x_ir_src(s->ir, A_SRC_TMADC0_SR0 +
                                                    k * stride + 4 * i));
        }
    }
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, tc4x_tmadc_base(), 0);
    tc4x_sim_add_command(s, "adc", tc4x_tmadc_command, s->tmadc, false);
    return true;
}

/* The GPIO ports over the store, held at the board's straps */
static bool tc4x_soc_add_ports(Tc4xSoc *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_TC4X_PORTS);
    QList *straps = qlist_new();

    object_property_add_child(OBJECT(s), "ports", OBJECT(dev));
    qdev_prop_set_bit(dev, "imprecise-io", s->imprecise_io);
    object_property_set_link(OBJECT(dev), "sfr-store", OBJECT(s->sfr),
                             &error_abort);
    for (uint32_t i = 0; i < s->num_port_straps; i++) {
        qlist_append_int(straps, s->port_straps[i]);
    }
    qdev_prop_set_array(dev, "straps", straps);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), errp)) {
        return false;
    }
    s->ports = TC4X_PORTS(dev);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, tc4x_ports_base(), 0);
    tc4x_sim_add_command(s, "gpio", tc4x_ports_command, s->ports, false);
    return true;
}

/*
 * The SFR store under the SoC's models.  TOM shadow registers SR0/SR1,
 * which cores rewrite often, are plain storage that GLB_CTRL copies to
 * CM0/CM1: their writes may skip the BQL.
 */
static bool tc4x_soc_add_sfr_store(Tc4xSoc *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_TC4X_SFR_STORE);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "sfr-store", OBJECT(dev));
    qdev_prop_set_bit(dev, "imprecise-io", s->imprecise_io);
    if (!sysbus_realize_and_unref(sbd, errp)) {
        return false;
    }
    s->sfr = TC4X_SFR_STORE(dev);
    tc4x_sfr_store_set_models(s->sfr, &soc_sfr_models, s);
    sysbus_mmio_map_overlap(sbd, 0, TC4X_SFR_SEGMENT_E,
                            TC4X_SFR_STORE_PRIORITY);
    sysbus_mmio_map_overlap(sbd, 1, TC4X_SFR_SEGMENT_F,
                            TC4X_SFR_STORE_PRIORITY);
    for (uint32_t cls = 0; cls < EGTM_CLS_COUNT; cls++) {
        for (uint32_t ch = 0; ch < EGTM_TOM_CH_COUNT; ch++) {
            uint32_t base = EGTM_TOM_BASE + cls * EGTM_CLS_STRIDE +
                            ch * EGTM_TOM_CH_STRIDE;

            tc4x_sfr_store_allow_lockless_write(s->sfr, base + EGTM_TOM_SR0);
            tc4x_sfr_store_allow_lockless_write(s->sfr, base + EGTM_TOM_SR1);
        }
    }
    return true;
}

static void tc4x_soc_realize(DeviceState *dev, Error **errp)
{
    Tc4xSoc *s = TC4X_SOC(dev);
    MemoryRegion *sysmem = get_system_memory();
    ObjectClass *cpu_class;

    cpu_class = s->cpu_type ? object_class_by_name(s->cpu_type) : NULL;
    if (!cpu_class || !object_class_dynamic_cast(cpu_class, TYPE_TRICORE_CPU)) {
        error_setg(errp, "cpu-type must name a TriCore CPU");
        return;
    }
    if (s->console >= ASCLIN_COUNT) {
        error_setg(errp, "console must be an ASCLIN instance below %u",
                   ASCLIN_COUNT);
        return;
    }

    if (!tc4x_soc_add_sfr_store(s, errp)) {
        return;
    }
    s->tom_cn0 = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, g_free);
    s->sim_line = g_string_new(NULL);
    s->sim_commands = g_array_new(false, false, sizeof(Tc4xSimCommand));

    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        g_autofree char *name = g_strdup_printf("core%d.spr", c);

        s->core_ram[c] = add_ram(s, &s->own_core_ram[c], s->core_memdev[c],
                                 name, TC4X_CORE_RAM_BASE(c),
                                 TC4X_CORE_RAM_SIZE, errp);
        if (!s->core_ram[c]) {
            return;
        }
    }
    object_property_set_link(OBJECT(&s->flash), "sfr-store", OBJECT(s->sfr),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flash), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flash), 0, TC4X_PFLASH_BASE_C);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flash), 1, TC4X_PFLASH_BASE_NC);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flash), 2, TC4X_DFLASH_BASE);
    s->lmu = add_ram(s, &s->own_lmu, s->lmu_memdev, "lmu", TC4X_LMU_BASE,
                     TC4X_LMU_SIZE, errp);
    if (!s->lmu) {
        return;
    }
    memory_region_init_alias(&s->lmu_alias, OBJECT(s), "lmu.nc", s->lmu, 0,
                             TC4X_LMU_SIZE);
    memory_region_add_subregion(sysmem, TC4X_LMU_NC_BASE, &s->lmu_alias);

    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        if (!tc4x_soc_add_cpu(s, c, errp)) {
            return;
        }
    }
    if (!tc4x_soc_add_ir(s, errp) || !tc4x_soc_add_timebase(s, errp) ||
        !tc4x_soc_add_i2c(s, errp) || !tc4x_soc_add_qspi(s, errp) ||
        !tc4x_soc_add_ports(s, errp) || !tc4x_soc_add_tmadc(s, errp)) {
        return;
    }
    s->bridge_time_request = tc4x_timebase_new_request(s->timebase);
    s->bridge_attrs = tc4x_master_attrs(TC4X_MASTER_DEBUG, false);
    tc4x_force_soc_status(s);
    tc4x_geth_init(s, sysmem);

    qemu_chr_fe_set_handlers(&s->uart, uart_can_receive, uart_receive, NULL,
                             NULL, s, NULL, true);
    qemu_chr_fe_set_handlers(&s->sim, sim_can_receive, sim_receive, NULL,
                             NULL, s, NULL, true);
}

static const Property tc4x_soc_properties[] = {
    DEFINE_PROP_STRING("cpu-type", Tc4xSoc, cpu_type),
    DEFINE_PROP_UINT32("console", Tc4xSoc, console, 0),
    DEFINE_PROP_CHR("console-chardev", Tc4xSoc, uart),
    DEFINE_PROP_CHR("sim-chardev", Tc4xSoc, sim),
    DEFINE_PROP_BOOL("imprecise-io", Tc4xSoc, imprecise_io, false),
    DEFINE_PROP_ARRAY("port-straps", Tc4xSoc, num_port_straps, port_straps,
                      qdev_prop_uint32, uint32_t),
    DEFINE_PROP_LINK("lmu-memdev", Tc4xSoc, lmu_memdev, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
    DEFINE_PROP_LINK("core0-memdev", Tc4xSoc, core_memdev[0],
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("core1-memdev", Tc4xSoc, core_memdev[1],
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("core2-memdev", Tc4xSoc, core_memdev[2],
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("core3-memdev", Tc4xSoc, core_memdev[3],
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("core4-memdev", Tc4xSoc, core_memdev[4],
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("core5-memdev", Tc4xSoc, core_memdev[5],
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("core6-memdev", Tc4xSoc, core_memdev[6],
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
};

static void tc4x_soc_init(Object *obj)
{
    Tc4xSoc *s = TC4X_SOC(obj);

    object_initialize_child(obj, "flash", &s->flash, TYPE_TC4X_FLASH);
    object_property_add_alias(obj, "pflash", OBJECT(&s->flash), "pflash");
    object_property_add_alias(obj, "dflash", OBJECT(&s->flash), "dflash");
    /* set by board code after the SoC is realized */
    object_property_add_link(obj, "dma-policy", TYPE_TC4X_DMA_POLICY,
                             &s->dma_policy, object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
}

static void tc4x_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "Infineon AURIX TC4Dx SoC";
    dc->realize = tc4x_soc_realize;
    /* created by the tc4x machines only */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_soc_reset_hold;
    device_class_set_props(dc, tc4x_soc_properties);
}

static const TypeInfo tc4x_soc_types[] = {
    {
        .name = TYPE_TC4X_DMA_POLICY,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(Tc4xDmaPolicyClass),
    }, {
        .name = TYPE_TC4X_SOC,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(Tc4xSoc),
        .instance_init = tc4x_soc_init,
        .class_init = tc4x_soc_class_init,
    },
};

DEFINE_TYPES(tc4x_soc_types)
