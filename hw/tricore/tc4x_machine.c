/*
 * Infineon AURIX TC4Dx machines: the tc4x SoC and the board around it.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qom/object_interfaces.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/address-spaces.h"
#include "system/hostmem.h"
#include "exec/icount.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "chardev/char.h"
#include "elf.h"
#include "cpu.h"
#include "hw/tricore/tc4x_machine.h"
#include "hw/tricore/tc4x_sim_bridge.h"
#include "tc4x_soc.h"

static void tc4x_load_image(Tc4xMachineState *m, const char *file)
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
    if (m->entry == 0) {
        m->entry = entry;
    }
}

static void tc4x_reset(MachineState *ms, ResetType type)
{
    Tc4xMachineState *m = TC4X_MACHINE(ms);
    Tc4xBoardClass *bc = TC4X_BOARD_GET_CLASS(m->board);

    qemu_devices_reset(type);
    if (bc->reset) {
        bc->reset(m->board, m->soc);
    }
    tc4x_soc_boot(m->soc, m->entry);
}

static void tc4x_board_list(void)
{
    g_autoptr(GSList) list = object_class_get_list_sorted(TYPE_TC4X_BOARD,
                                                          false);

    printf("Supported boards are:\n");
    for (GSList *el = list; el; el = el->next) {
        Tc4xBoardClass *bc = el->data;
        const char *type = object_class_get_name(OBJECT_CLASS(bc));
        g_autofree char *name = g_strndup(type, strlen(type) -
                                          strlen("-" TYPE_TC4X_BOARD));

        printf("%-20s %s\n", name, bc->desc);
    }
}

static Tc4xBoard *tc4x_board_create(const char *name)
{
    g_autofree char *type = g_strdup_printf("%s-" TYPE_TC4X_BOARD, name);
    ObjectClass *oc = object_class_by_name(type);

    if (!strcmp(name, "help")) {
        tc4x_board_list();
        exit(0);
    }
    if (!oc || !object_class_dynamic_cast(oc, TYPE_TC4X_BOARD) ||
        object_class_is_abstract(oc)) {
        error_report("tc4x: unknown board '%s' (board=help lists them)",
                     name);
        exit(1);
    }
    return TC4X_BOARD(object_new(type));
}

static void tc4x_apply_board(Tc4xSoc *soc, const Tc4xBoardClass *bc)
{
    tc4x_soc_add_semantics(soc, bc->reg_semantics, bc->n_reg_semantics,
                           bc->kernel_resets, bc->n_kernel_resets);
}

/* shm-dir: the RAM @name of the SoC is the shared file <shm-dir>/@name */
static void tc4x_add_shm_ram(Tc4xMachineState *m, const char *name,
                             const char *link, uint64_t size)
{
    Object *obj = object_new(TYPE_MEMORY_BACKEND_FILE);
    g_autofree char *path = g_build_filename(m->shm_dir, name, NULL);

    if (object_property_get_link(OBJECT(m->soc), link, &error_abort)) {
        error_report("tc4x: shm-dir and %s both give the RAM %s", link, name);
        exit(1);
    }
    object_property_set_str(obj, "mem-path", path, &error_fatal);
    object_property_set_int(obj, "size", size, &error_fatal);
    object_property_set_bool(obj, "share", true, &error_fatal);
    /* the RAM block keeps the RAM's name */
    object_property_set_bool(obj, "x-use-canonical-path-for-ramblock-id",
                             false, &error_fatal);
    object_property_add_child(object_get_objects_root(), name, obj);
    user_creatable_complete(USER_CREATABLE(obj), &error_fatal);
    object_property_set_link(OBJECT(m->soc), link, obj, &error_fatal);
    object_unref(obj);
}

/*
 * flash-dir: the drive of flash @index is the file DIR/@name, created
 * erased when it does not exist
 */
static DriveInfo *tc4x_flash_dir_drive(Tc4xMachineState *m, int index,
                                       const char *name, uint64_t size)
{
    g_autofree char *path = g_build_filename(m->flash_dir, name, NULL);
    struct stat st;
    int fd;

    if (drive_get(IF_PFLASH, 0, index)) {
        error_report("tc4x: flash-dir and -drive if=pflash,index=%d both "
                     "give the %s", index, name);
        exit(1);
    }
    if (g_mkdir_with_parents(m->flash_dir, 0700) < 0) {
        error_report("tc4x: cannot create flash-dir '%s': %s", m->flash_dir,
                     strerror(errno));
        exit(1);
    }
    fd = qemu_create(path, O_RDWR, 0600, &error_fatal);
    if (fstat(fd, &st) < 0 || (!st.st_size && ftruncate(fd, size) < 0)) {
        error_report("tc4x: cannot create '%s': %s", path, strerror(errno));
        exit(1);
    }
    qemu_close(fd);
    return drive_new(drive_add(IF_PFLASH, index, path, "format=raw"),
                     IF_PFLASH, &error_fatal);
}

/*
 * The drives of the flashes: -drive if=pflash with index 0 (PFLASH) and
 * index 1 (DFLASH), or the files of flash-dir
 */
static void tc4x_set_flash_drives(Tc4xMachineState *m, DeviceState *soc)
{
    static const struct {
        const char *name;
        uint64_t size;
    } flashes[] = {
        { "pflash", TC4X_PFLASH_SIZE },
        { "dflash", TC4X_DFLASH_SIZE },
    };

    for (int i = 0; i < ARRAY_SIZE(flashes); i++) {
        DriveInfo *dinfo = m->flash_dir ?
            tc4x_flash_dir_drive(m, i, flashes[i].name, flashes[i].size) :
            drive_get(IF_PFLASH, 0, i);

        if (dinfo) {
            qdev_prop_set_drive_err(soc, flashes[i].name,
                                    blk_by_legacy_dinfo(dinfo), &error_fatal);
        }
    }
}

/*
 * The sim bridge on the character device sim-chardev names, else on the
 * second serial port
 */
static void tc4x_add_sim_bridge(Tc4xMachineState *m)
{
    DeviceState *dev = qdev_new(TYPE_TC4X_SIM_BRIDGE);
    Chardev *chr = serial_hd(1);

    if (m->sim_chardev) {
        chr = qemu_chr_find(m->sim_chardev);
        if (!chr) {
            error_report("tc4x: sim-chardev '%s' is not a character device",
                         m->sim_chardev);
            exit(1);
        }
    }
    object_property_add_child(OBJECT(m), "sim-bridge", OBJECT(dev));
    if (chr) {
        qdev_prop_set_chr(dev, "chardev", chr);
    }
    object_property_set_link(OBJECT(dev), "soc", OBJECT(m->soc),
                             &error_abort);
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    m->sim_bridge = TC4X_SIM_BRIDGE(dev);
}

/*
 * Deterministic mode: guest time and every device event follow the
 * instruction count, which is the same in every run only with a fixed
 * icount shift and no real-time sleep of idle CPUs (-icount also makes
 * TCG single-threaded); guest random numbers, which board devices draw
 * with qemu_guest_getrandom(), then come from -seed.
 */
static bool tc4x_check_deterministic(Tc4xMachineState *m, Error **errp)
{
    if (m->time_scale < 1) {
        error_setg(errp, "tc4x: time-scale must be at least 1");
        return false;
    }
    if (!m->deterministic) {
        if (m->time_scale != 1) {
            error_setg(errp, "tc4x: time-scale needs deterministic=on");
            return false;
        }
        return true;
    }
    if (icount_enabled() != ICOUNT_PRECISE || icount_sleep_enabled()) {
        error_setg(errp, "tc4x: deterministic=on needs "
                   "-icount shift=N,sleep=off");
        return false;
    }
    if (!qemu_guest_random_seeded()) {
        error_setg(errp, "tc4x: deterministic=on needs -seed");
        return false;
    }
    return true;
}

static void tc4x_init(MachineState *ms)
{
    Tc4xMachineState *m = TC4X_MACHINE(ms);
    Tc4xMachineClass *tmc = TC4X_MACHINE_GET_CLASS(m);
    Tc4xBoardClass *bc;
    DeviceState *dev;
    QList *straps;

    tc4x_check_deterministic(m, &error_fatal);
    assert(m->board_name || tmc->board);
    m->board = tc4x_board_create(m->board_name ? m->board_name : tmc->board);
    object_property_add_child(OBJECT(ms), "tc4x-board", OBJECT(m->board));
    object_unref(OBJECT(m->board));
    bc = TC4X_BOARD_GET_CLASS(m->board);

    dev = qdev_new(TYPE_TC4X_SOC);
    m->soc = TC4X_SOC(dev);
    object_property_add_child(OBJECT(ms), "soc", OBJECT(dev));
    qdev_prop_set_string(dev, "cpu-type", ms->cpu_type);
    qdev_prop_set_uint32(dev, "console", bc->console);
    qdev_prop_set_chr(dev, "console-chardev", serial_hd(0));
    qdev_prop_set_bit(dev, "imprecise-io", m->imprecise_io);
    qdev_prop_set_bit(dev, "deterministic", m->deterministic);
    qdev_prop_set_uint32(dev, "time-scale", m->time_scale);
    straps = qlist_new();
    for (unsigned i = 0; i < bc->n_inputs; i++) {
        const Tc4xBoardInput *in = &bc->inputs[i];

        qlist_append_int(straps, TC4X_PORTS_STRAP(in->port, in->pin,
                                                  in->level));
    }
    qdev_prop_set_array(dev, "port-straps", straps);
    tc4x_set_flash_drives(m, dev);
    if (m->shm_dir) {
        if (g_mkdir_with_parents(m->shm_dir, 0700) < 0) {
            error_report("tc4x: cannot create shm-dir '%s': %s", m->shm_dir,
                         strerror(errno));
            exit(1);
        }
        for (int c = 0; c < TC4X_NUM_CORES; c++) {
            g_autofree char *name = g_strdup_printf("core%d.spr", c);
            g_autofree char *link = g_strdup_printf("core%d-memdev", c);

            tc4x_add_shm_ram(m, name, link, TC4X_CORE_RAM_SIZE);
        }
        tc4x_add_shm_ram(m, "lmu", "lmu-memdev", TC4X_LMU_SIZE);
    }
    if (bc->wire) {
        bc->wire(m->board, m->soc);
    }
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    qemu_register_resettable(OBJECT(dev));
    tc4x_add_sim_bridge(m);

    tc4x_apply_board(m->soc, bc);
    if (bc->init) {
        bc->init(m->board, m->soc);
    }

    if (m->sfr_script) {
        Error *err = NULL;

        if (!tc4x_sim_bridge_run_script(m->sim_bridge, m->sfr_script,
                                        &err)) {
            error_reportf_err(err, "tc4x: sfr-script: ");
            exit(1);
        }
    }

    if (ms->kernel_filename) {
        g_auto(GStrv) files = g_strsplit(ms->kernel_filename, ",", -1);

        for (int i = 0; files[i]; i++) {
            tc4x_load_image(m, files[i]);
        }
    }
}

static char *tc4x_get_sfr_script(Object *obj, Error **errp)
{
    return g_strdup(TC4X_MACHINE(obj)->sfr_script);
}

static void tc4x_set_sfr_script(Object *obj, const char *value, Error **errp)
{
    Tc4xMachineState *m = TC4X_MACHINE(obj);

    g_free(m->sfr_script);
    m->sfr_script = g_strdup(value);
}

static char *tc4x_get_sim_chardev(Object *obj, Error **errp)
{
    return g_strdup(TC4X_MACHINE(obj)->sim_chardev);
}

static void tc4x_set_sim_chardev(Object *obj, const char *value,
                                 Error **errp)
{
    Tc4xMachineState *m = TC4X_MACHINE(obj);

    g_free(m->sim_chardev);
    m->sim_chardev = g_strdup(value);
}

static char *tc4x_get_board(Object *obj, Error **errp)
{
    return g_strdup(TC4X_MACHINE(obj)->board_name);
}

static void tc4x_set_board(Object *obj, const char *value, Error **errp)
{
    Tc4xMachineState *m = TC4X_MACHINE(obj);

    g_free(m->board_name);
    m->board_name = g_strdup(value);
}

static char *tc4x_get_shm_dir(Object *obj, Error **errp)
{
    return g_strdup(TC4X_MACHINE(obj)->shm_dir);
}

static void tc4x_set_shm_dir(Object *obj, const char *value, Error **errp)
{
    Tc4xMachineState *m = TC4X_MACHINE(obj);

    g_free(m->shm_dir);
    m->shm_dir = g_strdup(value);
}

static char *tc4x_get_flash_dir(Object *obj, Error **errp)
{
    return g_strdup(TC4X_MACHINE(obj)->flash_dir);
}

static void tc4x_set_flash_dir(Object *obj, const char *value, Error **errp)
{
    Tc4xMachineState *m = TC4X_MACHINE(obj);

    g_free(m->flash_dir);
    m->flash_dir = g_strdup(value);
}

static bool tc4x_get_imprecise_io(Object *obj, Error **errp)
{
    return TC4X_MACHINE(obj)->imprecise_io;
}

static void tc4x_set_imprecise_io(Object *obj, bool value, Error **errp)
{
    TC4X_MACHINE(obj)->imprecise_io = value;
}

static bool tc4x_get_deterministic(Object *obj, Error **errp)
{
    return TC4X_MACHINE(obj)->deterministic;
}

static void tc4x_set_deterministic(Object *obj, bool value, Error **errp)
{
    TC4X_MACHINE(obj)->deterministic = value;
}

static void tc4x_get_time_scale(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &TC4X_MACHINE(obj)->time_scale, errp);
}

static void tc4x_set_time_scale(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &TC4X_MACHINE(obj)->time_scale, errp);
}

static void tc4x_machine_instance_init(Object *obj)
{
    TC4X_MACHINE(obj)->time_scale = 1;
}

static void tc4x_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "shm-dir", tc4x_get_shm_dir,
                                  tc4x_set_shm_dir);
    object_class_property_set_description(oc, "shm-dir",
        "Directory (e.g. /dev/shm/tc4x) for file-backed shared guest RAM");

    object_class_property_add_str(oc, "flash-dir", tc4x_get_flash_dir,
                                  tc4x_set_flash_dir);
    object_class_property_set_description(oc, "flash-dir",
        "Directory for the PFLASH and DFLASH drives (raw files pflash and "
        "dflash): files with contents keep them, the images seeding only "
        "the RAMs");

    object_class_property_add_str(oc, "sfr-script", tc4x_get_sfr_script,
                                  tc4x_set_sfr_script);
    object_class_property_set_description(oc, "sfr-script",
        "File of sim-bridge commands run at machine init; a command "
        "that fails stops QEMU");

    object_class_property_add_str(oc, "sim-chardev", tc4x_get_sim_chardev,
                                  tc4x_set_sim_chardev);
    object_class_property_set_description(oc, "sim-chardev",
        "ID of the character device of the sim bridge (default: the "
        "second serial port)");

    object_class_property_add_bool(oc, "imprecise-io", tc4x_get_imprecise_io,
                                   tc4x_set_imprecise_io);
    object_class_property_set_description(oc, "imprecise-io",
        "SFR accesses take effect at the next TB instead of the next "
        "instruction: faster, not instruction-precise (default off)");

    object_class_property_add_bool(oc, "deterministic",
                                   tc4x_get_deterministic,
                                   tc4x_set_deterministic);
    object_class_property_set_description(oc, "deterministic",
        "Guest time and device events follow the instruction count, so "
        "runs repeat exactly; needs -icount shift=N,sleep=off, "
        "-accel tcg,thread=single and -seed (default off)");

    object_class_property_add(oc, "time-scale", "uint32",
                              tc4x_get_time_scale, tc4x_set_time_scale,
                              NULL, NULL);
    object_class_property_set_description(oc, "time-scale",
        "With deterministic=on: guest time runs at 1/time-scale of "
        "QEMU_CLOCK_VIRTUAL, fixed (default 1)");

    mc->init = tc4x_init;
    mc->reset = tc4x_reset;
    mc->max_cpus = TC4X_NUM_CORES;
    mc->default_cpus = TC4X_NUM_CORES;
    mc->min_cpus = TC4X_NUM_CORES;
    mc->default_cpu_type = TRICORE_CPU_TYPE_NAME("tc4x");
}

static void tc4x_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Tc4xMachineClass *tmc = TC4X_MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "board", tc4x_get_board,
                                  tc4x_set_board);
    object_class_property_set_description(oc, "board",
        "Board around the SoC (default generic, board=help lists them)");

    mc->desc = "Infineon AURIX TC4Dx (6x TriCore 1.8)";
    tmc->board = "generic";
}

static void tc4x_generic_board_class_init(ObjectClass *oc, const void *data)
{
    Tc4xBoardClass *bc = TC4X_BOARD_CLASS(oc);

    bc->desc = "SoC only: console on ASCLIN0, no board inputs, no PHY";
}

static const TypeInfo tc4x_machine_types[] = {
    {
        .name = TYPE_TC4X_MACHINE,
        .parent = TYPE_MACHINE,
        .abstract = true,
        .instance_size = sizeof(Tc4xMachineState),
        .instance_init = tc4x_machine_instance_init,
        .class_size = sizeof(Tc4xMachineClass),
        .class_init = tc4x_machine_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("tc4x"),
        .parent = TYPE_TC4X_MACHINE,
        .class_init = tc4x_class_init,
    }, {
        .name = TYPE_TC4X_BOARD,
        .parent = TYPE_OBJECT,
        .abstract = true,
        .instance_size = sizeof(Tc4xBoard),
        .class_size = sizeof(Tc4xBoardClass),
    }, {
        .name = TC4X_BOARD_TYPE_NAME("generic"),
        .parent = TYPE_TC4X_BOARD,
        .class_init = tc4x_generic_board_class_init,
    },
};

DEFINE_TYPES(tc4x_machine_types)
