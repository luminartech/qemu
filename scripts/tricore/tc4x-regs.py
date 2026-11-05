#!/usr/bin/env python3
#
# Generate include/hw/tricore/tc4x_regs.h, the addresses and bit fields of
# the TC4x SoC registers the tc4x models and boards use, from the TC4x SFR
# headers of Infineon's iLLD (Infra/Sfr/TC4DA/_Reg in any AURIX TC4x code
# example of https://github.com/Infineon/AURIX_code_examples):
#
#   scripts/tricore/tc4x-regs.py SFR_REG_DIR \
#       > include/hw/tricore/tc4x_regs.h
#
# Each register in REGISTERS (iLLD names, shell patterns) gets a REG32()
# with its absolute address; each iLLD register type of those gets FIELD()
# lines for its named bit fields, under the type's name without "Ifx_", so
# that the instances of one module share them (QSPI_STATUS for QSPI0..7).
# Module instances other than the first are found from a stride.
#
# Copyright (c) 2026 MicroVision, Inc.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import fnmatch
import pathlib
import re
import sys

REGISTERS = [
    # SCU clocks: the start-up status the SoC reports done
    "CLOCK_RAMPCON0", "CLOCK_RAMPSTAT",
    "CLOCK_SYSPLLCON0", "CLOCK_SYSPLLSTAT",
    "CLOCK_PERPLLCON0", "CLOCK_PERPLLSTAT",
    "CLOCK_SYSCCUCON0", "CLOCK_SYSCCUCON1",
    "CLOCK_PERCCUCON0", "CLOCK_PERCCUCON1",
    "VMT0_MEMDONE", "VMT0_MEMDONECS", "VMT1_MEMDONE",
    "PMS_DTS_CON", "PMS_DTS_STAT",
    "SCU_SOTACTRL",
    # CPU0 (CPUn at a fixed stride)
    "CPU0_HRA_PC", "CPU0_HRA_CORE_ID", "CPU0_HRA_BOOTCON",
    "CPU1_HRA_PC",
    # CPU0 system timer: VM0 of the hypervisor view, the VM view (STM_*)
    "CPU0_STM_CLC", "CPU0_STM_ABS", "CPU0_STMHV_VM0_CMP0",
    "CPU0_STMHV_VM0_CMP1",
    "CPU0_STMHV_VM0_CMCON", "CPU0_STMHV_VM0_STM_ICR", "CPU0_STMHV_VM0_ISCR",
    "CPU0_STMHV_VM0_ISR", "CPU0_STMHV_VM1_CMP0", "CPU0_STM_CMP0",
    # I2C0 kernel registers (I2Cn at a fixed stride)
    "I2C0_CLC1", "I2C0_BUSSTAT", "I2C0_FIFOCFG", "I2C0_MRPSCTRL",
    "I2C0_RPSSTAT", "I2C0_TPSCTRL", "I2C0_FFSSTAT", "I2C0_ERRIRQSM",
    "I2C0_ERRIRQSS", "I2C0_ERRIRQSC", "I2C0_PIRQSM", "I2C0_PIRQSS",
    "I2C0_PIRQSC", "I2C0_RIS", "I2C0_MIS", "I2C0_ICR", "I2C0_TXD",
    "I2C0_RXD", "I2C1_CLC1", "I2C2_CLC1",
    # interrupt router: GPSR software triggers and service request nodes
    "INT_GPSRG0_SWC0", "INT_GPSRG0_SWC1", "INT_GPSRG1_SWC0",
    "INT_GPSRG0_SWCACCEN0",
    "SRC_STMCPU0_SR0", "SRC_STMCPU1_SR0", "SRC_GPSR0_SR0", "SRC_GPSR1_SR0",
    "SRC_ASCLIN0_TX", "SRC_ASCLIN0_RX", "SRC_ASCLIN1_TX",
    "SRC_QSPI0_TX", "SRC_QSPI0_RX", "SRC_QSPI0_PT", "SRC_QSPI0_U",
    "SRC_QSPI1_TX",
    "SRC_DMA0_CH0", "SRC_DMA1_CH0",
    "SRC_GETH0_DMA0", "SRC_GETH0_INTR",
    "SRC_CSS0_SR0", "SRC_PKC", "SRC_RNG", "SRC_PPU_ICI",
    "SRC_EGTM_TIM0_SR0", "SRC_EGTM_TOM0_SR0", "SRC_EGTM_TIM1_SR0",
    # ASCLIN0 (ASCLINn at a fixed stride)
    "ASCLIN0_TXFIFOCON", "ASCLIN0_RXFIFOCON", "ASCLIN0_FLAGS",
    "ASCLIN0_FLAGSSET", "ASCLIN0_FLAGSCLEAR", "ASCLIN0_CSR",
    "ASCLIN0_TXDATA0", "ASCLIN0_RXDATA0", "ASCLIN1_FLAGS",
    # QSPI0 (QSPIn at a fixed stride)
    "QSPI0_GLOBALCON1", "QSPI0_STATUS", "QSPI0_BACONENTRY",
    "QSPI0_DATAENTRY0", "QSPI0_RXEXIT0", "QSPI0_MCCON", "QSPI1_STATUS",
    # DMA0 channel 0 (channels and DMA1 at fixed strides)
    "DMA0_TSR0", "DMA0_TSR1", "DMA0_CH0_*", "DMA0_CH1_RDCRCR",
    "DMA1_TSR0",
    # DMU command interfaces and flash protection
    "DMU_HCI_STATUS", "DMU_HCI_BCONTROL", "DMU_HCI_ERR", "DMU_HCI_CLRERR",
    "DMU_HCI_SLEEP",
    "DMU_CSCI_STATUS", "DMU_CSCI_BCONTROL", "DMU_CSCI_ERR",
    "DMU_CSCI_CLRERR", "DMU_CSCI_SLEEP",
    "DMU_PROT_ACCENHCI_WRA", "DMU_PROT_ACCENCSCI_WRA",
    "DMU_PROT_ACCENCSPF0_WRA", "DMU_PROT_ACCENCSDF0_WRA",
    "DMU_PROT_ACCENCSDFUCB_WRA", "DMU_PROT_ACCENHDFUCB_WRA",
    # TMADC
    "ADC_CLKEN_TMADC", "ADC_TMADC0_MODCFG", "ADC_TMADC0_MODSTAT",
    "ADC_TMADC1_MODCFG",
    # eGTM: TBU, TOM and TIM channel 0 of cluster 0
    "EGTM_CLS0_TBU_CH0_BASE",
    "EGTM_CLS0_TOM_CH0_SR0", "EGTM_CLS0_TOM_CH0_SR1",
    "EGTM_CLS0_TOM_CH0_CM0", "EGTM_CLS0_TOM_CH0_CM1",
    "EGTM_CLS0_TOM_CH0_CN0", "EGTM_CLS0_TOM_CH1_CN0",
    "EGTM_CLS0_TOM_TGC0_GLB_CTRL", "EGTM_CLS0_TOM_TGC1_GLB_CTRL",
    "EGTM_CLS1_TOM_CH0_CN0",
    "EGTM_CLS0_TIM_CH0_CTRL", "EGTM_CLS0_TIM_CH1_CTRL",
    # system control: resets and the safety watchdog
    "SMM_SWRSTCON", "SMM_WRMRSTTRIG", "SMM_WRMRSTTRIGH", "SMM_RSTSTAT",
    "SMM_RSTSTATH", "SMM_RSTSTATCLR", "SMM_RSTTRIGCTRLA",
    "SMM_RSTTRIGCTRLC", "SMM_SMURSTSTATB",
    "WTU_WDTSEC_ACCEN_RDA", "WTU_WDTSEC_CTRLA", "WTU_WDTSEC_CTRLB",
    "WTU_WDTSEC_STAT",
    "SMU_CS_STS", "SMU_CS_RTC", "SMU_CS_AGSH0_STS",
    "FCE_CLC",
]

REGISTER = re.compile(r"#define (\w+) /\*.*?\*/ "
                      r"\(\*\(volatile (Ifx_\w+)\*\)0x([0-9A-Fa-f]+)u\)")
STRUCT = re.compile(r"typedef struct _(Ifx_\w+)_Bits\s*\{(.*?)\}", re.DOTALL)
BITFIELD = re.compile(r"Ifx_\w+ +(\w+):(\d+);\s*/\*\*< \\brief \[(\d+):(\d+)\]")
NOTICE = re.compile(r"\\copyright (Copyright .*?)\n.*?"
                    r"(Boost Software License.*?DEALINGS IN THE SOFTWARE\.)",
                    re.DOTALL)


def registers(reg_dir):
    """[(name, type, addr, module header)] in header order"""
    out = []
    for header in sorted(reg_dir.glob("*_reg.h")):
        text = header.read_text(errors="ignore")
        for name, reg_type, addr in REGISTER.findall(text):
            out.append((name, reg_type, int(addr, 16), header.stem))
    return out


def bit_fields(reg_dir):
    """{register type: [(field, lo, width)]} for the named bit fields"""
    fields = {}
    for header in sorted(reg_dir.glob("*_regdef.h")):
        for name, body in STRUCT.findall(header.read_text(errors="ignore")):
            fields[name] = [(f, int(lo), int(hi) - int(lo) + 1)
                            for f, _width, hi, lo in BITFIELD.findall(body)]
    return fields


def main():
    reg_dir = pathlib.Path(sys.argv[1])
    regs = registers(reg_dir)
    fields = bit_fields(reg_dir)
    chosen = []
    for pattern in REGISTERS:
        hits = [r for r in regs if fnmatch.fnmatchcase(r[0], pattern)]
        if not hits:
            sys.exit(f"tc4x-regs.py: no register matches {pattern}")
        chosen += [r for r in hits if r not in chosen]

    asclin = (reg_dir / "IfxAsclin_reg.h").read_text(errors="ignore")
    notice = NOTICE.search(asclin)
    out = ["/*",
           " * SPDX" "-License-Identifier: GPL-2.0-or-later AND BSL-1.0",
           " *",
           " * TC4x SoC registers: absolute addresses (A_<name>) and the bit",
           " * fields of their register types (R_<type>_<field>_SHIFT,",
           " * _LENGTH and _MASK, for FIELD_EX32() and FIELD_DP32()),",
           " * generated by scripts/tricore/tc4x-regs.py from the SFR",
           " * headers of the Infineon iLLD, which carry this notice:",
           " *"]
    for line in (notice.group(1) + "\n\n" + notice.group(2)).splitlines():
        line = line.lstrip(" *").rstrip()
        out.append(f" * {line}".rstrip())
    out += [" */",
            "",
            "#ifndef HW_TRICORE_TC4X_REGS_H",
            "#define HW_TRICORE_TC4X_REGS_H",
            "",
            '#include "hw/core/registerfields.h"']

    done_types = set()
    for module in sorted({r[3] for r in chosen}):
        name = module.removeprefix("Ifx").removesuffix("_reg")
        out += ["", f"/* {name} */"]
        types = []
        for name, reg_type, addr, mod in chosen:
            if mod == module:
                out.append(f"REG32({name}, {addr:#010x})")
                if reg_type not in types:
                    types.append(reg_type)
        for reg_type in types:
            if reg_type in done_types or not fields.get(reg_type):
                continue
            done_types.add(reg_type)
            short = reg_type.removeprefix("Ifx_")
            out += [f"    FIELD({short}, {f}, {lo}, {width})"
                    for f, lo, width in fields[reg_type]]
    out += ["", "#endif"]
    print("\n".join(out))


if __name__ == "__main__":
    main()
