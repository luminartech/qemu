/*
 * Infineon AURIX TC4Dx machine: six TriCore 1.8 cores, their scratchpad
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
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "elf.h"
#include "cpu.h"
#include "trace.h"
#include "hw/tricore/tc4x.h"

#define TC4X_NUM_CORES      6
#define TC4X_CORE_RAM_SIZE  (2 * MiB)       /* DSPR + PSPR (at +1 MiB) */
#define TC4X_PSPR_OFFSET    (1 * MiB)
#define TC4X_LOCAL_DSPR     0xD0000000u
#define TC4X_LOCAL_PSPR     0xC0000000u

#define TC4X_STM_HZ         500000000ULL

#define CPU_SFR_BASE        0xF8800000u
#define CPU_SFR_STRIDE      0x40000u
#define CPU_STM_ABS         0x20u
#define CPU_STM_VM_BASE     0x100u
#define CPU_STM_VM_STRIDE   0x20u
#define CPU_STM_VM_COUNT    8
#define STM_CMP0            0x00u
#define STM_CMP1            0x04u
#define STM_CMCON           0x08u
#define STM_ICR             0x0Cu
#define STM_ISCR            0x10u
#define STM_ISR             0x14u
#define CPU_HRA_PC          0x1FE08u
#define CPU_HRA_CORE_ID     0x1FE1Cu
#define CPU_HRA_BOOTCON     0x1FE60u
#define CPU_HRHV_CORE_ID    0x3FE1Cu

#define SRC_BASE            0xF4432000u
#define SRC_SIZE            0x2000u
#define SRC_STMCPU_BASE     (SRC_BASE + 0x20u)
#define SRC_STMCPU_STRIDE   0x40u
#define SRC_ASCLIN_TX(n)    (SRC_BASE + 0x2B0u + (n) * 12u)
#define SRC_ASCLIN_RX(n)    (SRC_BASE + 0x2B4u + (n) * 12u)
#define SRC_SRPN_MASK       0xFFu
#define SRC_TOS_SHIFT       12
#define SRC_TOS_MASK        0xFu
#define SRC_SRE             BIT(23)
#define SRC_SRR             BIT(24)
#define SRC_CLRR            BIT(25)
#define SRC_SETR            BIT(26)
#define SRC_IOV             BIT(27)
#define SRC_IOVCLR          BIT(28)
#define SRC_WRITABLE        0x0080FFFFu

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

/* DMU flash commands: the command sequences of the iLLD IfxFlash.h */
#define DMU_HCI_STATUS      0xF8040004u
#define DMU_HCI_ERR         0xF8040010u
#define DMU_HCI_CLRERR      0xF8040014u
#define DMU_CMD_BASE        0xF8080000u
#define DMU_CMD_SIZE        0x10000u
#define DMU_CMD_5554        0x5554u
#define DMU_CMD_55F0        0x55F0u
#define DMU_CMD_55F4        0x55F4u
#define DMU_CMD_AA50        0xAA50u
#define DMU_CMD_AA58        0xAA58u
#define DMU_CMD_AAA8        0xAAA8u
#define DMU_STATUS_DFPAGE   BIT(24)
#define DMU_STATUS_PFPAGE   BIT(25)
#define DMU_STATUS_REQDONE  BIT(31)
#define DMU_ERR_SQER        BIT(1)
#define DMU_ERR_PVER        BIT(6)
#define DMU_ERR_EVER        BIT(7)
#define DMU_DFLASH_BASE     0xAE000000u
#define DMU_DF_SECTOR       (2 * KiB)
#define DMU_PF_SECTOR       (16 * KiB)
#define DMU_DF_PAGE         8u
#define DMU_PF_PAGE         32u
#define DMU_DF_WORDLINE     512u
#define DMU_PF_WORDLINE     1024u
#define DMU_PAGE_BUF_SIZE   512     /* IFXFLASH_PFLASH_BURST_LENGTH */

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
 * TMADC kernels: MODSTAT follows the clock enable and MODCFG.RUN;
 * IfxAdc_Tmadc_runModule() waits for the calibration phase (CALPH)
 */
#define ADC_CLKEN_TMADC     0xF5000890u
#define TMADC_BASE          0xF5020000u
#define TMADC_STRIDE        0x800u
#define TMADC_COUNT         8u
#define TMADC_MODCFG        0x000u
#define TMADC_RUN           BIT(1)
#define TMADC_MODSTAT       0x1D4u
#define TMADC_MODSTAT_DIS   BIT(0)
#define TMADC_MODSTAT_RUN   BIT(1)
#define TMADC_MODSTAT_CALPH_DONE (2u << 2)

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

#define QSPI_BASE           0xF4401000u
#define QSPI_STRIDE         0x200u
#define QSPI_COUNT          8u
#define QSPI_CS_COUNT       16u
#define QSPI_STATUS         0x140u
#define QSPI_BACONENTRY     0x15Cu
#define QSPI_DATAENTRY      0x160u
#define QSPI_RXEXIT         0x180u
#define QSPI_RXEXIT_END     0x1A0u
#define QSPI_STATUS_TXF     BIT(9)
#define QSPI_STATUS_RXF     BIT(10)
#define QSPI_STATUS_PT1F    BIT(11)
#define QSPI_STATUS_PT2F    BIT(12)
#define QSPI_BACON_CS_SHIFT 28
#define QSPI_RXLEVEL_SHIFT  20
#define QSPI_RXLEVEL_MAX    15u
#define QSPI_GLOBALCON1     0x114u
#define QSPI_GLOBALCON1_TXEN BIT(9)
#define QSPI_GLOBALCON1_RXEN BIT(10)
#define QSPI_GLOBALCON1_PT2EN BIT(12)
#define SRC_QSPI0_TX        (SRC_BASE + 0x460u)
#define SRC_QSPI0_RX        (SRC_BASE + 0x464u)
#define SRC_QSPI0_PT        (SRC_BASE + 0x46Cu)
#define SRC_QSPI0_U         (SRC_BASE + 0x470u)   /* move counter IAL/IBL */
#define QSPI_MCCON          0x1B8u
#define QSPI_MCCON_IALEN    BIT(20)
#define QSPI_MCCON_IALF     BIT(21)
#define QSPI_MCCON_IALC     BIT(22)
#define QSPI_MCCON_IALS     BIT(23)
#define SRC_QSPI_STRIDE     0x14u

/* eGTM TBU channel 0: 27 bits of the STM time base */
#define EGTM_TBU_CH0        0xF9080108u
#define EGTM_TBU_MASK       0x07FFFFFFu

/*
 * Start-up status reported as done: CLOCK_RAMPSTAT.ACTIVE (IfxClock.c),
 * VMTn_MEMDONE(CS) (the memory test sequence of IfxVmt.h)
 */
#define CLOCK_RAMPSTAT      0xF006420Cu
/*
 * PLLs: STAT.PWRSTAT and STAT.PLLLOCK follow CON0.PLLPWR (lock at once);
 * IfxClock.c waits for both while it powers the PLLs up
 */
#define CLOCK_SYSPLLCON0    0xF0064300u
#define CLOCK_SYSPLLSTAT    0xF006430Cu
#define CLOCK_PERPLLCON0    0xF0064380u
#define CLOCK_PERPLLSTAT    0xF006438Cu
#define CLOCK_PLLCON0_PLLPWR BIT(0)
#define CLOCK_PLLSTAT_PWRSTAT BIT(0)
#define CLOCK_PLLSTAT_PLLLOCK BIT(1)
#define CLOCK_RAMPSTAT_ACTIVE BIT(23)
#define VMT_BASE            0xF0400000u
#define VMT_STRIDE          0x20000u
#define VMT_COUNT           7u
#define VMT_MEMDONE         0x8Cu
#define VMT_MEMDONECS       0xC0u

#define TYPE_TC4X_MACHINE MACHINE_TYPE_NAME("tc4x")
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xSoc, TC4X_MACHINE)

typedef struct Tc4xTomCounter {
    uint32_t start;     /* CN0 value written by software */
    int64_t t0_ns;      /* guest time of that write */
} Tc4xTomCounter;

typedef struct Tc4xStmTimer {
    struct Tc4xSoc *s;
    QEMUTimer *timer;
    uint8_t core;
    uint8_t vm;
    uint8_t cmp;
    bool fired;
    uint64_t fired_target;  /* compare value of the last match */
} Tc4xStmTimer;

/*
 * SFR storage: 4 KiB pages of 32-bit words in a two-level table, allocated
 * on first use and never freed.  Values and per-word flags are atomic: the
 * cores' interrupt acknowledge updates SRC registers without the BQL.
 */
#define SFR_PAGE_BITS       12
#define SFR_PAGE_WORDS      (1u << (SFR_PAGE_BITS - 2))
#define SFR_L2_BITS         10
#define SFR_L2_SIZE         (1u << SFR_L2_BITS)
#define SFR_L1_SIZE         (1u << (32 - SFR_PAGE_BITS - SFR_L2_BITS))

/* Bits pinned for reads whatever the register holds */
typedef struct Tc4xSfrForces {
    uint32_t value[SFR_PAGE_WORDS];
    uint32_t mask[SFR_PAGE_WORDS];
} Tc4xSfrForces;

typedef struct Tc4xSfrPage {
    uint32_t val[SFR_PAGE_WORDS];
    uint32_t reads[SFR_PAGE_WORDS];     /* since the last "hot" query */
    /* written, or its first read logged as unimplemented */
    unsigned long known[BITS_TO_LONGS(SFR_PAGE_WORDS)];
    Tc4xSfrForces *forces;
} Tc4xSfrPage;

typedef struct Tc4xQspiSlave {
    const Tc4xQspiSlaveOps *ops;
    void *opaque;
} Tc4xQspiSlave;

typedef struct Tc4xSimCommand {
    const char *prefix;
    Tc4xSimCommandFn fn;
    void *opaque;
    bool qspi_lock;
} Tc4xSimCommand;

struct Tc4xSoc {
    MachineState parent_obj;

    TriCoreCPU *cpu[TC4X_NUM_CORES];
    MemoryRegion cpu_root[TC4X_NUM_CORES];
    MemoryRegion cpu_sys_alias[TC4X_NUM_CORES];
    MemoryRegion cpu_local_dspr[TC4X_NUM_CORES];
    MemoryRegion cpu_local_pspr[TC4X_NUM_CORES];
    MemoryRegion core_ram[TC4X_NUM_CORES];
    MemoryRegion seg1_ram;
    MemoryRegion pflash, pflash_alias;
    MemoryRegion dflash;
    MemoryRegion lmu, lmu_alias;
    MemoryRegion sfr_e, sfr_f;

    Tc4xSfrPage **sfr_l1[SFR_L1_SIZE];
    GHashTable *tom_cn0;   /* TOM CN0 addr -> Tc4xTomCounter */
    GHashTable *watches;
    /*
     * Interrupt router.  Cores acknowledge interrupts without the BQL, so
     * pending, cpu_prio and the SRC registers' SRR bits are under ir_lock.
     */
    QemuRecMutex ir_lock;
    GHashTable *pending;   /* SRC addrs with SRR set */
    uint32_t cpu_prio[TC4X_NUM_CORES];

    Tc4xStmTimer stm[TC4X_NUM_CORES][CPU_STM_VM_COUNT][2];

    unsigned console;       /* ASCLIN instance on serial0 */
    CharFrontend uart;
    uint8_t rx_fifo[ASCLIN_RXFIFO_DEPTH];
    unsigned rx_head, rx_count;

    CharFrontend sim;
    GString *sim_line;
    GArray *sim_commands;   /* Tc4xSimCommand */

    GQueue *qspi_rx[QSPI_COUNT];
    Tc4xQspiSlave qspi_slave[QSPI_COUNT][QSPI_CS_COUNT];
    int qspi_cs[QSPI_COUNT];          /* chip select of the frame, -1: none */
    bool qspi_rx_fed[QSPI_COUNT];     /* receive data fed by board code */
    bool qspi_dma_busy[QSPI_COUNT];   /* hardware-request DMA job in progress */
    /* QSPI registers, RX queues and slaves, taken after the BQL */
    QemuRecMutex qspi_lock;

    uint8_t page_buf[DMU_PAGE_BUF_SIZE];
    unsigned page_len;
    uint32_t cmd_addr;
    uint32_t cmd_count;
    bool cmd_armed;     /* AAA8 <- 0xA0 / 0x80 seen, waiting for the opcode */
    uint32_t cmd_class;

    uint32_t boot_pc;
    char *sfr_script;
};

static void *sfr_publish(void **slot, size_t size)
{
    void *fresh = g_malloc0(size);
    void *old = qatomic_cmpxchg(slot, NULL, fresh);

    if (old) {
        g_free(fresh);
        return old;
    }
    return fresh;
}

static Tc4xSfrPage *sfr_page(Tc4xSoc *s, uint32_t addr, bool alloc)
{
    Tc4xSfrPage ***l1 = &s->sfr_l1[addr >> (SFR_PAGE_BITS + SFR_L2_BITS)];
    Tc4xSfrPage **l2 = qatomic_load_acquire(l1);
    Tc4xSfrPage **slot;
    Tc4xSfrPage *pg;

    if (!l2) {
        if (!alloc) {
            return NULL;
        }
        l2 = sfr_publish((void **)l1, SFR_L2_SIZE * sizeof(*l2));
    }
    slot = &l2[(addr >> SFR_PAGE_BITS) & (SFR_L2_SIZE - 1)];
    pg = qatomic_load_acquire(slot);
    if (!pg && alloc) {
        pg = sfr_publish((void **)slot, sizeof(*pg));
    }
    return pg;
}

static unsigned sfr_word(uint32_t addr)
{
    return (addr >> 2) & (SFR_PAGE_WORDS - 1);
}

static uint32_t reg_get(Tc4xSoc *s, uint32_t addr)
{
    Tc4xSfrPage *pg = sfr_page(s, addr, false);

    return pg ? qatomic_read(&pg->val[sfr_word(addr)]) : 0;
}

static void reg_set(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    Tc4xSfrPage *pg = sfr_page(s, addr, true);

    qatomic_set(&pg->val[sfr_word(addr)], val);
    set_bit_atomic(sfr_word(addr), pg->known);
}

/* Pin the bits in @mask of @addr to @value for every read (mask 0: release) */
static void reg_force(Tc4xSoc *s, uint32_t addr, uint32_t value,
                      uint32_t mask)
{
    Tc4xSfrPage *pg = sfr_page(s, addr, true);
    Tc4xSfrForces *f = sfr_publish((void **)&pg->forces, sizeof(*f));

    qatomic_set(&f->value[sfr_word(addr)], value);
    qatomic_set(&f->mask[sfr_word(addr)], mask);
}

static uint32_t reg_apply_force(Tc4xSfrPage *pg, unsigned w, uint32_t v)
{
    Tc4xSfrForces *f = pg ? qatomic_load_acquire(&pg->forces) : NULL;
    uint32_t mask = f ? qatomic_read(&f->mask[w]) : 0;

    return mask ? (v & ~mask) | (qatomic_read(&f->value[w]) & mask) : v;
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

static void ir_update(Tc4xSoc *s)
{
    uint32_t best[TC4X_NUM_CORES] = { 0 };
    GHashTableIter it;
    gpointer key;

    QEMU_LOCK_GUARD(&s->ir_lock);
    g_hash_table_iter_init(&it, s->pending);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        uint32_t v = reg_get(s, GPOINTER_TO_UINT(key));
        uint32_t tos = (v >> SRC_TOS_SHIFT) & SRC_TOS_MASK;
        uint32_t prio = v & SRC_SRPN_MASK;

        if ((v & SRC_SRE) && tos < TC4X_NUM_CORES && prio > best[tos]) {
            best[tos] = prio;
        }
    }
    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        if (best[c] != s->cpu_prio[c]) {
            qatomic_set(&s->cpu_prio[c], best[c]);
            tricore_cpu_set_pending_irq(s->cpu[c], best[c]);
        }
    }
}

static void src_set_srr(Tc4xSoc *s, uint32_t addr, bool set)
{
    uint32_t v;

    QEMU_LOCK_GUARD(&s->ir_lock);
    v = reg_get(s, addr);
    if (set) {
        if (v & SRC_SRR) {
            v |= SRC_IOV;
        }
        v |= SRC_SRR;
        g_hash_table_add(s->pending, GUINT_TO_POINTER(addr));
    } else {
        v &= ~SRC_SRR;
        g_hash_table_remove(s->pending, GUINT_TO_POINTER(addr));
    }
    reg_set(s, addr, v);
    ir_update(s);
}

/* Called by a core taking an interrupt, without the BQL */
static void ir_ack(void *opaque, uint32_t core, uint32_t prio)
{
    Tc4xSoc *s = opaque;
    GHashTableIter it;
    gpointer key;

    QEMU_LOCK_GUARD(&s->ir_lock);
    g_hash_table_iter_init(&it, s->pending);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        uint32_t src = GPOINTER_TO_UINT(key);
        uint32_t v = reg_get(s, src);

        if ((v & SRC_SRE) && (v & SRC_SRPN_MASK) == prio &&
            ((v >> SRC_TOS_SHIFT) & SRC_TOS_MASK) == core) {
            src_set_srr(s, src, false);
            return;
        }
    }
}

static void src_write(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    uint32_t v;

    QEMU_LOCK_GUARD(&s->ir_lock);
    v = (reg_get(s, addr) & ~SRC_WRITABLE) | (val & SRC_WRITABLE);

    if (val & SRC_IOVCLR) {
        v &= ~SRC_IOV;
    }
    reg_set(s, addr, v);
    if (val & SRC_SETR) {
        src_set_srr(s, addr, true);
    } else if (val & SRC_CLRR) {
        src_set_srr(s, addr, false);
    } else {
        ir_update(s);
    }
}

static int64_t guest_ns(Tc4xSoc *s)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int64_t host_ns_for_guest(Tc4xSoc *s, int64_t ns)
{
    return ns;
}

static uint64_t stm_now(Tc4xSoc *s)
{
    /* read on every timer poll: keep it a division by a constant */
    QEMU_BUILD_BUG_ON(NANOSECONDS_PER_SECOND % TC4X_STM_HZ);
    return guest_ns(s) / (NANOSECONDS_PER_SECOND / TC4X_STM_HZ);
}

static uint32_t stm_vm_reg(int core, int vm, uint32_t off)
{
    return CPU_SFR_BASE + core * CPU_SFR_STRIDE + CPU_STM_VM_BASE +
           vm * CPU_STM_VM_STRIDE + off;
}

#define STM_LATE_WINDOW     (TC4X_STM_HZ / 10)     /* 100 ms */

static void stm_schedule(Tc4xSoc *s, int core, int vm, int cmp)
{
    Tc4xStmTimer *t = &s->stm[core][vm][cmp];
    uint32_t icr = reg_get(s, stm_vm_reg(core, vm, STM_ICR));
    uint32_t cmcon = reg_get(s, stm_vm_reg(core, vm, STM_CMCON)) >>
                     (cmp ? 16 : 0);
    uint32_t msize = cmcon & 0x1F;
    uint32_t mstart = (cmcon >> 8) & 0x3F;
    uint64_t mask = (2ULL << msize) - 1;
    uint64_t now = stm_now(s);
    uint64_t cur, target, delta, due;

    if (!(icr & (cmp ? BIT(4) : BIT(0)))) {
        timer_del(t->timer);
        return;
    }
    target = reg_get(s, stm_vm_reg(core, vm, cmp ? STM_CMP1 : STM_CMP0)) &
             mask;
    cur = (now >> mstart) & mask;
    delta = (target - cur) & mask;
    if (delta == 0) {
        delta = mask + 1;
    }
    /*
     * A compare can be found slightly behind the counter without having
     * matched: software that re-arms with CMP = ABS + period can be
     * preempted for a period in between.  Rather than waiting 8.6 s for the
     * 32-bit wrap, treat it as due now unless this value already fired.
     */
    if (mask > 2 * STM_LATE_WINDOW &&
        ((cur - target) & mask) < STM_LATE_WINDOW &&
        !(t->fired && (t->fired_target & mask) == target)) {
        delta = 1;
    }
    due = (now >> mstart << mstart) + (delta << mstart);
    timer_mod(t->timer, host_ns_for_guest(s, muldiv64(due,
                                                      NANOSECONDS_PER_SECOND,
                                                      TC4X_STM_HZ)));
}

static void stm_fire(void *opaque)
{
    Tc4xStmTimer *t = opaque;
    Tc4xSoc *s = t->s;
    uint32_t isr_addr = stm_vm_reg(t->core, t->vm, STM_ISR);
    uint32_t icr = reg_get(s, stm_vm_reg(t->core, t->vm, STM_ICR));
    uint32_t os = (icr >> (t->cmp ? 5 : 1)) & 1;

    reg_set(s, isr_addr, reg_get(s, isr_addr) | BIT(t->cmp));
    t->fired = true;
    t->fired_target = reg_get(s, stm_vm_reg(t->core, t->vm,
                                            t->cmp ? STM_CMP1 : STM_CMP0));
    src_set_srr(s, SRC_STMCPU_BASE + t->core * SRC_STMCPU_STRIDE +
                   (2 * t->vm + os) * 4, true);
    stm_schedule(s, t->core, t->vm, t->cmp);
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
    src_set_srr(s, SRC_ASCLIN_RX(s->console), true);
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
        src_set_srr(s, SRC_ASCLIN_TX(n), true);
    } else if (off == ASCLIN_FLAGSCLEAR) {
        reg_set(s, flags, reg_get(s, flags) & ~val);
    } else if (off == ASCLIN_FLAGSSET) {
        reg_set(s, flags, reg_get(s, flags) | val);
    } else {
        return false;
    }
    return true;
}

static void dmu_status(Tc4xSoc *s, uint32_t set, uint32_t clr)
{
    reg_set(s, DMU_HCI_STATUS, (reg_get(s, DMU_HCI_STATUS) & ~clr) | set);
}

static void dmu_error(Tc4xSoc *s, uint32_t err)
{
    reg_set(s, DMU_HCI_ERR, reg_get(s, DMU_HCI_ERR) | err);
}

/* A command fails with SQER unless its range lies in one flash */
static bool dmu_in_flash(Tc4xSoc *s, uint32_t addr, uint64_t len)
{
    MemoryRegionSection sec;
    bool ok;

    if (len == 0) {
        return true;
    }
    sec = memory_region_find(get_system_memory(), addr, len);
    ok = (sec.mr == &s->pflash || sec.mr == &s->dflash) &&
         int128_get64(sec.size) == len;
    if (sec.mr) {
        memory_region_unref(sec.mr);
    }
    return ok;
}

static bool dmu_range_is(uint32_t addr, uint64_t len, uint8_t value)
{
    uint8_t buf[256];

    while (len) {
        uint32_t n = MIN(len, sizeof(buf));

        address_space_read(&address_space_memory, addr,
                           MEMTXATTRS_UNSPECIFIED, buf, n);
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != value) {
                return false;
            }
        }
        addr += n;
        len -= n;
    }
    return true;
}

static uint32_t dmu_sector_size(uint32_t addr)
{
    return (addr & 0x0F000000u) == (DMU_DFLASH_BASE & 0x0F000000u) ?
           DMU_DF_SECTOR : DMU_PF_SECTOR;
}

static void dmu_execute(Tc4xSoc *s, uint32_t class, uint32_t opcode)
{
    uint32_t addr = s->cmd_addr;

    if (class == 0xA0 && (opcode == 0xAA || opcode == 0xA6)) {
        /* Write page / write burst: program the loaded page buffer */
        if (!dmu_in_flash(s, addr, s->page_len)) {
            dmu_error(s, DMU_ERR_SQER);
        } else {
            if (!dmu_range_is(addr, s->page_len, 0)) {
                dmu_error(s, DMU_ERR_PVER);
            }
            address_space_write(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, s->page_buf,
                                s->page_len);
        }
        s->page_len = 0;
        dmu_status(s, DMU_STATUS_REQDONE,
                   DMU_STATUS_DFPAGE | DMU_STATUS_PFPAGE);
    } else if (class == 0x80 && opcode == 0x50) {
        /* Erase logical sector range: erased flash reads as zero */
        uint64_t len = (uint64_t)s->cmd_count * dmu_sector_size(addr);

        if (dmu_in_flash(s, addr, len)) {
            address_space_set(&address_space_memory, addr, 0, len,
                              MEMTXATTRS_UNSPECIFIED);
        } else {
            dmu_error(s, DMU_ERR_SQER);
        }
        dmu_status(s, DMU_STATUS_REQDONE, 0);
    } else if (class == 0x80 &&
               (opcode == 0x56 || opcode == 0x58 || opcode == 0x5F)) {
        /*
         * Verify erased page (0x56) / wordline (0x58) / logical sector
         * range (0x5F); the page form takes count 0
         */
        bool df = dmu_sector_size(addr) == DMU_DF_SECTOR;
        uint32_t page = df ? DMU_DF_PAGE : DMU_PF_PAGE;
        uint32_t wordline = df ? DMU_DF_WORDLINE : DMU_PF_WORDLINE;
        uint64_t len = opcode == 0x56 ? page :
                       opcode == 0x58 ? wordline :
                       (uint64_t)s->cmd_count * dmu_sector_size(addr);

        if (!dmu_in_flash(s, addr, len)) {
            dmu_error(s, DMU_ERR_SQER);
        } else if (!dmu_range_is(addr, len, 0)) {
            dmu_error(s, DMU_ERR_EVER);
        }
        dmu_status(s, DMU_STATUS_REQDONE, 0);
    } else {
        qemu_log_mask(LOG_UNIMP, "tc4x-dmu: command 0x%02x/0x%02x at 0x%08x "
                      "completes without effect\n", class, opcode, addr);
        dmu_status(s, DMU_STATUS_REQDONE, 0);
    }
}

static void dmu_cmd_write(Tc4xSoc *s, uint32_t off, uint32_t val)
{
    switch (off) {
    case DMU_CMD_5554:
        if (val == 0xF0) {              /* reset to read */
            s->page_len = 0;
            s->cmd_armed = false;
            dmu_status(s, 0, DMU_STATUS_DFPAGE | DMU_STATUS_PFPAGE);
        } else if (val == 0xFA) {       /* clear status (F5: resume protection) */
            dmu_status(s, 0, DMU_STATUS_REQDONE);
            reg_set(s, DMU_HCI_ERR, 0);
        } else if (val == 0x5D || val == 0x50) {  /* enter page mode */
            s->page_len = 0;
            dmu_status(s, val == 0x5D ? DMU_STATUS_DFPAGE : DMU_STATUS_PFPAGE,
                       DMU_STATUS_REQDONE);
        }
        break;
    case DMU_CMD_55F0:
    case DMU_CMD_55F4:                  /* load page, 64-bit as two words */
        if (s->page_len + 4 <= DMU_PAGE_BUF_SIZE) {
            stl_le_p(&s->page_buf[s->page_len], val);
            s->page_len += 4;
        } else {
            dmu_error(s, DMU_ERR_SQER);
        }
        break;
    case DMU_CMD_AA50:
        s->cmd_addr = val;
        s->cmd_armed = false;
        break;
    case DMU_CMD_AA58:
        s->cmd_count = val;
        break;
    case DMU_CMD_AAA8:
        if (!s->cmd_armed) {
            s->cmd_class = val;
            s->cmd_armed = true;
        } else {
            s->cmd_armed = false;
            dmu_status(s, 0, DMU_STATUS_REQDONE);
            dmu_execute(s, s->cmd_class, val);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "tc4x-dmu: command cycle +0x%04x <- 0x%08x\n",
                      off, val);
        break;
    }
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

/*
 * Execute one transaction of channel ch.  Returns true if the channel reloaded
 * a linked TCS from SHADR; *auto_start reports that the loaded TCS has SCH set.
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
    uint8_t buf[1u << 7];   /* CHDW 6 and 7 are reserved but encodable */

    *auto_start = false;
    for (uint32_t i = 0; i < moves; i++) {
        address_space_read(&address_space_memory, sadr, MEMTXATTRS_UNSPECIFIED,
                           buf, width);
        address_space_write(&address_space_memory, dadr, MEMTXATTRS_UNSPECIFIED,
                            buf, width);
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

    if (extract32(adicr, 26, 2) & 2) {
        reg_set(s, dma_reg(ch, DMA_CHCSR),
                reg_get(s, dma_reg(ch, DMA_CHCSR)) | DMA_CHCSR_ICH);
        src_set_srr(s, dma_src(ch), true);
    }

    if (extract32(adicr, 16, 4) == DMA_SHCT_LINKED) {
        uint32_t tcs[8];

        address_space_read(&address_space_memory,
                           reg_get(s, dma_reg(ch, DMA_SHADR)),
                           MEMTXATTRS_UNSPECIFIED, tcs, sizeof(tcs));
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

static bool qspi_decode(uint32_t addr, uint32_t *n, uint32_t *off)
{
    if (addr < QSPI_BASE || addr >= QSPI_BASE + QSPI_COUNT * QSPI_STRIDE) {
        return false;
    }
    *n = (addr - QSPI_BASE) / QSPI_STRIDE;
    *off = (addr - QSPI_BASE) % QSPI_STRIDE;
    return true;
}

/*
 * TX / RX FIFO events of QSPIn as CPU interrupts: with TXEN / RXEN set and
 * the service request routed to a CPU, each word (sent at once, answered at
 * once) raises TX (FIFO empty again) and RX (word received), as an
 * interrupt-driven SPI master expects.  Requests routed to the DMA are left
 * to qspi_dma_kick.
 */
static void qspi_cpu_events(Tc4xSoc *s, uint32_t n)
{
    uint32_t con1 = reg_get(s, QSPI_BASE + n * QSPI_STRIDE + QSPI_GLOBALCON1);
    static const struct {
        uint32_t enable, src;
    } ev[] = {
        { QSPI_GLOBALCON1_TXEN, SRC_QSPI0_TX },
        { QSPI_GLOBALCON1_RXEN, SRC_QSPI0_RX },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(ev); i++) {
        uint32_t src = ev[i].src + n * SRC_QSPI_STRIDE;
        uint32_t tos = (reg_get(s, src) >> SRC_TOS_SHIFT) & SRC_TOS_MASK;

        if ((con1 & ev[i].enable) && (reg_get(s, src) & SRC_SRE) &&
            tos < TC4X_NUM_CORES) {
            src_set_srr(s, src, true);
        }
    }
}

/* The slave of the current frame of QSPIn, NULL when none answers */
static Tc4xQspiSlave *qspi_selected(Tc4xSoc *s, uint32_t n)
{
    Tc4xQspiSlave *sl;

    if (s->qspi_cs[n] < 0) {
        return NULL;
    }
    sl = &s->qspi_slave[n][s->qspi_cs[n]];
    return sl->ops ? sl : NULL;
}

/* BACONENTRY starts a frame on the chip select it names */
static void qspi_bacon(Tc4xSoc *s, uint32_t n, uint32_t bacon)
{
    Tc4xQspiSlave *sl;

    s->qspi_cs[n] = bacon >> QSPI_BACON_CS_SHIFT;
    sl = qspi_selected(s, n);
    if (sl) {
        sl->ops->frame(sl->opaque, bacon);
    }
}

static void qspi_transmit(Tc4xSoc *s, uint32_t n, uint32_t val)
{
    Tc4xQspiSlave *sl = qspi_selected(s, n);
    uint32_t reply = 0;

    if (sl) {
        reply = sl->ops->word(sl->opaque, val);
    }
    g_queue_push_tail(s->qspi_rx[n], GUINT_TO_POINTER(reply));
    qspi_cpu_events(s, n);
}

/*
 * DMA-driven SPI jobs: the TX and RX DMA channels wait for QSPI hardware
 * requests (TSR.HTRE) and the CPU starts the frame by writing BACONENTRY.
 * Run the TX list (feeding DATAENTRY/BACONENTRY), then the RX list (draining
 * RXEXIT), then signal end of job with the phase-transition-2 interrupt.
 */
static bool qspi_dma_side(Tc4xSoc *s, uint32_t ch, uint32_t n, bool tx)
{
    uint32_t base = QSPI_BASE + n * QSPI_STRIDE;
    uint32_t a;

    if (!(reg_get(s, dma_tsr(ch)) & DMA_TSR_HTRE)) {
        return false;
    }
    a = reg_get(s, dma_reg(ch, tx ? DMA_DADR : DMA_SADR));
    return tx ? a >= base + QSPI_BACONENTRY && a < base + QSPI_RXEXIT
              : a >= base + QSPI_RXEXIT && a < base + QSPI_RXEXIT_END;
}

static void qspi_dma_kick(Tc4xSoc *s, uint32_t n)
{
    uint32_t base = QSPI_BASE + n * QSPI_STRIDE;
    uint32_t mccon;
    bool ran = false;

    if (s->qspi_dma_busy[n] || s->qspi_rx_fed[n]) {
        return;     /* written by the TX list itself / fed by board code */
    }
    s->qspi_dma_busy[n] = true;
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
        mccon = reg_get(s, base + QSPI_MCCON);
        /* Frame-based CS jobs end with IAL (move counter), others with PT2 */
        if (reg_get(s, base + QSPI_GLOBALCON1) & QSPI_GLOBALCON1_PT2EN) {
            src_set_srr(s, SRC_QSPI0_PT + n * SRC_QSPI_STRIDE, true);
        } else if (mccon & QSPI_MCCON_IALEN) {
            reg_set(s, base + QSPI_MCCON, mccon | QSPI_MCCON_IALF);
            src_set_srr(s, SRC_QSPI0_U + n * SRC_QSPI_STRIDE, true);
        }
    }
    s->qspi_dma_busy[n] = false;
}

static uint32_t qspi_status(Tc4xSoc *s, uint32_t n, uint32_t stored)
{
    uint32_t level = MIN(g_queue_get_length(s->qspi_rx[n]), QSPI_RXLEVEL_MAX);

    stored &= ~(0xFu << QSPI_RXLEVEL_SHIFT) & ~(0xFu << 16);
    /* TX completes instantly: TX FIFO empty, phase transitions done */
    stored |= QSPI_STATUS_TXF | QSPI_STATUS_PT1F | QSPI_STATUS_PT2F;
    return stored | (level << QSPI_RXLEVEL_SHIFT) |
           (level ? QSPI_STATUS_RXF : 0);
}

void tc4x_qspi_attach(Tc4xSoc *s, unsigned module, unsigned cs,
                      const Tc4xQspiSlaveOps *ops, void *opaque)
{
    assert(module < QSPI_COUNT && cs < QSPI_CS_COUNT);
    QEMU_LOCK_GUARD(&s->qspi_lock);
    s->qspi_slave[module][cs] = (Tc4xQspiSlave) { ops, opaque };
}

void tc4x_qspi_set_rx_fed(Tc4xSoc *s, unsigned module, bool fed)
{
    assert(module < QSPI_COUNT);
    QEMU_LOCK_GUARD(&s->qspi_lock);
    s->qspi_rx_fed[module] = fed;
}

void tc4x_qspi_lock(Tc4xSoc *s)
{
    qemu_rec_mutex_lock(&s->qspi_lock);
}

void tc4x_qspi_unlock(Tc4xSoc *s)
{
    qemu_rec_mutex_unlock(&s->qspi_lock);
}

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
    int64_t now = guest_ns(s);
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
    c->t0_ns = guest_ns(s);
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

static uint32_t sfr_read_word_locked(Tc4xSoc *s, uint32_t addr);

static uint32_t sfr_read_word(Tc4xSoc *s, uint32_t addr)
{
    uint32_t qn, qoff;

    if (qspi_decode(addr, &qn, &qoff)) {
        QEMU_LOCK_GUARD(&s->qspi_lock);
        return sfr_read_word_locked(s, addr);
    }
    return sfr_read_word_locked(s, addr);
}

static uint32_t sfr_read_word_locked(Tc4xSoc *s, uint32_t addr)
{
    uint32_t qn, qoff;
    uint32_t v = reg_get(s, addr);
    uint32_t off;
    int core;

    if (addr == EGTM_TBU_CH0) {
        v = stm_now(s) & EGTM_TBU_MASK;
    } else if (sfr_is_cpu(addr, &core, &off)) {
        if (off == CPU_STM_ABS) {
            v = (uint32_t)stm_now(s);
        } else if (off == CPU_STM_ABS + 4) {
            v = stm_now(s) >> 32;
        } else if (off == CPU_HRA_CORE_ID || off == CPU_HRHV_CORE_ID) {
            v = core;
        } else if (off == CPU_HRA_BOOTCON) {
            v = (v & ~1u) | (s->cpu[core]->env.boot_halted ? 1 : 0);
        }
    } else if (addr >= TMADC_BASE &&
               addr < TMADC_BASE + TMADC_COUNT * TMADC_STRIDE &&
               (addr - TMADC_BASE) % TMADC_STRIDE == TMADC_MODSTAT) {
        uint32_t n = (addr - TMADC_BASE) / TMADC_STRIDE;
        bool clk = reg_get(s, ADC_CLKEN_TMADC) & BIT(n);
        bool run = reg_get(s, TMADC_BASE + n * TMADC_STRIDE + TMADC_MODCFG) &
                   TMADC_RUN;

        /*
         * The power-on calibration phase completes at once: MODSTAT.CALPH
         * reads 10B (calibration completed) as soon as the clock runs.
         */
        v = (clk ? TMADC_MODSTAT_CALPH_DONE : TMADC_MODSTAT_DIS) |
            (clk && run ? TMADC_MODSTAT_RUN : 0);
    } else if (tom_is_cn0(addr)) {
        v = tom_cn0_read(s, addr);
    } else if (qspi_decode(addr, &qn, &qoff) && qoff == QSPI_STATUS) {
        v = qspi_status(s, qn, v);
    } else if (qspi_decode(addr, &qn, &qoff) && qoff >= QSPI_RXEXIT &&
               qoff < QSPI_RXEXIT_END) {
        v = g_queue_is_empty(s->qspi_rx[qn]) ? 0 :
            GPOINTER_TO_UINT(g_queue_pop_head(s->qspi_rx[qn]));
    } else if (addr == CLOCK_SYSPLLSTAT || addr == CLOCK_PERPLLSTAT) {
        uint32_t con0 = reg_get(s, addr == CLOCK_SYSPLLSTAT ?
                                CLOCK_SYSPLLCON0 : CLOCK_PERPLLCON0);
        uint32_t on = CLOCK_PLLSTAT_PWRSTAT | CLOCK_PLLSTAT_PLLLOCK;

        v = (v & ~on) | ((con0 & CLOCK_PLLCON0_PLLPWR) ? on : 0);
    } else if (asclin_read(s, addr, &v)) {
        /* served by the ASCLIN model */
    } else {
        Tc4xSfrPage *pg = sfr_page(s, addr, true);
        unsigned long bit = BIT_MASK(sfr_word(addr));

        if (!(qatomic_fetch_or(&pg->known[BIT_WORD(sfr_word(addr))], bit) &
              bit)) {
            qemu_log_mask(LOG_UNIMP, "tc4x-sfr: first read of 0x%08x\n", addr);
        }
    }

    return reg_apply_force(sfr_page(s, addr, false), sfr_word(addr), v);
}

/* Writes of the QSPI modules: true when the write is consumed */
static bool qspi_write(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    uint32_t n, off;

    if (!qspi_decode(addr, &n, &off)) {
        return false;
    }
    if (off >= QSPI_DATAENTRY && off < QSPI_RXEXIT) {
        qspi_transmit(s, n, val);
        return true;
    }
    if (off == QSPI_MCCON) {
        uint32_t flags = reg_get(s, addr) & QSPI_MCCON_IALF;

        if (val & QSPI_MCCON_IALC) {
            flags = 0;
        }
        if (val & QSPI_MCCON_IALS) {
            flags = QSPI_MCCON_IALF;
        }
        reg_set(s, addr, (val & ~(QSPI_MCCON_IALF | QSPI_MCCON_IALC |
                                  QSPI_MCCON_IALS)) | flags);
        return true;
    }
    if (off == QSPI_BACONENTRY) {
        qspi_bacon(s, n, val);
        qspi_dma_kick(s, n);
    }
    return false;
}

static void sfr_write_word_locked(Tc4xSoc *s, uint32_t addr,
                                  uint32_t val);

static void sfr_write_word(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    uint32_t qn, qoff;

    if (qspi_decode(addr, &qn, &qoff)) {
        QEMU_LOCK_GUARD(&s->qspi_lock);
        sfr_write_word_locked(s, addr, val);
        return;
    }
    sfr_write_word_locked(s, addr, val);
}

static void sfr_write_word_locked(Tc4xSoc *s, uint32_t addr,
                                  uint32_t val)
{
    uint32_t off;
    int core;

    if (addr >= SRC_BASE && addr < SRC_BASE + SRC_SIZE) {
        src_write(s, addr, val);
        return;
    }
    if (asclin_write(s, addr, val)) {
        return;
    }
    if (dma_write(s, addr, val)) {
        return;
    }
    if (tom_is_cn0(addr)) {
        tom_cn0_write(s, addr, val);
        return;
    }
    if (tom_is_glb_ctrl(addr)) {
        tom_glb_ctrl_write(s, addr, val);       /* then stored as written */
    }
    if (qspi_write(s, addr, val)) {
        return;
    }
    if (addr >= DMU_CMD_BASE && addr < DMU_CMD_BASE + DMU_CMD_SIZE) {
        dmu_cmd_write(s, addr - DMU_CMD_BASE, val);
        return;
    }
    if (addr == DMU_HCI_CLRERR) {
        reg_set(s, DMU_HCI_ERR, reg_get(s, DMU_HCI_ERR) & ~val);
        return;
    }

    reg_set(s, addr, val);

    if (!sfr_is_cpu(addr, &core, &off)) {
        return;
    }
    if (off == CPU_HRA_BOOTCON && !(val & 1) &&
        s->cpu[core]->env.boot_halted) {
        uint32_t pc = reg_get(s, addr - CPU_HRA_BOOTCON + CPU_HRA_PC) & ~1u;

        tricore_cpu_release_boot_halt(s->cpu[core], pc);
    } else if (off >= CPU_STM_VM_BASE &&
               off < CPU_STM_VM_BASE + CPU_STM_VM_COUNT * CPU_STM_VM_STRIDE) {
        int vm = (off - CPU_STM_VM_BASE) / CPU_STM_VM_STRIDE;
        uint32_t r = (off - CPU_STM_VM_BASE) % CPU_STM_VM_STRIDE;

        if (r == STM_ISCR) {
            uint32_t isr_addr = stm_vm_reg(core, vm, STM_ISR);
            uint32_t isr = reg_get(s, isr_addr);

            isr &= ~((val & BIT(0) ? BIT(0) : 0) | (val & BIT(2) ? BIT(1) : 0));
            isr |= (val & BIT(1) ? BIT(0) : 0) | (val & BIT(3) ? BIT(1) : 0);
            reg_set(s, isr_addr, isr);
            reg_set(s, addr, 0);
        } else if (r <= STM_ICR) {
            stm_schedule(s, core, vm, 0);
            stm_schedule(s, core, vm, 1);
        }
    }
}

static void sfr_count_read(Tc4xSoc *s, uint32_t addr)
{
    qatomic_inc(&sfr_page(s, addr, true)->reads[sfr_word(addr)]);
}

static uint64_t sfr_read(void *opaque, hwaddr offset, unsigned size,
                         uint32_t base)
{
    Tc4xSoc *s = opaque;
    uint32_t addr = base + offset;
    uint32_t shift = (addr & 3) * 8;
    uint32_t word = sfr_read_word(s, addr & ~3u);

    sfr_count_read(s, addr);
    return extract32(word, shift, size * 8);
}

static void sfr_write(void *opaque, hwaddr offset, uint64_t val,
                      unsigned size, uint32_t base)
{
    Tc4xSoc *s = opaque;
    uint32_t addr = base + offset;
    uint32_t aligned = addr & ~3u;
    uint32_t shift = (addr & 3) * 8;
    uint32_t word = val;

    if (size < 4) {
        word = deposit32(reg_get(s, aligned), shift, size * 8, val);
    }
    if (g_hash_table_contains(s->watches, GUINT_TO_POINTER(aligned))) {
        sim_printf(s, "ev w 0x%08x 0x%08x %u %d\n", aligned, word,
                   size, current_cpu ? current_cpu->cpu_index : -1);
    }
    sfr_write_word(s, aligned, word);
}

static uint64_t sfr_e_read(void *opaque, hwaddr off, unsigned size)
{
    return sfr_read(opaque, off, size, 0xE0000000u);
}

static void sfr_e_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    sfr_write(opaque, off, val, size, 0xE0000000u);
}

static uint64_t sfr_f_read(void *opaque, hwaddr off, unsigned size)
{
    return sfr_read(opaque, off, size, 0xF0000000u);
}

static void sfr_f_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    sfr_write(opaque, off, val, size, 0xF0000000u);
}

static const MemoryRegionOps sfr_e_ops = {
    .read = sfr_e_read,
    .write = sfr_e_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps sfr_f_ops = {
    .read = sfr_f_read,
    .write = sfr_f_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

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
        address_space_read(&address_space_memory, a[1],
                           MEMTXATTRS_UNSPECIFIED, &v, size);
        sim_printf(s, "ok 0x%08x\n", le32_to_cpu(v));
    } else if (!strcmp(argv[0], "w") && argc >= 3) {
        uint32_t size = argc >= 4 ? a[3] : 4;
        uint32_t v = cpu_to_le32(a[2]);

        if (size != 1 && size != 2 && size != 4) {
            sim_printf(s, "err size must be 1, 2 or 4\n");
            return;
        }
        address_space_write(&address_space_memory, a[1],
                            MEMTXATTRS_UNSPECIFIED, &v, size);
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "poke") && argc >= 3) {
        reg_set(s, a[1] & ~3u, a[2]);
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "force") && argc >= 3) {
        reg_force(s, a[1] & ~3u, a[2], argc >= 4 ? a[3] : 0xFFFFFFFFu);
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "unforce") && argc >= 2) {
        reg_force(s, a[1] & ~3u, 0, 0);
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "irq") && argc >= 2) {
        src_set_srr(s, a[1] & ~3u, true);
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "watch") && argc >= 2) {
        g_hash_table_add(s->watches, GUINT_TO_POINTER(a[1] & ~3u));
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "unwatch") && argc >= 2) {
        g_hash_table_remove(s->watches, GUINT_TO_POINTER(a[1] & ~3u));
        sim_printf(s, "ok\n");
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
        uint32_t min = argc >= 2 ? a[1] : 100;

        for (uint32_t i1 = 0; i1 < SFR_L1_SIZE; i1++) {
            Tc4xSfrPage **l2 = qatomic_load_acquire(&s->sfr_l1[i1]);

            for (uint32_t i2 = 0; l2 && i2 < SFR_L2_SIZE; i2++) {
                Tc4xSfrPage *pg = qatomic_load_acquire(&l2[i2]);
                uint32_t page = (i1 << (SFR_PAGE_BITS + SFR_L2_BITS)) |
                                (i2 << SFR_PAGE_BITS);

                for (uint32_t w = 0; pg && w < SFR_PAGE_WORDS; w++) {
                    uint32_t n = qatomic_xchg(&pg->reads[w], 0);

                    if (n >= min) {
                        sim_printf(s, "hot 0x%08x %u 0x%08x\n",
                                   page + w * 4, n,
                                   qatomic_read(&pg->val[w]));
                    }
                }
            }
        }
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "vmstop")) {
        vm_stop(RUN_STATE_PAUSED);
        sim_printf(s, "ok\n");
    } else if (!strcmp(argv[0], "vmcont")) {
        vm_start();
        sim_printf(s, "ok\n");
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

static void sim_exec(Tc4xSoc *s, char *line)
{
    g_auto(GStrv) argv = g_strsplit_set(g_strstrip(line), " \t", -1);
    int argc = g_strv_length(argv);

    if (argc == 0 || argv[0][0] == '\0' || argv[0][0] == '#') {
        return;
    }
    if (sim_needs_qspi_lock(s, argv[0])) {
        QEMU_LOCK_GUARD(&s->qspi_lock);
        sim_exec_argv(s, argc, argv);
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
            sim_exec(s, s->sim_line->str);
            g_string_truncate(s->sim_line, 0);
        } else if (buf[i] != '\r') {
            g_string_append_c(s->sim_line, buf[i]);
        }
    }
}

static void add_ram(Tc4xSoc *s, MemoryRegion *mr, const char *name,
                    hwaddr base, uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static void add_alias(MemoryRegion *alias, const char *name,
                      MemoryRegion *orig, hwaddr base)
{
    memory_region_init_alias(alias, NULL, name, orig, 0,
                             memory_region_size(orig));
    memory_region_add_subregion(get_system_memory(), base, alias);
}

static void tc4x_load_image(Tc4xSoc *s, const char *file)
{
    hwaddr entry = 0;
    uint64_t elf_entry = 0;
    ssize_t size;

    if (g_str_has_suffix(file, ".hex")) {
        size = load_targphys_hex_as(file, &entry, &address_space_memory);
    } else {
        size = load_elf(file, NULL, NULL, NULL, &elf_entry, NULL, NULL, NULL,
                        ELFDATA2LSB, EM_TRICORE, 1, 0);
        entry = elf_entry;
    }
    if (size <= 0) {
        error_report("tc4x: cannot load '%s'", file);
        exit(1);
    }
    if (s->boot_pc == 0) {
        s->boot_pc = entry;
    }
}

static void tc4x_reset(MachineState *ms, ResetType type)
{
    Tc4xSoc *s = TC4X_MACHINE(ms);

    qemu_devices_reset(type);
    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        cpu_reset(CPU(s->cpu[c]));
    }
    WITH_QEMU_LOCK_GUARD(&s->ir_lock) {
        /* the CPU reset dropped the published priorities */
        memset(s->cpu_prio, 0, sizeof(s->cpu_prio));
    }
    ir_update(s);
    s->cpu[0]->env.PC = s->boot_pc;
    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        reg_set(s, CPU_SFR_BASE + c * CPU_SFR_STRIDE + CPU_HRA_BOOTCON,
                c == 0 ? 0 : 1);
    }
}

static void tc4x_force_soc_status(Tc4xSoc *s)
{
    reg_force(s, CLOCK_RAMPSTAT, CLOCK_RAMPSTAT_ACTIVE,
              CLOCK_RAMPSTAT_ACTIVE);
    for (unsigned n = 0; n < VMT_COUNT; n++) {
        reg_force(s, VMT_BASE + n * VMT_STRIDE + VMT_MEMDONE, UINT32_MAX,
                  UINT32_MAX);
        reg_force(s, VMT_BASE + n * VMT_STRIDE + VMT_MEMDONECS, UINT32_MAX,
                  UINT32_MAX);
    }
}

static void tc4x_init(MachineState *ms)
{
    Tc4xSoc *s = TC4X_MACHINE(ms);
    MemoryRegion *sysmem = get_system_memory();
    s->watches = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->tom_cn0 = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, g_free);
    s->pending = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_rec_mutex_init(&s->ir_lock);
    qemu_rec_mutex_init(&s->qspi_lock);
    s->sim_line = g_string_new(NULL);
    s->sim_commands = g_array_new(false, false, sizeof(Tc4xSimCommand));
    for (int n = 0; n < QSPI_COUNT; n++) {
        s->qspi_rx[n] = g_queue_new();
        s->qspi_cs[n] = -1;
    }
    tc4x_force_soc_status(s);

    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        g_autofree char *name = g_strdup_printf("core%d.spr", c);
        hwaddr base = 0x70000000u - c * 0x10000000u;

        add_ram(s, &s->core_ram[c], name, base, TC4X_CORE_RAM_SIZE);
    }
    add_ram(s, &s->seg1_ram, "seg1", 0x10000000u, 16 * MiB);
    add_ram(s, &s->pflash, "pflash", 0x80000000u, 80 * MiB);
    add_alias(&s->pflash_alias, "pflash.nc", &s->pflash, 0xA0000000u);
    add_ram(s, &s->dflash, "dflash", 0xAE000000u, 32 * MiB);
    add_ram(s, &s->lmu, "lmu", 0x90000000u, 64 * MiB);
    add_alias(&s->lmu_alias, "lmu.nc", &s->lmu, 0xB0000000u);

    memory_region_init_io(&s->sfr_e, NULL, &sfr_e_ops, s, "sfr.e",
                          0x10000000u);
    memory_region_add_subregion(sysmem, 0xE0000000u, &s->sfr_e);
    memory_region_init_io(&s->sfr_f, NULL, &sfr_f_ops, s, "sfr.f",
                          0x10000000u);
    memory_region_add_subregion(sysmem, 0xF0000000u, &s->sfr_f);

    for (int c = 0; c < TC4X_NUM_CORES; c++) {
        Object *cpuobj = object_new(ms->cpu_type);
        g_autofree char *root = g_strdup_printf("core%d.view", c);

        memory_region_init(&s->cpu_root[c], NULL, root, UINT64_MAX);
        memory_region_init_alias(&s->cpu_sys_alias[c], NULL, "sys", sysmem,
                                 0, UINT64_MAX);
        memory_region_add_subregion_overlap(&s->cpu_root[c], 0,
                                            &s->cpu_sys_alias[c], 0);
        memory_region_init_alias(&s->cpu_local_dspr[c], NULL, "dspr.local",
                                 &s->core_ram[c], 0, TC4X_PSPR_OFFSET);
        memory_region_add_subregion_overlap(&s->cpu_root[c], TC4X_LOCAL_DSPR,
                                            &s->cpu_local_dspr[c], 1);
        memory_region_init_alias(&s->cpu_local_pspr[c], NULL, "pspr.local",
                                 &s->core_ram[c], TC4X_PSPR_OFFSET,
                                 TC4X_CORE_RAM_SIZE - TC4X_PSPR_OFFSET);
        memory_region_add_subregion_overlap(&s->cpu_root[c], TC4X_LOCAL_PSPR,
                                            &s->cpu_local_pspr[c], 1);

        object_property_set_uint(cpuobj, "core-id", c, &error_fatal);
        object_property_set_bool(cpuobj, "start-halted", c != 0, &error_fatal);
        object_property_set_link(cpuobj, "memory", OBJECT(&s->cpu_root[c]),
                                 &error_fatal);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        s->cpu[c] = TRICORE_CPU(cpuobj);
        s->cpu[c]->irq_ack = ir_ack;
        s->cpu[c]->irq_ack_opaque = s;

        for (int vm = 0; vm < CPU_STM_VM_COUNT; vm++) {
            for (int cmp = 0; cmp < 2; cmp++) {
                Tc4xStmTimer *t = &s->stm[c][vm][cmp];

                t->s = s;
                t->core = c;
                t->vm = vm;
                t->cmp = cmp;
                t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stm_fire, t);
            }
        }
    }

    qemu_chr_fe_init(&s->uart, serial_hd(0), &error_abort);
    qemu_chr_fe_set_handlers(&s->uart, uart_can_receive, uart_receive, NULL,
                             NULL, s, NULL, true);
    if (serial_hd(1)) {
        qemu_chr_fe_init(&s->sim, serial_hd(1), &error_abort);
        qemu_chr_fe_set_handlers(&s->sim, sim_can_receive, sim_receive, NULL,
                                 NULL, s, NULL, true);
    }

    if (s->sfr_script) {
        g_autofree char *text = NULL;
        g_auto(GStrv) lines = NULL;

        if (!g_file_get_contents(s->sfr_script, &text, NULL, NULL)) {
            error_report("tc4x: cannot read sfr-script '%s'", s->sfr_script);
            exit(1);
        }
        lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            sim_exec(s, lines[i]);
        }
    }

    if (ms->kernel_filename) {
        g_auto(GStrv) files = g_strsplit(ms->kernel_filename, ",", -1);

        for (int i = 0; files[i]; i++) {
            tc4x_load_image(s, files[i]);
        }
    }
}

static char *tc4x_get_sfr_script(Object *obj, Error **errp)
{
    return g_strdup(TC4X_MACHINE(obj)->sfr_script);
}

static void tc4x_set_sfr_script(Object *obj, const char *value, Error **errp)
{
    Tc4xSoc *s = TC4X_MACHINE(obj);

    g_free(s->sfr_script);
    s->sfr_script = g_strdup(value);
}

static void tc4x_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "sfr-script", tc4x_get_sfr_script,
                                  tc4x_set_sfr_script);
    object_class_property_set_description(oc, "sfr-script",
        "File of sim-bridge commands (force/poke/watch) run at machine init");

    mc->desc = "Infineon AURIX TC4Dx (6x TriCore 1.8)";
    mc->init = tc4x_init;
    mc->reset = tc4x_reset;
    mc->max_cpus = TC4X_NUM_CORES;
    mc->default_cpus = TC4X_NUM_CORES;
    mc->min_cpus = TC4X_NUM_CORES;
    mc->default_cpu_type = TRICORE_CPU_TYPE_NAME("tc4x");
}

static const TypeInfo tc4x_types[] = {
    {
        .name = TYPE_TC4X_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(Tc4xSoc),
        .class_init = tc4x_class_init,
    },
};

DEFINE_TYPES(tc4x_types)
