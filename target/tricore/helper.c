/*
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

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/registerfields.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "fpu/softfloat-helpers.h"
#include "qemu/qemu-print.h"

enum {
    TLBRET_DIRTY = -4,
    TLBRET_INVALID = -3,
    TLBRET_NOMATCH = -2,
    TLBRET_BADADDR = -1,
    TLBRET_MATCH = 0
};

static int get_physical_address(CPUTriCoreState *env, hwaddr *physical,
                                int *prot, vaddr address,
                                MMUAccessType access_type, int mmu_idx)
{
    int ret = TLBRET_MATCH;

    *physical = address & 0xFFFFFFFF;
    *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    return ret;
}

hwaddr tricore_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    TriCoreCPU *cpu = TRICORE_CPU(cs);
    hwaddr phys_addr;
    int prot;
    int mmu_idx = cpu_mmu_index(cs, false);

    if (get_physical_address(&cpu->env, &phys_addr, &prot, addr,
                             MMU_DATA_LOAD, mmu_idx)) {
        return -1;
    }
    return phys_addr;
}

/* TODO: Add exception support */
static void raise_mmu_exception(CPUTriCoreState *env, vaddr address,
                                int rw, int tlb_error)
{
}

/*
 * Range-based memory protection (TC1.8 architecture manual volume 1,
 * memory protection system): with CORECON.PROTEN set, a load needs a data
 * range [DPRn_L, DPRn_U) that DPRE_x enables for the protection set x
 * PSW.PRS selects, a store one that DPWE_x enables, an instruction fetch a
 * code range [CPRn_L, CPRn_U) that CPXE_x enables; ranges add up.  A
 * refused access is an MPR, MPW or MPX trap (class 1), a data access with
 * its address in DEADD; an atomic read and write needs both permissions.
 * Permissions are kept per TLB page: a page that an enabled range touches
 * is open to that kind of access as a whole, so a hole in a page with an
 * open part (a stack guard beside the stack) is not refused.  Checking
 * such pages on every access slows a core that keeps its stack there by
 * orders of magnitude.  The CSAs of context saves and restores are not
 * checked (TRICORE_MMU_IDX_CSA).
 */
static bool mpu_enabled(CPUTriCoreState *env, int mmu_idx)
{
    return tricore_has_feature(env, TRICORE_FEATURE_18) &&
           (env->SYSCON & MASK_SYSCON_PRO_TEN) && mmu_idx < TRICORE_PRS_COUNT;
}

static uint32_t csfr(CPUTriCoreState *env, uint32_t offset)
{
    return env->csfr_generic[offset / 4];
}

/* Whether a range of @access enabled in set @prs touches [@lo, @hi] */
static bool mpu_touches(CPUTriCoreState *env, unsigned prs,
                        MMUAccessType access, uint32_t lo, uint32_t hi)
{
    bool code = access == MMU_INST_FETCH;
    uint32_t enable = csfr(env, code ? CSFR_CPXE(prs) :
                           access == MMU_DATA_LOAD ? CSFR_DPRE(prs) :
                           CSFR_DPWE(prs));

    for (unsigned n = 0; n < TRICORE_MPU_RANGES; n++) {
        if ((enable & BIT(n)) &&
            hi >= csfr(env, code ? CSFR_CPR_L(n) : CSFR_DPR_L(n)) &&
            lo < csfr(env, code ? CSFR_CPR_U(n) : CSFR_DPR_U(n))) {
            return true;
        }
    }
    return false;
}

/* The page's permissions */
static int mpu_page_prot(CPUTriCoreState *env, unsigned prs, vaddr page)
{
    static const struct {
        MMUAccessType access;
        int prot;
    } kinds[] = {
        { MMU_DATA_LOAD, PAGE_READ },
        { MMU_DATA_STORE, PAGE_WRITE },
        { MMU_INST_FETCH, PAGE_EXEC },
    };
    int prot = 0;

    for (size_t i = 0; i < ARRAY_SIZE(kinds); i++) {
        if (mpu_touches(env, prs, kinds[i].access, page,
                        page + TARGET_PAGE_SIZE - 1)) {
            prot |= kinds[i].prot;
        }
    }
    return prot;
}

void helper_mpu_changed(CPUTriCoreState *env)
{
    tlb_flush(env_cpu(env));
}

bool tricore_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                          MMUAccessType rw, int mmu_idx,
                          bool probe, uintptr_t retaddr)
{
    CPUTriCoreState *env = cpu_env(cs);
    hwaddr physical;
    int prot;
    int ret = 0;

    if (mpu_enabled(env, mmu_idx)) {
        vaddr page = address & TARGET_PAGE_MASK;

        prot = mpu_page_prot(env, mmu_idx, page);
        if (!(prot & BIT(rw))) {
            if (probe) {
                return false;
            }
            qemu_log_mask(CPU_LOG_INT, "core%u: memory protection refuses "
                          "%s of 0x%08" VADDR_PRIx " in set %d\n",
                          env->CORE_ID, rw == MMU_INST_FETCH ? "fetch" :
                          rw == MMU_DATA_LOAD ? "load" : "store", address,
                          mmu_idx);
            if (rw != MMU_INST_FETCH) {
                env->csfr_generic[CSFR_DEADD / 4] = address;
            }
            tricore_raise_trap(env, TRAPC_PROT,
                               rw == MMU_INST_FETCH ? TIN1_MPX :
                               rw == MMU_DATA_LOAD ? TIN1_MPR : TIN1_MPW,
                               retaddr);
        }
        tlb_set_page(cs, page, page, prot, mmu_idx, TARGET_PAGE_SIZE);
        return true;
    }

    rw &= 1;
    ret = get_physical_address(env, &physical, &prot,
                               address, rw, mmu_idx);

    qemu_log_mask(CPU_LOG_MMU, "%s address=0x%" VADDR_PRIx " ret %d physical "
                  HWADDR_FMT_plx " prot %d\n",
                  __func__, address, ret, physical, prot);

    if (ret == TLBRET_MATCH) {
        tlb_set_page(cs, address & TARGET_PAGE_MASK,
                     physical & TARGET_PAGE_MASK, prot | PAGE_EXEC,
                     mmu_idx, TARGET_PAGE_SIZE);
        return true;
    } else {
        assert(ret < 0);
        if (probe) {
            return false;
        }
        raise_mmu_exception(env, address, rw, ret);
        cpu_loop_exit_restore(cs, retaddr);
    }
}

void fpu_set_state(CPUTriCoreState *env)
{
    switch (extract32(env->PSW, 24, 2)) {
    case 0:
        set_float_rounding_mode(float_round_nearest_even, &env->fp_status);
        break;
    case 1:
        set_float_rounding_mode(float_round_up, &env->fp_status);
        break;
    case 2:
        set_float_rounding_mode(float_round_down, &env->fp_status);
        break;
    case 3:
        set_float_rounding_mode(float_round_to_zero, &env->fp_status);
        break;
    }

    set_flush_inputs_to_zero(1, &env->fp_status);
    set_flush_to_zero(1, &env->fp_status);
    set_float_detect_tininess(float_tininess_before_rounding, &env->fp_status);
    set_float_ftz_detection(float_ftz_before_rounding, &env->fp_status);
    set_default_nan_mode(1, &env->fp_status);
    /* Default NaN pattern: sign bit clear, frac msb set */
    set_float_default_nan_pattern(0b01000000, &env->fp_status);
}
