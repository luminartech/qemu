/*
 *  TriCore emulation for qemu: main CPU struct.
 *
 *  Copyright (c) 2012-2014 Bastian Koppelmann C-Lab/University Paderborn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRICORE_CPU_H
#define TRICORE_CPU_H

#include "cpu-qom.h"
#include "hw/core/registerfields.h"
#include "exec/cpu-common.h"
#include "exec/memattrs.h"
#include "qemu/cpu-float.h"
#include "tricore-defs.h"

#ifdef CONFIG_USER_ONLY
#error "TriCore does not support user mode emulation"
#endif

typedef struct CPUArchState {
    /* GPR Register */
    uint32_t gpr_a[16];
    uint32_t gpr_d[16];
/* Frequently accessed PSW_USB bits are stored separately for efficiency.
       This contains all the other bits.  Use psw_{read,write} to access
       the whole PSW.  */
    uint32_t PSW;
    /* PSW flag cache for faster execution */
    uint32_t PSW_USB_C;
    uint32_t PSW_USB_V;   /* Only if bit 31 set, then flag is set  */
    uint32_t PSW_USB_SV;  /* Only if bit 31 set, then flag is set  */
    uint32_t PSW_USB_AV;  /* Only if bit 31 set, then flag is set. */
    uint32_t PSW_USB_SAV; /* Only if bit 31 set, then flag is set. */

#define R(ADDR, NAME, FEATURE) uint32_t NAME;
#define A(ADDR, NAME, FEATURE) uint32_t NAME;
#define E(ADDR, NAME, FEATURE) uint32_t NAME;
#include "csfr.h.inc"
#undef R
#undef A
#undef E

    /* Floating Point Registers */
    float_status fp_status;

    /*
     * Backing store for CSFRs the translator has no dedicated field for
     * (e.g. the TC1.8 MPU range registers). Indexed by (offset & 0xfffc) >> 2.
     */
    uint32_t csfr_generic[0x10000 / 4];

    /* Priority of the highest pending service request routed here (0: none) */
    uint32_t pending_irq_prio;
    /* Core is parked in boot-halt until released through BOOTCON.BHALT */
    bool boot_halted;

    /* Fields up to here are cleared at reset */
    struct {} end_reset_fields;

    /* Internal CPU feature flags.  */
    uint64_t features;

    /*
     * With TriCoreCPU.csa_cache: host addresses of the last two RAM pages
     * that held a context save area (op_helper.c csa_host), by bit 0 of
     * the page number; a guest page address with low bits set means none.
     */
    uint32_t csa_page[2];
    uint8_t *csa_host_page[2];
} CPUTriCoreState;

/**
 * TriCoreCPU:
 * @env: #CPUTriCoreState
 *
 * A TriCore CPU.
 */
struct ArchCPU {
    CPUState parent_obj;

    CPUTriCoreState env;

    uint32_t core_id;
    /*
     * MemTxAttrs.requester_id of the core's accesses with PSW.S clear and
     * set (properties "requester-id", "requester-id-safe"; UINT32_MAX:
     * unspecified attributes), and the attributes made of them
     */
    uint32_t requester_id[2];
    MemTxAttrs mem_attrs[2];
    bool start_halted;
    /* remember context save area pages across TBs (op_helper.c csa_host) */
    bool csa_cache;
    /* called with the priority of every interrupt the CPU takes */
    void (*irq_ack)(void *opaque, uint32_t core_id, uint32_t prio);
    void *irq_ack_opaque;
};

#define CPU_INTERRUPT_NMI   CPU_INTERRUPT_TGT_EXT_3
/* A data access asynchronous error (DAE) waits to be taken */
#define CPU_INTERRUPT_DAE   CPU_INTERRUPT_TGT_EXT_2

void tricore_cpu_set_pending_irq(TriCoreCPU *cpu, uint32_t prio);
void tricore_cpu_release_boot_halt(TriCoreCPU *cpu, uint32_t pc);
void tricore_cpu_do_interrupt(CPUTriCoreState *env, uint32_t pipn);
void tricore_cpu_enter_trap(CPUTriCoreState *env, uint32_t class, int tin,
                            uint32_t ret);
/* Raise the non-maskable interrupt: trap class 7, TIN 0 (with the BQL) */
void tricore_cpu_raise_nmi(TriCoreCPU *cpu);
/* Take a synchronous trap at the instruction of @retaddr */
G_NORETURN void tricore_raise_trap(CPUTriCoreState *env, uint32_t class,
                                   int tin, uintptr_t retaddr);
/* A memory transaction answered with an error: a class 4 bus error trap */
void tricore_cpu_do_transaction_failed(CPUState *cs, hwaddr physaddr,
                                       vaddr addr, unsigned size,
                                       MMUAccessType access_type, int mmu_idx,
                                       MemTxAttrs attrs, MemTxResult response,
                                       uintptr_t retaddr);

struct TriCoreCPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

bool tricore_cpu_translate_for_debug(CPUState *cpu, vaddr addr,
                                     TranslateForDebugResult *result);
void tricore_cpu_dump_state(CPUState *cpu, FILE *f, int flags);

FIELD(PCXI, PCPN_13, 24, 8)
FIELD(PCXI, PCPN_161, 22, 8)
FIELD(PCXI, PIE_13, 23, 1)
FIELD(PCXI, PIE_161, 21, 1)
FIELD(PCXI, UL_13, 22, 1)
FIELD(PCXI, UL_161, 20, 1)
FIELD(PCXI, PCXS, 16, 4)
FIELD(PCXI, PCXO, 0, 16)

FIELD(ICR, IE_161, 15, 1)
FIELD(ICR, IE_13, 8, 1)
FIELD(ICR, PIPN, 16, 8)
FIELD(ICR, CCPN, 0, 8)

#define MASK_PSW_USB 0xff000000
#define MASK_USB_C   0x80000000
#define MASK_USB_V   0x40000000
#define MASK_USB_SV  0x20000000
#define MASK_USB_AV  0x10000000
#define MASK_USB_SAV 0x08000000
#define MASK_PSW_PRS 0x00003000
#define MASK_PSW_PRS2 0x00008000    /* TC1.8: PSW.PRS[2] */
#define MASK_PSW_S   0x00004000     /* TC1.6.2: safe task identifier */
#define MASK_PSW_IO  0x00000c00
#define MASK_PSW_IS  0x00000200
#define MASK_PSW_GW  0x00000100
#define MASK_PSW_CDE 0x00000080
#define MASK_PSW_CDC 0x0000007f
#define MASK_PSW_FPU_RM 0x3000000

#define MASK_SYSCON_PRO_TEN 0x2
#define MASK_SYSCON_FCD_SF  0x1
#define MASK_SYSCON_IS      0x8     /* PSW.S at interrupt entry */
#define MASK_SYSCON_TS      0x10    /* PSW.S at trap entry */

#define MASK_CPUID_MOD     0xffff0000
#define MASK_CPUID_MOD_32B 0x0000ff00
#define MASK_CPUID_REV     0x000000ff


#define MASK_FCX_FCXS 0x000f0000
#define MASK_FCX_FCXO 0x0000ffff

#define MASK_LCX_LCXS 0x000f0000
#define MASK_LCX_LCX0 0x0000ffff

#define MASK_DBGSR_DE 0x1
#define MASK_DBGSR_HALT 0x6
#define MASK_DBGSR_SUSP 0x10
#define MASK_DBGSR_PREVSUSP 0x20
#define MASK_DBGSR_PEVT 0x40
#define MASK_DBGSR_EVTSRC 0x1f00

enum tricore_priv_levels {
    TRICORE_PRIV_UM0 = 0x0, /* user mode-0 flag */
    TRICORE_PRIV_UM1 = 0x1, /* user mode-1 flag */
    TRICORE_PRIV_SM  = 0x2, /* kernel mode flag */
};

enum tricore_features {
    TRICORE_FEATURE_13,
    TRICORE_FEATURE_131,
    TRICORE_FEATURE_16,
    TRICORE_FEATURE_161,
    TRICORE_FEATURE_162,
    TRICORE_FEATURE_18,
};

static inline int tricore_has_feature(CPUTriCoreState *env, int feature)
{
    return (env->features & (1ULL << feature)) != 0;
}

/* TriCore Traps Classes*/
enum {
    TRAPC_NONE     = -1,
    TRAPC_MMU      = 0,
    TRAPC_PROT     = 1,
    TRAPC_INSN_ERR = 2,
    TRAPC_CTX_MNG  = 3,
    TRAPC_SYSBUS   = 4,
    TRAPC_ASSERT   = 5,
    TRAPC_SYSCALL  = 6,
    TRAPC_NMI      = 7,
    TRAPC_IRQ      = 8
};

/* Class 0 TIN */
enum {
    TIN0_VAF = 0,
    TIN0_VAP = 1,
};

/* Class 1 TIN */
enum {
    TIN1_PRIV = 1,
    TIN1_MPR  = 2,
    TIN1_MPW  = 3,
    TIN1_MPX  = 4,
    TIN1_MPP  = 5,
    TIN1_MPN  = 6,
    TIN1_GRWP = 7,
};

/* Class 2 TIN */
enum {
    TIN2_IOPC = 1,
    TIN2_UOPC = 2,
    TIN2_OPD  = 3,
    TIN2_ALN  = 4,
    TIN2_MEM  = 5,
};

/* Class 3 TIN */
enum {
    TIN3_FCD  = 1,
    TIN3_CDO  = 2,
    TIN3_CDU  = 3,
    TIN3_FCU  = 4,
    TIN3_CSU  = 5,
    TIN3_CTYP = 6,
    TIN3_NEST = 7,
};

/* Class 4 TIN */
enum {
    TIN4_PSE = 1,
    TIN4_DSE = 2,
    TIN4_DAE = 3,
    TIN4_CAE = 4,
    TIN4_PIE = 5,
    TIN4_DIE = 6,
};

/* Class 5 TIN */
enum {
    TIN5_OVF  = 1,
    TIN5_SOVF = 1,
};

/* Class 6 TIN
 *
 * Is always TIN6_SYS
 */

/* Class 7 TIN */
enum {
    TIN7_NMI = 0,
};

int tricore_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n);
int tricore_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n);

void fpu_set_state(CPUTriCoreState *env);

static inline uint32_t psw_read(CPUTriCoreState *env)
{
    /* clear all USB bits */
    env->PSW &= 0x7ffffff;
    /* now set them from the cache */
    env->PSW |= ((env->PSW_USB_C != 0) << 31);
    env->PSW |= ((env->PSW_USB_V   & (1 << 31))  >> 1);
    env->PSW |= ((env->PSW_USB_SV  & (1 << 31))  >> 2);
    env->PSW |= ((env->PSW_USB_AV  & (1 << 31))  >> 3);
    env->PSW |= ((env->PSW_USB_SAV & (1 << 31))  >> 4);

    return env->PSW;
}

static inline void psw_write(CPUTriCoreState *env, uint32_t val)
{
    /* the FPU state follows PSW.RM, which calls and returns keep */
    bool rm_changed = (env->PSW ^ val) & MASK_PSW_FPU_RM;

    env->PSW_USB_C = (val & MASK_USB_C);
    env->PSW_USB_V = (val & MASK_USB_V) << 1;
    env->PSW_USB_SV = (val & MASK_USB_SV) << 2;
    env->PSW_USB_AV = (val & MASK_USB_AV) << 3;
    env->PSW_USB_SAV = (val & MASK_USB_SAV) << 4;
    env->PSW = val;
    if (rm_changed) {
        fpu_set_state(env);
    }
}

#define FIELD_GETTER_WITH_FEATURE(NAME, REG, FIELD, FEATURE)                  \
static inline uint32_t NAME(CPUTriCoreState *env)                             \
{                                                                             \
    if (tricore_has_feature(env, TRICORE_FEATURE_##FEATURE)) {                \
        return FIELD_EX32(env->REG, REG, FIELD ## _ ## FEATURE);              \
    }                                                                         \
    return FIELD_EX32(env->REG, REG, FIELD ## _13);                           \
}

#define FIELD_GETTER(NAME, REG, FIELD)                                        \
static inline uint32_t NAME(CPUTriCoreState *env)                             \
{                                                                             \
    return FIELD_EX32(env->REG, REG, FIELD);                                  \
}

#define FIELD_SETTER_WITH_FEATURE(NAME, REG, FIELD, FEATURE)                  \
static inline void NAME(CPUTriCoreState *env, uint32_t val)                   \
{                                                                             \
    if (tricore_has_feature(env, TRICORE_FEATURE_##FEATURE)) {                \
        env->REG = FIELD_DP32(env->REG, REG, FIELD ## _ ## FEATURE, val);     \
    } else {                                                                  \
        env->REG = FIELD_DP32(env->REG, REG, FIELD ## _13, val);              \
    }                                                                         \
}

#define FIELD_SETTER(NAME, REG, FIELD)                                        \
static inline void NAME(CPUTriCoreState *env, uint32_t val)                   \
{                                                                             \
    env->REG = FIELD_DP32(env->REG, REG, FIELD, val);                         \
}

FIELD_GETTER_WITH_FEATURE(pcxi_get_pcpn, PCXI, PCPN, 161)
FIELD_SETTER_WITH_FEATURE(pcxi_set_pcpn, PCXI, PCPN, 161)
FIELD_GETTER_WITH_FEATURE(pcxi_get_pie, PCXI, PIE, 161)
FIELD_SETTER_WITH_FEATURE(pcxi_set_pie, PCXI, PIE, 161)
FIELD_GETTER_WITH_FEATURE(pcxi_get_ul, PCXI, UL, 161)
FIELD_SETTER_WITH_FEATURE(pcxi_set_ul, PCXI, UL, 161)
FIELD_GETTER(pcxi_get_pcxs, PCXI, PCXS)
FIELD_GETTER(pcxi_get_pcxo, PCXI, PCXO)

FIELD_GETTER_WITH_FEATURE(icr_get_ie, ICR, IE, 161)
FIELD_SETTER_WITH_FEATURE(icr_set_ie, ICR, IE, 161)
FIELD_GETTER(icr_get_ccpn, ICR, CCPN)
FIELD_SETTER(icr_set_ccpn, ICR, CCPN)

#define MMU_USER_IDX 2

/*
 * MMU indexes: the protection register set the range-based memory
 * protection checks an access against (PSW.PRS, TC1.8: 0..7), or the
 * context save areas of calls, returns, traps and interrupts, which it
 * does not check; each for PSW.S clear and set, since PSW.S is part of
 * the bus attributes of every access (MemTxAttrs.requester_id)
 */
#define TRICORE_PRS_COUNT   8
#define TRICORE_MMU_IDX_CSA TRICORE_PRS_COUNT
#define TRICORE_MMU_SETS    (TRICORE_PRS_COUNT + 1)

/* MMU index of protection set (or TRICORE_MMU_IDX_CSA) @set under @safe */
static inline int tricore_mmu_idx(unsigned set, bool safe)
{
    return set + (safe ? TRICORE_MMU_SETS : 0);
}

/* The protection set (or TRICORE_MMU_IDX_CSA) of MMU index @idx */
static inline unsigned tricore_mmu_idx_set(int idx)
{
    return idx % TRICORE_MMU_SETS;
}

/* PSW.S of MMU index @idx */
static inline bool tricore_mmu_idx_safe(int idx)
{
    return idx >= TRICORE_MMU_SETS;
}

FIELD(TB_FLAGS, PRIV, 0, 2)
FIELD(TB_FLAGS, PRS, 2, 3)
FIELD(TB_FLAGS, S, 5, 1)

/* PSW.PRS: bits 13:12, TC1.8 adds bit 2 of the set number at bit 15 */
static inline unsigned tricore_psw_prs(CPUTriCoreState *env)
{
    unsigned prs = extract32(env->PSW, 12, 2);

    if (tricore_has_feature(env, TRICORE_FEATURE_18)) {
        prs |= extract32(env->PSW, 15, 1) << 2;
    }
    return prs;
}

/* PSW.S (TC1.6.2) */
static inline bool tricore_psw_s(CPUTriCoreState *env)
{
    return tricore_has_feature(env, TRICORE_FEATURE_162) &&
           (env->PSW & MASK_PSW_S);
}

/* TC1.8 memory protection CSFRs: DPRn_L/U, CPRn_L/U, CPXE/DPRE/DPWE_x */
#define CSFR_DPR_L(n)       (0xC000 + (n) * 8)
#define CSFR_DPR_U(n)       (0xC004 + (n) * 8)
#define CSFR_CPR_L(n)       (0xD000 + (n) * 8)
#define CSFR_CPR_U(n)       (0xD004 + (n) * 8)
/* sets 0-3 at E0x0 + x*4, sets 4-7 at E0x0 + 0x40 + (x - 4)*4 */
#define CSFR_PRS_SET(x)     (((x) & 3) * 4 + ((x) >> 2) * 0x40)
#define CSFR_CPXE(x)        (0xE000 + CSFR_PRS_SET(x))
#define CSFR_DPRE(x)        (0xE010 + CSFR_PRS_SET(x))
#define CSFR_DPWE(x)        (0xE020 + CSFR_PRS_SET(x))
#define CSFR_DEADD          0x901C      /* data error address */
#define CSFR_MPU_FIRST      CSFR_DPR_L(0)
#define CSFR_MPU_END        (CSFR_DPWE(TRICORE_PRS_COUNT - 1) + 4)
#define TRICORE_MPU_RANGES  32

void cpu_state_reset(CPUTriCoreState *s);
void tricore_tcg_init(void);
void tricore_translate_code(CPUState *cs, TranslationBlock *tb,
                            int *max_insns, vaddr pc, void *host_pc);

#define CPU_RESOLVING_TYPE TYPE_TRICORE_CPU

/* helpers.c */
bool tricore_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                          MMUAccessType access_type, int mmu_idx,
                          bool probe, uintptr_t retaddr);

#endif /* TRICORE_CPU_H */
