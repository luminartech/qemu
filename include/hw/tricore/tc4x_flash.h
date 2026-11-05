/*
 * TC4x program flash (PFLASH) and data flash (DFLASH).
 *
 * Each flash is a ROM device: the guest reads and fetches from it at RAM
 * speed, a guest store to it is refused (guest error, bus error response).
 * Its contents change through the device's storage functions only, which
 * invalidate translated code of the bytes they change and write them
 * through to the flash's drive.
 *
 * sysbus MMIO regions: 0 and 1 the PFLASH read windows (cached and
 * non-cached), 2 the DFLASH window.  The windows show the storage
 * linearly unless tc4x_flash_set_maps() maps parts of it elsewhere;
 * devices may overlay them with regions of higher priority.
 *
 * Properties "pflash" and "dflash": drives (raw, TC4X_PFLASH_SIZE and
 * TC4X_DFLASH_SIZE bytes) that hold the flash contents across runs.  A
 * flash without a drive starts erased and lives in memory.  When a drive
 * holds anything but erased cells, the flashes are kept: images loaded
 * with -kernel do not write them.  Otherwise the images seed them once,
 * at the first machine reset; later resets leave the flash contents as
 * the guest programmed them.
 *
 * The DMU command interfaces of the host (HCI) and of the CSRM (CSCI)
 * program, erase and verify the flashes.  Their registers stay SFR store
 * words, so the SoC passes writes of the command windows and of CLRERR
 * to tc4x_flash_dmu_write().  Commands at addresses that are not aligned
 * to their page, wordline or sector fail with a sequence error.
 * Link "policy": the board's rules (where a command finds its flash,
 * which commands it runs or refuses itself, what a failed command does),
 * an object with the TYPE_TC4X_FLASH_POLICY interface.
 *
 * Locking: guest reads run without the BQL; everything else runs under
 * it.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TRICORE_TC4X_FLASH_H
#define HW_TRICORE_TC4X_FLASH_H

#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/tricore/tc4x_sfr_store.h"

#define TYPE_TC4X_FLASH "tc4x-flash"
OBJECT_DECLARE_SIMPLE_TYPE(Tc4xFlashState, TC4X_FLASH)

#define TC4X_PFLASH_SIZE        (80 * MiB)
#define TC4X_DFLASH_SIZE        (32 * MiB)
#define TC4X_PFLASH_BASE_C      0x80000000u     /* cached read window */
#define TC4X_PFLASH_BASE_NC     0xA0000000u     /* non-cached read window */
#define TC4X_DFLASH_BASE        0xAE000000u

/* The program and data flash, as storage */
typedef enum Tc4xFlash {
    TC4X_PFLASH,
    TC4X_DFLASH,
    TC4X_FLASH_COUNT,
} Tc4xFlash;

/*
 * A PFLASH window map: @size bytes of storage from @storage at window
 * offset @win
 */
#define TC4X_PFLASH_MAPS_MAX 16
typedef struct Tc4xPflashMap {
    uint64_t win;
    uint64_t size;
    uint64_t storage;
} Tc4xPflashMap;

/* A DMU command as the flash policy sees it, before it runs */
typedef enum Tc4xDmuOp {
    TC4X_DMU_PROGRAM,       /* write page or burst: @len bytes of @data */
    TC4X_DMU_ERASE,         /* erase logical sectors: @len bytes */
    TC4X_DMU_VERIFY,        /* verify erased: @len bytes */
    TC4X_DMU_RESET_TO_READ,
    TC4X_DMU_OTHER,         /* @code: the 5554 value, or class << 8 | opcode */
} Tc4xDmuOp;

typedef struct Tc4xDmuCommand {
    Tc4xDmuOp op;
    uint32_t addr;          /* command address */
    uint32_t code;
    bool csrm;              /* from the CSRM command interface, else host */
    bool located;           /* @flash and @off hold @addr (program..verify) */
    Tc4xFlash flash;
    uint64_t off;
    uint64_t len;
    const uint8_t *data;
} Tc4xDmuCommand;

/* DMU_HCI_ERR / DMU_CSCI_ERR flags */
#define TC4X_DMU_ERR_SQER       BIT(1)      /* sequence error */
#define TC4X_DMU_ERR_PROER      BIT(2)      /* protection error */
#define TC4X_DMU_ERR_PVER       BIT(6)      /* program verify error */
#define TC4X_DMU_ERR_EVER       BIT(7)      /* erase verify error */

#define TYPE_TC4X_FLASH_POLICY "tc4x-flash-policy"
typedef struct Tc4xFlashPolicyClass Tc4xFlashPolicyClass;
DECLARE_CLASS_CHECKERS(Tc4xFlashPolicyClass, TC4X_FLASH_POLICY,
                       TYPE_TC4X_FLASH_POLICY)
#define TC4X_FLASH_POLICY(obj) \
    INTERFACE_CHECK(Tc4xFlashPolicy, (obj), TYPE_TC4X_FLASH_POLICY)

typedef struct Tc4xFlashPolicy Tc4xFlashPolicy;

/* The board's flash rules; every method is optional */
struct Tc4xFlashPolicyClass {
    InterfaceClass parent_class;

    /*
     * Place [@addr, @addr + @len) of a command in one flash (@flash,
     * @off), or return false: no flash there, the command fails (SQER).
     * Without it the PFLASH storage is linear from both read windows and
     * the DFLASH from its window.
     */
    bool (*locate)(Tc4xFlashPolicy *p, uint32_t addr, uint64_t len,
                   Tc4xFlash *flash, uint64_t *off);
    /*
     * Error flags (TC4X_DMU_ERR_*) that refuse @cmd, or 0 to let it run;
     * a refused command has no effect
     */
    uint32_t (*permit)(Tc4xFlashPolicy *p, const Tc4xDmuCommand *cmd);
    /*
     * Sees every permitted command before it runs; returns true when it
     * executed or refused the command itself, raising error flags with
     * tc4x_flash_dmu_error(), false to let the flash run it.  Either way
     * the flash updates the request status.
     */
    bool (*command)(Tc4xFlashPolicy *p, const Tc4xDmuCommand *cmd);
    /*
     * Sees the error flags @err (TC4X_DMU_ERR_*) raised on the CSRM
     * command interface (@csrm) or the host's, whoever raised them, once
     * they are set.  Without it a failed command has no effect beyond its
     * flags.  It must not raise error flags itself.
     */
    void (*error)(Tc4xFlashPolicy *p, bool csrm, uint32_t err);
    /* At every reset, once the flash contents are final */
    void (*reset)(Tc4xFlashPolicy *p);
};

#define TC4X_DMU_PAGE_BUF_SIZE  512     /* IFXFLASH_PFLASH_BURST_LENGTH */

/* A DMU command interface: the host's (HCI) or the CSRM's (CSCI) */
typedef struct Tc4xDmuIf {
    uint32_t status;    /* addresses of its STATUS and ERR registers */
    uint32_t err;
    bool csrm;
    uint8_t page_buf[TC4X_DMU_PAGE_BUF_SIZE];
    unsigned page_len;
    uint32_t cmd_addr;
    uint32_t cmd_count;
    bool cmd_armed;     /* AAA8 <- 0xA0 / 0x80 seen, waiting for the opcode */
    uint32_t cmd_class;
} Tc4xDmuIf;

struct Tc4xFlashState {
    SysBusDevice parent_obj;

    Tc4xSfrStore *sfr;
    Object *policy;
    Tc4xDmuIf dmu[2];           /* host, CSRM */
    Tc4xDmuIf *dmu_cur;         /* the interface running a command */

    BlockBackend *blk[TC4X_FLASH_COUNT];
    MemoryRegion mem[TC4X_FLASH_COUNT];     /* the storage */
    MemoryRegion pflash_win[2], pflash_linear[2], dflash_win;
    MemoryRegion *pflash_maps[2][TC4X_PFLASH_MAPS_MAX];
    unsigned n_pflash_maps;
    bool kept;
    bool seeded;        /* the images had their chance to seed the flashes */
};

/* Size of @flash's storage */
uint64_t tc4x_flash_size(Tc4xFlash flash);

/* The flashes came from their drives (board init on) */
bool tc4x_flash_is_kept(Tc4xFlashState *s);

/*
 * Read, write or fill [@off, @off + @len) of @flash's storage.  Reads
 * beyond the storage give erased cells; writes must stay inside it.
 */
void tc4x_flash_storage_read(Tc4xFlashState *s, Tc4xFlash flash,
                             uint64_t off, void *buf, uint64_t len);
void tc4x_flash_storage_write(Tc4xFlashState *s, Tc4xFlash flash,
                              uint64_t off, const void *buf, uint64_t len);
void tc4x_flash_storage_fill(Tc4xFlashState *s, Tc4xFlash flash,
                             uint64_t off, uint8_t value, uint64_t len);

/*
 * A write of @val to the SFR at @addr: true when it is a DMU command
 * cycle or a CLRERR write, which the flash served
 */
bool tc4x_flash_dmu_write(Tc4xFlashState *s, uint32_t addr, uint32_t val);

/*
 * Raise the error flags @err (TC4X_DMU_ERR_*) of the CSRM command
 * interface (@csrm) or the host's, and pass them to the policy's error
 * method
 */
void tc4x_flash_dmu_error(Tc4xFlashState *s, bool csrm, uint32_t err);

/*
 * A debugger's write of @len bytes at system address @addr: when the
 * address shows flash storage, patch it (written through) and return
 * true
 */
bool tc4x_flash_patch(Tc4xFlashState *s, hwaddr addr, const void *buf,
                      unsigned len);

/*
 * The PFLASH read windows show the storage with @maps (n = 0: linearly),
 * replacing the maps of the previous call.  Each map must lie inside the
 * window and the storage.
 */
void tc4x_flash_set_maps(Tc4xFlashState *s, const Tc4xPflashMap *maps,
                         unsigned n);

#endif
