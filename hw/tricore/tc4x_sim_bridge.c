/*
 * The sim bridge of the tc4x machines (TYPE_TC4X_SIM_BRIDGE) and the
 * interface of the objects that serve its commands
 * (TYPE_TC4X_SIM_COMMANDS).
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "hw/tricore/tc4x_master.h"
#include "hw/tricore/tc4x_sim_bridge.h"
#include "tc4x_soc.h"

/* A command of tc4x_sim_add_command(): any word starting with prefix */
typedef struct Tc4xSimLegacyCommand {
    const char *prefix;
    Tc4xSimCommandFn fn;
    void *opaque;
    Tc4xSoc *qspi_lock;     /* runs under the QSPI bus locks of this SoC */
} Tc4xSimLegacyCommand;

/* Everything under the BQL */
struct Tc4xSimBridge {
    DeviceState parent_obj;

    CharFrontend chr;
    Tc4xSoc *soc;

    GString *line;          /* received, up to its newline */
    MemTxAttrs attrs;       /* the bus master of r and w */
    /* command word -> GPtrArray of the objects that serve it */
    GHashTable *servers;
    bool servers_found;
    GArray *legacy;         /* Tc4xSimLegacyCommand */
};

static Tc4xSimBridge *sim_bridge;

bool tc4x_sim_parse_uint(const char *arg, uint64_t max, uint64_t *val)
{
    uint64_t v;

    /* qemu_strtou64 takes "-1" as UINT64_MAX */
    if (arg[0] == '-' || qemu_strtou64(arg, NULL, 0, &v) < 0 || v > max) {
        return false;
    }
    *val = v;
    return true;
}

bool tc4x_sim_parse_int(const char *arg, int64_t min, int64_t max,
                        int64_t *val)
{
    int64_t v;

    if (qemu_strtoi64(arg, NULL, 0, &v) < 0 || v < min || v > max) {
        return false;
    }
    *val = v;
    return true;
}

bool tc4x_sim_parse_double(const char *arg, double *val)
{
    return qemu_strtod_finite(arg, NULL, val) == 0;
}

void tc4x_sim_event(const char *fmt, ...)
{
    g_autofree char *text = NULL;
    g_autofree char *line = NULL;
    va_list ap;

    assert(bql_locked());
    if (!sim_bridge || !qemu_chr_fe_backend_connected(&sim_bridge->chr)) {
        return;
    }
    va_start(ap, fmt);
    text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    line = g_strdup_printf("ev %s\n", text);
    qemu_chr_fe_write_all(&sim_bridge->chr, (const uint8_t *)line,
                          strlen(line));
}

/* The reply line of an error, on one line */
static void sim_reply_error(GString *out, Error *err)
{
    g_autofree char *msg = g_strdup(error_get_pretty(err));

    g_strdelimit(msg, "\r\n", ' ');
    g_string_append_printf(out, "err %s\n", msg);
    error_free(err);
}

static bool sim_access_size(const char *arg, uint64_t *size)
{
    return tc4x_sim_parse_uint(arg, 4, size) && *size != 0 && *size != 3;
}

/*
 * r ADDR [SIZE]: read through the bus as the selected master.  A refused
 * read is an error that still carries the value the bus returned, in the
 * place of the value of "ok VALUE".
 */
static void sim_read(Tc4xSimBridge *b, int argc, char **argv, GString *out)
{
    uint64_t addr, size = 4;
    uint32_t v = 0;

    if (argc < 2 || argc > 3 ||
        !tc4x_sim_parse_uint(argv[1], UINT32_MAX, &addr) ||
        (argc == 3 && !sim_access_size(argv[2], &size))) {
        g_string_append(out, "err usage: r ADDR [1|2|4]\n");
        return;
    }
    if (address_space_read(&address_space_memory, addr, b->attrs, &v,
                           size) != MEMTX_OK) {
        g_string_append_printf(out, "err 0x%08x bus error at 0x%08" PRIx64
                               "\n", le32_to_cpu(v), addr);
        return;
    }
    g_string_append_printf(out, "ok 0x%08x\n", le32_to_cpu(v));
}

/* w ADDR VAL [SIZE]: write through the bus; as a debugger, patch flash */
static void sim_write(Tc4xSimBridge *b, int argc, char **argv, GString *out)
{
    uint64_t addr, val, size = 4;
    uint32_t v;

    if (argc < 3 || argc > 4 ||
        !tc4x_sim_parse_uint(argv[1], UINT32_MAX, &addr) ||
        (argc == 4 && !sim_access_size(argv[3], &size)) ||
        !tc4x_sim_parse_uint(argv[2], MAKE_64BIT_MASK(0, size * 8), &val)) {
        g_string_append(out, "err usage: w ADDR VAL [1|2|4] (VAL fits "
                        "in SIZE bytes)\n");
        return;
    }
    v = cpu_to_le32(val);
    if (!tc4x_flash_patch(tc4x_soc_flash(b->soc), addr, &v, size) &&
        address_space_write(&address_space_memory, addr, b->attrs, &v,
                            size) != MEMTX_OK) {
        g_string_append_printf(out, "err bus error at 0x%08" PRIx64 "\n",
                               addr);
        return;
    }
    g_string_append(out, "ok\n");
}

/* master [debug | cpu N [safe] | dma CH]: the bus master of r and w */
static void sim_master(Tc4xSimBridge *b, int argc, char **argv,
                       GString *out)
{
    bool safe = argc == 4 && !strcmp(argv[3], "safe");
    unsigned m;
    uint64_t n;
    int core;

    if (argc == 2 && !strcmp(argv[1], "debug")) {
        b->attrs = tc4x_master_attrs(TC4X_MASTER_DEBUG, false);
    } else if ((argc == 3 || safe) && !strcmp(argv[1], "cpu") &&
               tc4x_sim_parse_uint(argv[2], TC4X_MASTER_CPU_COUNT - 1, &n)) {
        b->attrs = tc4x_master_attrs(TC4X_MASTER_CPU(n), safe);
    } else if (argc == 3 && !strcmp(argv[1], "dma") &&
               tc4x_sim_parse_uint(argv[2], TC4X_MASTER_DMA_COUNT - 1, &n)) {
        b->attrs = tc4x_master_attrs(TC4X_MASTER_DMA(n), false);
    } else if (argc != 1) {
        g_string_append(out,
                        "err usage: master [debug | cpu N [safe] | dma CH]\n");
        return;
    }
    m = TC4X_REQUESTER_MASTER(b->attrs.requester_id);
    core = tc4x_master_core(b->attrs);
    if (m == TC4X_MASTER_DEBUG) {
        g_string_append(out, "ok debug\n");
    } else if (core >= 0) {
        g_string_append_printf(out, "ok cpu %d%s\n", core,
                               TC4X_REQUESTER_SAFE(b->attrs.requester_id) ?
                               " safe" : "");
    } else {
        g_string_append_printf(out, "ok dma %u\n", m - TC4X_MASTER_DMA(0));
    }
}

static Object *sim_resolve(const char *path, GString *out)
{
    bool ambiguous = false;
    Object *obj = object_resolve_path(path, &ambiguous);

    if (!obj) {
        g_string_append_printf(out, "err %s path '%s'\n",
                               ambiguous ? "ambiguous" : "no object at",
                               path);
    }
    return obj;
}

/* get PATH PROPERTY: the value of a QOM property */
static void sim_get(Tc4xSimBridge *b, int argc, char **argv, GString *out)
{
    g_autofree char *val = NULL;
    Error *err = NULL;
    Object *obj;

    if (argc != 3) {
        g_string_append(out, "err usage: get PATH PROPERTY\n");
        return;
    }
    obj = sim_resolve(argv[1], out);
    if (!obj) {
        return;
    }
    val = object_property_print(obj, argv[2], false, &err);
    if (!val) {
        sim_reply_error(out, err);
        return;
    }
    g_string_append_printf(out, "ok %s\n", val);
}

/* set PATH PROPERTY VALUE: set a QOM property from its text form */
static void sim_set(Tc4xSimBridge *b, int argc, char **argv, GString *out)
{
    g_autofree char *val = NULL;
    Error *err = NULL;
    Object *obj;

    if (argc < 4) {
        g_string_append(out, "err usage: set PATH PROPERTY VALUE\n");
        return;
    }
    obj = sim_resolve(argv[1], out);
    if (!obj) {
        return;
    }
    val = g_strjoinv(" ", &argv[3]);
    if (!object_property_parse(obj, argv[2], val, &err)) {
        sim_reply_error(out, err);
        return;
    }
    g_string_append(out, "ok\n");
}

static void sim_vmstop(Tc4xSimBridge *b, int argc, char **argv,
                       GString *out)
{
    if (argc != 1) {
        g_string_append(out, "err usage: vmstop\n");
        return;
    }
    vm_stop(RUN_STATE_PAUSED);
    g_string_append(out, "ok\n");
}

static void sim_vmcont(Tc4xSimBridge *b, int argc, char **argv,
                       GString *out)
{
    if (argc != 1) {
        g_string_append(out, "err usage: vmcont\n");
        return;
    }
    vm_start();
    g_string_append(out, "ok\n");
}

static const struct {
    const char *name;
    void (*fn)(Tc4xSimBridge *b, int argc, char **argv, GString *out);
} sim_builtins[] = {
    { "r", sim_read },
    { "w", sim_write },
    { "master", sim_master },
    { "get", sim_get },
    { "set", sim_set },
    { "vmstop", sim_vmstop },
    { "vmcont", sim_vmcont },
};

static int sim_add_servers(Object *obj, void *opaque)
{
    Tc4xSimBridge *b = opaque;
    Tc4xSimCommandsClass *k;

    if (!object_dynamic_cast(obj, TYPE_TC4X_SIM_COMMANDS)) {
        return 0;
    }
    k = TC4X_SIM_COMMANDS_GET_CLASS(obj);
    for (const char *const *w = k->commands; w && *w; w++) {
        GPtrArray *objs = g_hash_table_lookup(b->servers, *w);

        if (!objs) {
            objs = g_ptr_array_new_with_free_func(object_unref);
            g_hash_table_insert(b->servers, (gpointer)*w, objs);
        }
        g_ptr_array_add(objs, object_ref(obj));
    }
    return 0;
}

/* The objects of the QOM tree that serve commands */
static void sim_find_servers(Tc4xSimBridge *b)
{
    g_hash_table_remove_all(b->servers);
    object_child_foreach_recursive(object_get_root(), sim_add_servers, b);
    b->servers_found = true;
}

static bool sim_run_servers(Tc4xSimBridge *b, int argc, char **argv,
                            GString *out)
{
    GPtrArray *objs = g_hash_table_lookup(b->servers, argv[0]);

    for (guint i = 0; objs && i < objs->len; i++) {
        Object *obj = g_ptr_array_index(objs, i);

        if (TC4X_SIM_COMMANDS_GET_CLASS(obj)->run(TC4X_SIM_COMMANDS(obj),
                                                  argc, argv, out)) {
            return true;
        }
    }
    return false;
}

static bool sim_run_legacy(Tc4xSimBridge *b, int argc, char **argv,
                           GString *out)
{
    for (guint i = 0; i < b->legacy->len; i++) {
        Tc4xSimLegacyCommand *c = &g_array_index(b->legacy,
                                                 Tc4xSimLegacyCommand, i);
        bool done;

        if (!g_str_has_prefix(argv[0], c->prefix)) {
            continue;
        }
        if (c->qspi_lock) {
            tc4x_qspi_lock(c->qspi_lock);
        }
        done = c->fn(c->opaque, argc, argv, out);
        if (c->qspi_lock) {
            tc4x_qspi_unlock(c->qspi_lock);
        }
        if (done) {
            return true;
        }
    }
    return false;
}

/*
 * Built-in commands, then the objects that serve the word, then the
 * commands of tc4x_sim_add_command().  An unknown word looks for objects
 * added since the last search.
 */
static void sim_dispatch(Tc4xSimBridge *b, int argc, char **argv,
                         GString *out)
{
    for (size_t i = 0; i < ARRAY_SIZE(sim_builtins); i++) {
        if (!strcmp(argv[0], sim_builtins[i].name)) {
            sim_builtins[i].fn(b, argc, argv, out);
            return;
        }
    }
    if (!b->servers_found) {
        sim_find_servers(b);
    }
    if (sim_run_servers(b, argc, argv, out) ||
        sim_run_legacy(b, argc, argv, out)) {
        return;
    }
    sim_find_servers(b);
    if (!sim_run_servers(b, argc, argv, out)) {
        g_string_append_printf(out, "err unknown command '%s'\n", argv[0]);
    }
}

bool tc4x_sim_bridge_exec(Tc4xSimBridge *b, const char *line, GString *out)
{
    g_auto(GStrv) words = g_strsplit_set(line, " \t\r", -1);
    g_autoptr(GPtrArray) args = g_ptr_array_new();
    size_t start = out->len;
    const char *last;
    int argc;

    assert(bql_locked());
    for (int i = 0; words[i]; i++) {
        if (words[i][0]) {
            g_ptr_array_add(args, words[i]);
        }
    }
    argc = args->len;
    g_ptr_array_add(args, NULL);
    if (argc == 0 || ((char *)args->pdata[0])[0] == '#') {
        return true;
    }
    sim_dispatch(b, argc, (char **)args->pdata, out);
    if (out->len == start || out->str[out->len - 1] != '\n') {
        g_string_append_printf(out, "%serr no reply from '%s'\n",
                               out->len == start ? "" : "\n",
                               (char *)args->pdata[0]);
    }
    /* the last line of the reply */
    last = out->str + out->len - 1;
    while (last > out->str + start && last[-1] != '\n') {
        last--;
    }
    return !g_str_has_prefix(last, "err");
}

bool tc4x_sim_bridge_run_script(Tc4xSimBridge *b, const char *path,
                                Error **errp)
{
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    GError *gerr = NULL;

    if (!g_file_get_contents(path, &text, NULL, &gerr)) {
        error_setg(errp, "cannot read '%s': %s", path, gerr->message);
        g_error_free(gerr);
        return false;
    }
    lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        g_autoptr(GString) out = g_string_new(NULL);

        if (!tc4x_sim_bridge_exec(b, lines[i], out)) {
            g_strchomp(out->str);
            error_setg(errp, "%s:%d: '%s': %s", path, i + 1,
                       g_strstrip(lines[i]), out->str);
            return false;
        }
    }
    return true;
}

void tc4x_sim_add_command(Tc4xSoc *soc, const char *prefix,
                          Tc4xSimCommandFn fn, void *opaque, bool qspi_lock)
{
    Tc4xSimLegacyCommand c = {
        .prefix = prefix, .fn = fn, .opaque = opaque,
        .qspi_lock = qspi_lock ? soc : NULL,
    };

    assert(sim_bridge);
    g_array_append_val(sim_bridge->legacy, c);
}

static int sim_can_receive(void *opaque)
{
    return 256;
}

static void sim_receive(void *opaque, const uint8_t *buf, int size)
{
    Tc4xSimBridge *b = opaque;

    for (int i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            g_autoptr(GString) out = g_string_new(NULL);

            tc4x_sim_bridge_exec(b, b->line->str, out);
            g_string_truncate(b->line, 0);
            if (out->len) {
                qemu_chr_fe_write_all(&b->chr, (const uint8_t *)out->str,
                                      out->len);
            }
        } else {
            g_string_append_c(b->line, buf[i]);
        }
    }
}

static void sim_chr_event(void *opaque, QEMUChrEvent event)
{
    Tc4xSimBridge *b = opaque;

    if (event == CHR_EVENT_CLOSED) {
        g_string_truncate(b->line, 0);
    }
}

static void tc4x_sim_bridge_realize(DeviceState *dev, Error **errp)
{
    Tc4xSimBridge *b = TC4X_SIM_BRIDGE(dev);

    if (sim_bridge) {
        error_setg(errp, "a machine has one sim bridge");
        return;
    }
    if (!b->soc) {
        error_setg(errp, "link 'soc' must be set");
        return;
    }
    b->line = g_string_new(NULL);
    b->servers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                       (GDestroyNotify)g_ptr_array_unref);
    b->legacy = g_array_new(false, false, sizeof(Tc4xSimLegacyCommand));
    b->attrs = tc4x_master_attrs(TC4X_MASTER_DEBUG, false);
    qemu_chr_fe_set_handlers(&b->chr, sim_can_receive, sim_receive,
                             sim_chr_event, NULL, b, NULL, true);
    sim_bridge = b;
}

static const Property tc4x_sim_bridge_properties[] = {
    DEFINE_PROP_CHR("chardev", Tc4xSimBridge, chr),
    DEFINE_PROP_LINK("soc", Tc4xSimBridge, soc, TYPE_TC4X_SOC, Tc4xSoc *),
};

static const VMStateDescription vmstate_tc4x_sim_bridge = {
    .name = TYPE_TC4X_SIM_BRIDGE,
    .unmigratable = 1,
};

static void tc4x_sim_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "tc4x sim bridge";
    dc->realize = tc4x_sim_bridge_realize;
    dc->vmsd = &vmstate_tc4x_sim_bridge;
    /* created by the tc4x machines */
    dc->user_creatable = false;
    device_class_set_props(dc, tc4x_sim_bridge_properties);
}

static const TypeInfo tc4x_sim_bridge_types[] = {
    {
        .name = TYPE_TC4X_SIM_COMMANDS,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(Tc4xSimCommandsClass),
    }, {
        .name = TYPE_TC4X_SIM_BRIDGE,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(Tc4xSimBridge),
        .class_init = tc4x_sim_bridge_class_init,
    },
};

DEFINE_TYPES(tc4x_sim_bridge_types)
