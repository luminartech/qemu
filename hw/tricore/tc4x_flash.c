/*
 * TC4x program flash (PFLASH) and data flash (DFLASH): see tc4x_flash.h.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/block/block.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "hw/tricore/tc4x_flash.h"
#include "tc4x_soc.h"
#include "trace.h"

/* Granule of the seeded contents written to the drives */
#define FLASH_SEED_CHUNK    (64 * KiB)

/* DMU flash commands: the command sequences of the iLLD IfxFlash.h */
#define DMU_HCI_STATUS      0xF8040004u
#define DMU_HCI_ERR         0xF8040010u
#define DMU_HCI_CLRERR      0xF8040014u
#define DMU_CSCI_STATUS     0xF8040084u
#define DMU_CSCI_ERR        0xF8040090u
#define DMU_CSCI_CLRERR     0xF8040094u
#define DMU_CMD_BASE        0xF8080000u     /* host command interface */
#define DMU_CSCI_CMD_BASE   0xF80C0000u     /* CSRM command interface */
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
#define DMU_DF_SECTOR       (2 * KiB)
#define DMU_PF_SECTOR       (16 * KiB)
#define DMU_DF_PAGE         8u
#define DMU_PF_PAGE         32u
#define DMU_DF_WORDLINE     512u
#define DMU_PF_WORDLINE     1024u

static const char *const flash_names[TC4X_FLASH_COUNT] = {
    [TC4X_PFLASH] = "pflash",
    [TC4X_DFLASH] = "dflash",
};

static const uint64_t flash_sizes[TC4X_FLASH_COUNT] = {
    [TC4X_PFLASH] = TC4X_PFLASH_SIZE,
    [TC4X_DFLASH] = TC4X_DFLASH_SIZE,
};

uint64_t tc4x_flash_size(Tc4xFlash flash)
{
    return flash_sizes[flash];
}

static uint8_t *flash_ptr(Tc4xFlashState *s, Tc4xFlash flash)
{
    return memory_region_get_ram_ptr(&s->mem[flash]);
}

static bool flash_in_range(Tc4xFlash flash, uint64_t off, uint64_t len)
{
    return off <= flash_sizes[flash] && len <= flash_sizes[flash] - off;
}

/* Write [@off, @off + @len) of the storage to the drive */
static void flash_write_back(Tc4xFlashState *s, Tc4xFlash flash,
                             uint64_t off, uint64_t len)
{
    int ret;

    if (!s->blk[flash] || !len) {
        return;
    }
    ret = blk_pwrite(s->blk[flash], off, len, flash_ptr(s, flash) + off, 0);
    if (ret < 0) {
        error_report("tc4x-flash: cannot write %s drive: %s",
                     flash_names[flash], strerror(-ret));
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(RUN_STATE_IO_ERROR);
    }
}

/* The storage changed: no translated code of the old bytes runs again */
static void flash_changed(Tc4xFlashState *s, Tc4xFlash flash, uint64_t off,
                          uint64_t len)
{
    if (memory_region_is_romd(&s->mem[flash])) {
        memory_region_flush_rom_device(&s->mem[flash], off, len);
    }
    flash_write_back(s, flash, off, len);
    trace_tc4x_flash_write(flash_names[flash], off, len);
}

void tc4x_flash_storage_read(Tc4xFlashState *s, Tc4xFlash flash,
                             uint64_t off, void *buf, uint64_t len)
{
    uint64_t n = 0;

    if (off < flash_sizes[flash]) {
        n = MIN(len, flash_sizes[flash] - off);
        memcpy(buf, flash_ptr(s, flash) + off, n);
    }
    memset((uint8_t *)buf + n, 0, len - n);
}

void tc4x_flash_storage_write(Tc4xFlashState *s, Tc4xFlash flash,
                              uint64_t off, const void *buf, uint64_t len)
{
    assert(flash_in_range(flash, off, len));
    memcpy(flash_ptr(s, flash) + off, buf, len);
    flash_changed(s, flash, off, len);
}

void tc4x_flash_storage_fill(Tc4xFlashState *s, Tc4xFlash flash,
                             uint64_t off, uint8_t value, uint64_t len)
{
    assert(flash_in_range(flash, off, len));
    memset(flash_ptr(s, flash) + off, value, len);
    flash_changed(s, flash, off, len);
}

static void dmu_status(Tc4xFlashState *s, Tc4xDmuIf *d, uint32_t set,
                       uint32_t clr)
{
    tc4x_sfr_store_set(s->sfr, d->status,
                       (tc4x_sfr_store_get(s->sfr, d->status) & ~clr) | set);
}

static Tc4xFlashPolicyClass *flash_policy(Tc4xFlashState *s)
{
    return s->policy ? TC4X_FLASH_POLICY_GET_CLASS(s->policy) : NULL;
}

void tc4x_flash_dmu_error(Tc4xFlashState *s, bool csrm, uint32_t err)
{
    Tc4xFlashPolicyClass *pc = flash_policy(s);

    tc4x_sfr_store_or(s->sfr, s->dmu[csrm].err, err);
    if (pc && pc->error) {
        pc->error(TC4X_FLASH_POLICY(s->policy), csrm, err);
    }
}

/* Without a policy's locate method: PFLASH and DFLASH linear */
static bool flash_locate_linear(uint32_t addr, uint64_t len, Tc4xFlash *flash,
                                uint64_t *off)
{
    static const struct {
        uint32_t base;
        Tc4xFlash flash;
    } windows[] = {
        { TC4X_PFLASH_BASE_C, TC4X_PFLASH },
        { TC4X_PFLASH_BASE_NC, TC4X_PFLASH },
        { TC4X_DFLASH_BASE, TC4X_DFLASH },
    };

    for (size_t i = 0; i < ARRAY_SIZE(windows); i++) {
        if (addr >= windows[i].base &&
            (uint64_t)addr + len <=
            windows[i].base + flash_sizes[windows[i].flash]) {
            *flash = windows[i].flash;
            *off = addr - windows[i].base;
            return true;
        }
    }
    return false;
}

/*
 * The flash storage of a command's range; false: the command fails (SQER).
 * A range the policy places beyond the storage has no flash.
 */
static bool dmu_locate(Tc4xFlashState *s, uint32_t addr, uint64_t len,
                       Tc4xFlash *flash, uint64_t *off)
{
    Tc4xFlashPolicyClass *pc = flash_policy(s);
    bool found = pc && pc->locate ?
                 pc->locate(TC4X_FLASH_POLICY(s->policy), addr, len, flash,
                            off) :
                 flash_locate_linear(addr, len, flash, off);

    return found && flash_in_range(*flash, *off, len);
}

static bool dmu_range_is(Tc4xFlashState *s, Tc4xFlash flash, uint64_t off,
                         uint64_t len, uint8_t value)
{
    uint8_t buf[256];

    while (len) {
        uint32_t n = MIN(len, sizeof(buf));

        tc4x_flash_storage_read(s, flash, off, buf, n);
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != value) {
                return false;
            }
        }
        off += n;
        len -= n;
    }
    return true;
}

static uint32_t dmu_sector_size(uint32_t addr)
{
    return (addr & 0x0F000000u) == (TC4X_DFLASH_BASE & 0x0F000000u) ?
           DMU_DF_SECTOR : DMU_PF_SECTOR;
}

/* True when the policy refused, or executed, the command itself */
static bool dmu_policy(Tc4xFlashState *s, Tc4xDmuIf *d, Tc4xDmuOp op,
                       uint32_t addr, uint32_t code, uint64_t len,
                       const uint8_t *data)
{
    Tc4xFlashPolicyClass *pc = flash_policy(s);
    Tc4xFlashPolicy *p = s->policy ? TC4X_FLASH_POLICY(s->policy) : NULL;
    Tc4xDmuCommand cmd = {
        .op = op, .addr = addr, .code = code, .len = len, .data = data,
        .csrm = d->csrm,
    };
    uint32_t err;

    if (!pc) {
        return false;
    }
    if (op == TC4X_DMU_PROGRAM || op == TC4X_DMU_ERASE ||
        op == TC4X_DMU_VERIFY) {
        cmd.located = dmu_locate(s, addr, len, &cmd.flash, &cmd.off);
    }
    err = pc->permit ? pc->permit(p, &cmd) : 0;
    if (err) {
        tc4x_flash_dmu_error(s, d->csrm, err);
        return true;
    }
    return pc->command && pc->command(p, &cmd);
}

static uint64_t dmu_verify_len(uint32_t addr, uint32_t opcode, uint32_t count)
{
    bool df = dmu_sector_size(addr) == DMU_DF_SECTOR;

    return opcode == 0x56 ? (df ? DMU_DF_PAGE : DMU_PF_PAGE) :
           opcode == 0x58 ? (df ? DMU_DF_WORDLINE : DMU_PF_WORDLINE) :
           (uint64_t)count * dmu_sector_size(addr);
}

static void dmu_execute(Tc4xFlashState *s, Tc4xDmuIf *d, uint32_t class,
                        uint32_t opcode)
{
    uint32_t addr = d->cmd_addr;
    bool program = class == 0xA0 && (opcode == 0xAA || opcode == 0xA6);
    bool erase = class == 0x80 && opcode == 0x50;
    bool verify = class == 0x80 &&
                  (opcode == 0x56 || opcode == 0x58 || opcode == 0x5F);
    Tc4xFlash flash;
    uint64_t off, len;

    if (program && dmu_policy(s, d, TC4X_DMU_PROGRAM, addr, 0, d->page_len,
                              d->page_buf)) {
        d->page_len = 0;
        dmu_status(s, d, DMU_STATUS_REQDONE,
                   DMU_STATUS_DFPAGE | DMU_STATUS_PFPAGE);
        return;
    }
    if ((erase && dmu_policy(s, d, TC4X_DMU_ERASE, addr, 0,
                             (uint64_t)d->cmd_count * dmu_sector_size(addr),
                             NULL)) ||
        (verify && dmu_policy(s, d, TC4X_DMU_VERIFY, addr, 0,
                              dmu_verify_len(addr, opcode, d->cmd_count),
                              NULL)) ||
        (!program && !erase && !verify &&
         dmu_policy(s, d, TC4X_DMU_OTHER, addr, class << 8 | opcode, 0,
                    NULL))) {
        dmu_status(s, d, DMU_STATUS_REQDONE, 0);
        return;
    }

    if (program) {
        /* Write page / write burst: program the loaded page buffer */
        if (!dmu_locate(s, addr, d->page_len, &flash, &off)) {
            tc4x_flash_dmu_error(s, d->csrm, TC4X_DMU_ERR_SQER);
        } else {
            if (!dmu_range_is(s, flash, off, d->page_len, 0)) {
                tc4x_flash_dmu_error(s, d->csrm, TC4X_DMU_ERR_PVER);
            }
            tc4x_flash_storage_write(s, flash, off, d->page_buf, d->page_len);
        }
        d->page_len = 0;
        dmu_status(s, d, DMU_STATUS_REQDONE,
                   DMU_STATUS_DFPAGE | DMU_STATUS_PFPAGE);
    } else if (erase) {
        /* Erase logical sector range: erased flash reads as zero */
        len = (uint64_t)d->cmd_count * dmu_sector_size(addr);
        if (dmu_locate(s, addr, len, &flash, &off)) {
            tc4x_flash_storage_fill(s, flash, off, 0, len);
        } else {
            tc4x_flash_dmu_error(s, d->csrm, TC4X_DMU_ERR_SQER);
        }
        dmu_status(s, d, DMU_STATUS_REQDONE, 0);
    } else if (verify) {
        /*
         * Verify erased page (0x56) / wordline (0x58) / logical sector
         * range (0x5F); the page form takes count 0
         */
        len = dmu_verify_len(addr, opcode, d->cmd_count);
        if (!dmu_locate(s, addr, len, &flash, &off)) {
            tc4x_flash_dmu_error(s, d->csrm, TC4X_DMU_ERR_SQER);
        } else if (!dmu_range_is(s, flash, off, len, 0)) {
            tc4x_flash_dmu_error(s, d->csrm, TC4X_DMU_ERR_EVER);
        }
        dmu_status(s, d, DMU_STATUS_REQDONE, 0);
    } else {
        qemu_log_mask(LOG_UNIMP, "tc4x-flash: DMU command 0x%02x/0x%02x at "
                      "0x%08x completes without effect\n", class, opcode,
                      addr);
        dmu_status(s, d, DMU_STATUS_REQDONE, 0);
    }
}

static void dmu_cmd_write(Tc4xFlashState *s, Tc4xDmuIf *d, uint32_t off,
                          uint32_t val)
{
    switch (off) {
    case DMU_CMD_5554:
        if (dmu_policy(s, d,
                       val == 0xF0 ? TC4X_DMU_RESET_TO_READ : TC4X_DMU_OTHER,
                       0, val, 0, NULL)) {
            break;
        }
        if (val == 0xF0) {              /* reset to read */
            d->page_len = 0;
            d->cmd_armed = false;
            dmu_status(s, d, 0, DMU_STATUS_DFPAGE | DMU_STATUS_PFPAGE);
        } else if (val == 0xFA) {       /* clear status */
            dmu_status(s, d, 0, DMU_STATUS_REQDONE);
            tc4x_sfr_store_set(s->sfr, d->err, 0);
        } else if (val == 0x5D || val == 0x50) {  /* enter page mode */
            d->page_len = 0;
            dmu_status(s, d,
                       val == 0x5D ? DMU_STATUS_DFPAGE : DMU_STATUS_PFPAGE,
                       DMU_STATUS_REQDONE);
        }
        break;
    case DMU_CMD_55F0:
    case DMU_CMD_55F4:                  /* load page, 64-bit as two words */
        if (d->page_len + 4 <= TC4X_DMU_PAGE_BUF_SIZE) {
            stl_le_p(&d->page_buf[d->page_len], val);
            d->page_len += 4;
        } else {
            tc4x_flash_dmu_error(s, d->csrm, TC4X_DMU_ERR_SQER);
        }
        break;
    case DMU_CMD_AA50:
        d->cmd_addr = val;
        d->cmd_armed = false;
        break;
    case DMU_CMD_AA58:
        d->cmd_count = val;
        break;
    case DMU_CMD_AAA8:
        if (!d->cmd_armed) {
            d->cmd_class = val;
            d->cmd_armed = true;
        } else {
            d->cmd_armed = false;
            dmu_status(s, d, 0, DMU_STATUS_REQDONE);
            dmu_execute(s, d, d->cmd_class, val);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "tc4x-flash: DMU command cycle +0x%04x <- "
                      "0x%08x\n", off, val);
        break;
    }
}

bool tc4x_flash_dmu_write(Tc4xFlashState *s, uint32_t addr, uint32_t val)
{
    static const uint32_t cmd_base[2] = { DMU_CMD_BASE, DMU_CSCI_CMD_BASE };
    static const uint32_t clrerr[2] = { DMU_HCI_CLRERR, DMU_CSCI_CLRERR };

    for (int i = 0; i < ARRAY_SIZE(s->dmu); i++) {
        Tc4xDmuIf *d = &s->dmu[i];

        if (addr >= cmd_base[i] && addr < cmd_base[i] + DMU_CMD_SIZE) {
            s->dmu_cur = d;
            dmu_cmd_write(s, d, addr - cmd_base[i], val);
            s->dmu_cur = NULL;
            return true;
        }
        if (addr == clrerr[i]) {
            tc4x_sfr_store_set(s->sfr, d->err,
                               tc4x_sfr_store_get(s->sfr, d->err) & ~val);
            return true;
        }
    }
    return false;
}

bool tc4x_flash_patch(Tc4xFlashState *s, hwaddr addr, const void *buf,
                      unsigned len)
{
    MemoryRegionSection sec = memory_region_find(get_system_memory(), addr,
                                                 len);
    bool found = false;

    for (int f = 0; f < TC4X_FLASH_COUNT; f++) {
        if (sec.mr == &s->mem[f] && int128_get64(sec.size) == len) {
            tc4x_flash_storage_write(s, f, sec.offset_within_region, buf,
                                     len);
            found = true;
        }
    }
    if (sec.mr) {
        memory_region_unref(sec.mr);
    }
    return found;
}

bool tc4x_flash_is_kept(Tc4xFlashState *s)
{
    return s->kept;
}

/*
 * Reads come here only while a reset has the ROM devices out of ROMD
 * mode; otherwise they read the storage directly.
 */
static MemTxResult flash_mem_read(void *opaque, hwaddr addr, uint64_t *val,
                                  unsigned size, MemTxAttrs attrs)
{
    MemoryRegion *mr = opaque;

    *val = ldn_le_p((uint8_t *)memory_region_get_ram_ptr(mr) + addr, size);
    return MEMTX_OK;
}

/*
 * A store into a flash window is an error: the DMU programs flash.
 * Debug accesses do not come here, they write the storage directly, and
 * neither do the loader's writes at a reset (tc4x_flash_reset_hold).
 */
static MemTxResult flash_mem_write(Tc4xFlashState *s, Tc4xFlash flash,
                                   hwaddr addr, uint64_t val, unsigned size,
                                   MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "tc4x-flash: store of %u bytes to %s "
                  "offset 0x%" HWADDR_PRIx " refused: flash is programmed "
                  "through the DMU\n", size, flash_names[flash], addr);
    return MEMTX_ERROR;
}

static MemTxResult pflash_mem_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size, MemTxAttrs attrs)
{
    Tc4xFlashState *s = container_of(opaque, Tc4xFlashState,
                                     mem[TC4X_PFLASH]);

    return flash_mem_write(s, TC4X_PFLASH, addr, val, size, attrs);
}

static MemTxResult dflash_mem_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size, MemTxAttrs attrs)
{
    Tc4xFlashState *s = container_of(opaque, Tc4xFlashState,
                                     mem[TC4X_DFLASH]);

    return flash_mem_write(s, TC4X_DFLASH, addr, val, size, attrs);
}

static const MemoryRegionOps flash_mem_ops[TC4X_FLASH_COUNT] = {
    [TC4X_PFLASH] = {
        .read_with_attrs = flash_mem_read,
        .write_with_attrs = pflash_mem_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid = { .min_access_size = 1, .max_access_size = 8 },
        .impl = { .min_access_size = 1, .max_access_size = 8 },
    },
    [TC4X_DFLASH] = {
        .read_with_attrs = flash_mem_read,
        .write_with_attrs = dflash_mem_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid = { .min_access_size = 1, .max_access_size = 8 },
        .impl = { .min_access_size = 1, .max_access_size = 8 },
    },
};

void tc4x_flash_set_maps(Tc4xFlashState *s, const Tc4xPflashMap *maps,
                         unsigned n)
{
    uint64_t size = flash_sizes[TC4X_PFLASH];

    assert(n <= TC4X_PFLASH_MAPS_MAX);
    memory_region_transaction_begin();
    for (int w = 0; w < 2; w++) {
        for (unsigned i = 0; i < s->n_pflash_maps; i++) {
            memory_region_del_subregion(&s->pflash_win[w],
                                        s->pflash_maps[w][i]);
            object_unparent(OBJECT(s->pflash_maps[w][i]));
        }
        for (unsigned i = 0; i < n; i++) {
            MemoryRegion *mr = g_new0(MemoryRegion, 1);

            assert(maps[i].win <= size && maps[i].size <= size - maps[i].win &&
                   maps[i].storage <= size &&
                   maps[i].size <= size - maps[i].storage);
            memory_region_init_alias(mr, OBJECT(s), "pflash.map",
                                     &s->mem[TC4X_PFLASH], maps[i].storage,
                                     maps[i].size);
            memory_region_add_subregion_overlap(&s->pflash_win[w], maps[i].win,
                                                mr, 1);
            s->pflash_maps[w][i] = mr;
        }
    }
    s->n_pflash_maps = n;
    memory_region_transaction_commit();
}

/*
 * The images' ROM contents are written again at every reset, by
 * rom_reset() (hw/core/loader.c) through address_space_write_rom(), which
 * writes only regions it can access directly.  rom_reset() is a legacy
 * reset handler, registered when machine creation is done and so after
 * the SoC registered itself (tc4x_init): it runs in the hold phase of the
 * system reset after this one and before the exit phase.  Out of ROMD
 * mode the loader's writes skip the storage, except at the first reset of
 * flashes that are not kept.
 */
static void tc4x_flash_reset_hold(Object *obj, ResetType type)
{
    Tc4xFlashState *s = TC4X_FLASH(obj);

    for (int i = 0; i < ARRAY_SIZE(s->dmu); i++) {
        s->dmu[i].page_len = 0;
        s->dmu[i].cmd_armed = false;
    }
    if (s->kept) {
        s->seeded = true;
    }
    if (s->seeded) {
        for (int f = 0; f < TC4X_FLASH_COUNT; f++) {
            memory_region_rom_device_set_romd(&s->mem[f], false);
        }
    }
}

/* What the images seeded goes to the drives once */
static void flash_seed_drives(Tc4xFlashState *s)
{
    for (int f = 0; f < TC4X_FLASH_COUNT; f++) {
        for (uint64_t off = 0; off < flash_sizes[f];
             off += FLASH_SEED_CHUNK) {
            if (!buffer_is_zero(flash_ptr(s, f) + off, FLASH_SEED_CHUNK)) {
                flash_write_back(s, f, off, FLASH_SEED_CHUNK);
            }
        }
    }
}

static void tc4x_flash_reset_exit(Object *obj, ResetType type)
{
    Tc4xFlashState *s = TC4X_FLASH(obj);
    Tc4xFlashPolicyClass *pc = flash_policy(s);

    if (!s->seeded) {
        flash_seed_drives(s);
        s->seeded = true;
        trace_tc4x_flash_seeded();
    } else {
        for (int f = 0; f < TC4X_FLASH_COUNT; f++) {
            memory_region_rom_device_set_romd(&s->mem[f], true);
        }
    }
    if (pc && pc->reset) {
        pc->reset(TC4X_FLASH_POLICY(s->policy));
    }
}

static void tc4x_flash_init(Object *obj)
{
    Tc4xFlashState *s = TC4X_FLASH(obj);
    static const char *const win_name[2] = { "pflash.win", "pflash.win.nc" };

    for (int w = 0; w < 2; w++) {
        memory_region_init(&s->pflash_win[w], obj, win_name[w],
                           TC4X_PFLASH_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pflash_win[w]);
    }
    memory_region_init(&s->dflash_win, obj, "dflash.win", TC4X_DFLASH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->dflash_win);
    /* set by board code after the SoC is realized */
    object_property_add_link(obj, "policy", TYPE_TC4X_FLASH_POLICY,
                             &s->policy, object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    s->dmu[0] = (Tc4xDmuIf) { .status = DMU_HCI_STATUS, .err = DMU_HCI_ERR };
    s->dmu[1] = (Tc4xDmuIf) {
        .status = DMU_CSCI_STATUS, .err = DMU_CSCI_ERR, .csrm = true,
    };
}

/* The storage of @flash, from its drive if it has one */
static bool flash_init_storage(Tc4xFlashState *s, Tc4xFlash flash,
                               Error **errp)
{
    BlockBackend *blk = s->blk[flash];

    if (!memory_region_init_rom_device(&s->mem[flash], OBJECT(s),
                                       &flash_mem_ops[flash], &s->mem[flash],
                                       flash_names[flash], flash_sizes[flash],
                                       errp)) {
        return false;
    }
    if (!blk) {
        return true;
    }
    if (!blk_supports_write_perm(blk)) {
        error_setg(errp, "tc4x-flash: the %s drive must be writable",
                   flash_names[flash]);
        return false;
    }
    if (blk_set_perm(blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0 ||
        !blk_check_size_and_read_all(blk, DEVICE(s), flash_ptr(s, flash),
                                     flash_sizes[flash], errp)) {
        return false;
    }
    s->kept |= !buffer_is_zero(flash_ptr(s, flash), flash_sizes[flash]);
    return true;
}

static void tc4x_flash_realize(DeviceState *dev, Error **errp)
{
    Tc4xFlashState *s = TC4X_FLASH(dev);

    if (!s->sfr) {
        error_setg(errp, "tc4x-flash: link sfr-store must be set");
        return;
    }
    for (int f = 0; f < TC4X_FLASH_COUNT; f++) {
        if (!flash_init_storage(s, f, errp)) {
            return;
        }
    }
    for (int w = 0; w < 2; w++) {
        memory_region_init_alias(&s->pflash_linear[w], OBJECT(s),
                                 "pflash.linear", &s->mem[TC4X_PFLASH], 0,
                                 TC4X_PFLASH_SIZE);
        memory_region_add_subregion(&s->pflash_win[w], 0,
                                    &s->pflash_linear[w]);
    }
    memory_region_add_subregion(&s->dflash_win, 0, &s->mem[TC4X_DFLASH]);
}

static const Property tc4x_flash_properties[] = {
    DEFINE_PROP_DRIVE("pflash", Tc4xFlashState, blk[TC4X_PFLASH]),
    DEFINE_PROP_DRIVE("dflash", Tc4xFlashState, blk[TC4X_DFLASH]),
    DEFINE_PROP_LINK("sfr-store", Tc4xFlashState, sfr, TYPE_TC4X_SFR_STORE,
                     Tc4xSfrStore *),
};

static const VMStateDescription vmstate_tc4x_flash = {
    .name = TYPE_TC4X_FLASH,
    .unmigratable = 1,
};

static void tc4x_flash_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "TC4x program and data flash";
    dc->realize = tc4x_flash_realize;
    dc->vmsd = &vmstate_tc4x_flash;
    /* part of the tc4x SoC */
    dc->user_creatable = false;
    rc->phases.hold = tc4x_flash_reset_hold;
    rc->phases.exit = tc4x_flash_reset_exit;
    device_class_set_props(dc, tc4x_flash_properties);
}

/*
 * The board API of tc4x.h: a locate function and a DMU hook set one at a
 * time, served as one policy object
 */
#define TYPE_TC4X_FLASH_LEGACY_POLICY "tc4x-flash-legacy-policy"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xFlashLegacyPolicy, TC4X_FLASH_LEGACY_POLICY)

struct Tc4xFlashLegacyPolicy {
    Object parent_obj;

    Tc4xFlashLocateFn locate;
    void *locate_opaque;
    Tc4xDmuHookFn hook;
    void *hook_opaque;
};

static bool legacy_policy_locate(Tc4xFlashPolicy *p, uint32_t addr,
                                 uint64_t len, Tc4xFlash *flash, uint64_t *off)
{
    Tc4xFlashLegacyPolicy *l = TC4X_FLASH_LEGACY_POLICY(p);

    return l->locate ? l->locate(l->locate_opaque, addr, len, flash, off) :
           flash_locate_linear(addr, len, flash, off);
}

static bool legacy_policy_command(Tc4xFlashPolicy *p,
                                  const Tc4xDmuCommand *cmd)
{
    Tc4xFlashLegacyPolicy *l = TC4X_FLASH_LEGACY_POLICY(p);

    return l->hook && l->hook(l->hook_opaque, cmd);
}

static void tc4x_flash_legacy_policy_class_init(ObjectClass *oc,
                                                const void *data)
{
    Tc4xFlashPolicyClass *pc = TC4X_FLASH_POLICY_CLASS(oc);

    pc->locate = legacy_policy_locate;
    pc->command = legacy_policy_command;
}

static Tc4xFlashLegacyPolicy *flash_legacy_policy(Tc4xFlashState *s)
{
    Object *obj = object_resolve_path_component(OBJECT(s), "legacy-policy");

    if (!obj) {
        obj = object_new(TYPE_TC4X_FLASH_LEGACY_POLICY);
        object_property_add_child(OBJECT(s), "legacy-policy", obj);
        object_unref(obj);
        object_property_set_link(OBJECT(s), "policy", obj, &error_abort);
    }
    return TC4X_FLASH_LEGACY_POLICY(obj);
}

void tc4x_flash_set_locate_fn(Tc4xFlashState *s, Tc4xFlashLocateFn fn,
                              void *opaque)
{
    Tc4xFlashLegacyPolicy *l = flash_legacy_policy(s);

    l->locate = fn;
    l->locate_opaque = opaque;
}

void tc4x_flash_set_hook_fn(Tc4xFlashState *s, Tc4xDmuHookFn fn,
                            void *opaque)
{
    Tc4xFlashLegacyPolicy *l = flash_legacy_policy(s);

    l->hook = fn;
    l->hook_opaque = opaque;
}

void tc4x_flash_dmu_error_running(Tc4xFlashState *s, uint32_t err)
{
    tc4x_flash_dmu_error(s, s->dmu_cur && s->dmu_cur->csrm, err);
}

static const TypeInfo tc4x_flash_types[] = {
    {
        .name = TYPE_TC4X_FLASH_POLICY,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(Tc4xFlashPolicyClass),
    }, {
        .name = TYPE_TC4X_FLASH_LEGACY_POLICY,
        .parent = TYPE_OBJECT,
        .instance_size = sizeof(Tc4xFlashLegacyPolicy),
        .class_init = tc4x_flash_legacy_policy_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_FLASH_POLICY },
            { }
        },
    }, {
        .name = TYPE_TC4X_FLASH,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xFlashState),
        .instance_init = tc4x_flash_init,
        .class_init = tc4x_flash_class_init,
    },
};

DEFINE_TYPES(tc4x_flash_types)
