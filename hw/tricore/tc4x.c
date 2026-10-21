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
#include "system/reset.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "elf.h"
#include "cpu.h"

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

#define TYPE_TC4X_MACHINE MACHINE_TYPE_NAME("tc4x")
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xSoc, TC4X_MACHINE)

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

typedef struct Tc4xSfrPage {
    uint32_t val[SFR_PAGE_WORDS];
    /* written, or its first read logged as unimplemented */
    unsigned long known[BITS_TO_LONGS(SFR_PAGE_WORDS)];
} Tc4xSfrPage;

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
    /*
     * Interrupt router.  Cores acknowledge interrupts without the BQL, so
     * pending, cpu_prio and the SRC registers' SRR bits are under ir_lock.
     */
    QemuRecMutex ir_lock;
    GHashTable *pending;   /* SRC addrs with SRR set */
    uint32_t cpu_prio[TC4X_NUM_CORES];

    Tc4xStmTimer stm[TC4X_NUM_CORES][CPU_STM_VM_COUNT][2];

    uint32_t boot_pc;
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

static uint32_t sfr_read_word(Tc4xSoc *s, uint32_t addr)
{
    uint32_t v = reg_get(s, addr);
    uint32_t off;
    int core;

    if (sfr_is_cpu(addr, &core, &off)) {
        if (off == CPU_STM_ABS) {
            v = (uint32_t)stm_now(s);
        } else if (off == CPU_STM_ABS + 4) {
            v = stm_now(s) >> 32;
        } else if (off == CPU_HRA_CORE_ID || off == CPU_HRHV_CORE_ID) {
            v = core;
        } else if (off == CPU_HRA_BOOTCON) {
            v = (v & ~1u) | (s->cpu[core]->env.boot_halted ? 1 : 0);
        }
    } else {
        Tc4xSfrPage *pg = sfr_page(s, addr, true);
        unsigned long bit = BIT_MASK(sfr_word(addr));

        if (!(qatomic_fetch_or(&pg->known[BIT_WORD(sfr_word(addr))], bit) &
              bit)) {
            qemu_log_mask(LOG_UNIMP, "tc4x-sfr: first read of 0x%08x\n", addr);
        }
    }

    return v;
}

static void sfr_write_word(Tc4xSoc *s, uint32_t addr, uint32_t val)
{
    uint32_t off;
    int core;

    if (addr >= SRC_BASE && addr < SRC_BASE + SRC_SIZE) {
        src_write(s, addr, val);
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

static uint64_t sfr_read(void *opaque, hwaddr offset, unsigned size,
                         uint32_t base)
{
    Tc4xSoc *s = opaque;
    uint32_t addr = base + offset;
    uint32_t shift = (addr & 3) * 8;
    uint32_t word = sfr_read_word(s, addr & ~3u);

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

static void tc4x_init(MachineState *ms)
{
    Tc4xSoc *s = TC4X_MACHINE(ms);
    MemoryRegion *sysmem = get_system_memory();
    s->pending = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_rec_mutex_init(&s->ir_lock);

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

    if (ms->kernel_filename) {
        g_auto(GStrv) files = g_strsplit(ms->kernel_filename, ",", -1);

        for (int i = 0; files[i]; i++) {
            tc4x_load_image(s, files[i]);
        }
    }
}

static void tc4x_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

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
