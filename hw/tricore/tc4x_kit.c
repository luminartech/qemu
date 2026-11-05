/*
 * Infineon AURIX TC4D7 lite kit (KIT_A3G_TC4D7_LITE) around the tc4x SoC.
 *
 * The wiring is the one of the kit's board support header in Infineon's
 * public code examples (Boards/KIT_TC4D7_LITE/pin.h of the TC4D7 examples
 * in https://github.com/Infineon/AURIX_code_examples):
 *
 *   - user LEDs on P03.9 and P03.10, lit while the pin is low (the
 *     GPIO_LED_Button example drives the LED low while the button is
 *     pressed); changes are reported as trace events
 *   - user button on P03.11 to ground, read through the pin's pull-up
 *   - debug UART: ASCLIN0 on P14.0/P14.1, the console
 *   - potentiometer on TMADC0 channel 0
 *   - 2-Kbit serial EEPROM at I2C0 address 0x50 holding an EUI-48 node
 *     address at 0xFA (I2C_Read_Ext_Device example; Microchip 24AA02E48
 *     data sheet)
 *   - TI DP83825I RMII PHY at MDIO address 0 of GETH0 port 0 (ETH_Demo
 *     example; IEEE 802.3 Clause 22 registers, PHY identifier 0x2000A140)
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/core/resettable.h"
#include "hw/tricore/tc4x.h"
#include "hw/tricore/tc4x_board.h"
#include "hw/tricore/tc4x_machine.h"
#include "hw/tricore/tc4x_sim_bridge.h"
#include "system/reset.h"
#include "trace.h"

#define KIT_LED_PORT        3u
#define KIT_LED1_PIN        9u
#define KIT_LED2_PIN        10u
#define KIT_BUTTON_PORT     3u
#define KIT_BUTTON_PIN      11u
#define KIT_POTI_KERNEL     0u
#define KIT_POTI_CH         0u
#define KIT_POTI_VOLTS      1.65
#define KIT_EEPROM_BUS      0u
#define KIT_EEPROM_ADDR     0x50u
#define KIT_EEPROM_SIZE     256u
#define KIT_EEPROM_EUI48    0xFAu
/* The upper half of the 24AA02E48 is write-protected */
#define KIT_EEPROM_WP_START 0x80u
#define KIT_PHY_ADDR        0u

/* IEEE 802.3 Clause 22 registers and bits */
#define MII_BMCR            0x00u
#define MII_BMSR            0x01u
#define MII_PHYID1          0x02u
#define MII_PHYID2          0x03u
#define MII_ANAR            0x04u
#define MII_ANLPAR          0x05u
#define MII_ANER            0x06u
#define BMCR_RESET          BIT(15)
#define BMCR_ANENABLE       BIT(12)
#define BMCR_ANRESTART      BIT(9)
#define BMCR_DEFAULT        (BIT(13) | BMCR_ANENABLE | BIT(8))
/* 100BASE-TX FD/HD, 10BASE-T FD/HD, preamble suppression, AN, extended */
#define BMSR_ABILITIES      0x7849u
#define BMSR_ANEGCOMPLETE   BIT(5)
#define BMSR_LSTATUS        BIT(2)
#define ANAR_DEFAULT        0x01E1u
/* Link partner: 100BASE-TX FD/HD, 10BASE-T FD/HD, acknowledge, IEEE 802.3 */
#define ANLPAR_PARTNER      0x45E1u
#define ANER_LPANABLE       BIT(0)
#define DP83825I_PHYID1     0x2000u
#define DP83825I_PHYID2     0xA140u

#define KIT_NAME            "kit-tc4d7-lite"
#define TYPE_KIT_TC4D7_LITE TC4X_BOARD_TYPE_NAME(KIT_NAME)
OBJECT_DECLARE_SIMPLE_TYPE(KitTc4d7Lite, KIT_TC4D7_LITE)

/*
 * The DP83825I: the link is up while the cable is plugged and the port has
 * its network link
 */
#define TYPE_KIT_PHY        "dp83825i-phy"
OBJECT_DECLARE_SIMPLE_TYPE(KitPhy, KIT_PHY)

struct KitPhy {
    DeviceState parent_obj;

    uint16_t bmcr;
    uint16_t anar;
    bool cable;
    bool port_up;
};

struct KitTc4d7Lite {
    Tc4xBoard parent_obj;

    Tc4xSoc *soc;
    KitPhy *phy;
    uint8_t eeprom[KIT_EEPROM_SIZE];
    uint8_t eeprom_ptr;
    bool eeprom_addressed;
};

/* The node address the EEPROM holds: QEMU's default NIC address */
static const uint8_t kit_eui48[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

static void kit_led_changed(void *opaque, unsigned port, uint32_t old,
                            uint32_t new)
{
    static const unsigned pins[] = { KIT_LED1_PIN, KIT_LED2_PIN };

    for (unsigned i = 0; i < ARRAY_SIZE(pins); i++) {
        if ((old ^ new) & BIT(pins[i])) {
            trace_tc4x_kit_led(i + 1, !(new & BIT(pins[i])));
        }
    }
}

static bool kit_eeprom_start(void *opaque, bool read)
{
    KitTc4d7Lite *k = opaque;

    k->eeprom_addressed = read;
    return true;
}

/* The first byte of a write sets the word address, the rest is data */
static void kit_eeprom_write(void *opaque, const uint8_t *data, unsigned len)
{
    KitTc4d7Lite *k = opaque;

    for (unsigned i = 0; i < len; i++) {
        if (!k->eeprom_addressed) {
            k->eeprom_ptr = data[i];
            k->eeprom_addressed = true;
            continue;
        }
        if (k->eeprom_ptr < KIT_EEPROM_WP_START) {
            k->eeprom[k->eeprom_ptr] = data[i];
        }
        k->eeprom_ptr++;
    }
}

static void kit_eeprom_read(void *opaque, uint8_t *data, unsigned len)
{
    KitTc4d7Lite *k = opaque;

    for (unsigned i = 0; i < len; i++) {
        data[i] = k->eeprom[k->eeprom_ptr++];
    }
}

static const Tc4xI2cSlaveOps kit_eeprom_ops = {
    .start = kit_eeprom_start,
    .write = kit_eeprom_write,
    .read = kit_eeprom_read,
};

static void kit_phy_reset_hold(Object *obj, ResetType type)
{
    KitPhy *phy = KIT_PHY(obj);

    phy->bmcr = BMCR_DEFAULT;
    phy->anar = ANAR_DEFAULT;
}

/* Auto-negotiation completes at once with a 100BASE-TX full duplex partner */
static uint16_t kit_phy_read22(Tc4xGethPhy *gphy, unsigned reg)
{
    KitPhy *phy = KIT_PHY(gphy);
    bool link = phy->cable && phy->port_up;
    bool an_done = link && (phy->bmcr & BMCR_ANENABLE);

    switch (reg) {
    case MII_BMCR:
        return phy->bmcr;
    case MII_BMSR:
        return BMSR_ABILITIES | (link ? BMSR_LSTATUS : 0) |
               (an_done ? BMSR_ANEGCOMPLETE : 0);
    case MII_PHYID1:
        return DP83825I_PHYID1;
    case MII_PHYID2:
        return DP83825I_PHYID2;
    case MII_ANAR:
        return phy->anar;
    case MII_ANLPAR:
        return an_done ? ANLPAR_PARTNER : 0;
    case MII_ANER:
        return an_done ? ANER_LPANABLE : 0;
    default:
        return 0;
    }
}

static void kit_phy_write22(Tc4xGethPhy *gphy, unsigned reg, uint16_t val)
{
    KitPhy *phy = KIT_PHY(gphy);

    switch (reg) {
    case MII_BMCR:
        if (val & BMCR_RESET) {
            kit_phy_reset_hold(OBJECT(phy), RESET_TYPE_COLD);
        } else {
            phy->bmcr = val & ~BMCR_ANRESTART;
        }
        break;
    case MII_ANAR:
        phy->anar = val;
        break;
    }
}

static void kit_phy_port_link(Tc4xGethPhy *gphy, bool up)
{
    KIT_PHY(gphy)->port_up = up;
}

static void kit_phy_init(Object *obj)
{
    KIT_PHY(obj)->cable = true;
}

static void kit_phy_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    Tc4xGethPhyClass *pc = TC4X_GETH_PHY_CLASS(oc);

    dc->desc = "TI DP83825I Ethernet PHY of the TC4D7 lite kit";
    dc->user_creatable = false;
    rc->phases.hold = kit_phy_reset_hold;
    pc->read22 = kit_phy_read22;
    pc->write22 = kit_phy_write22;
    pc->port_link = kit_phy_port_link;
}

/*
 * button press|release   push or let go of the user button
 * link up|down           plug or unplug the Ethernet cable
 */
static bool kit_sim_run(Tc4xSimCommands *obj, int argc, char **argv,
                        GString *out)
{
    KitTc4d7Lite *k = KIT_TC4D7_LITE(obj);

    if (!strcmp(argv[0], "button")) {
        if (argc != 2 || (strcmp(argv[1], "press") &&
                         strcmp(argv[1], "release"))) {
            g_string_append(out, "err usage: button press|release\n");
            return true;
        }
        tc4x_gpio_set_input(k->soc, KIT_BUTTON_PORT, KIT_BUTTON_PIN,
                            strcmp(argv[1], "press") != 0);
    } else {    /* link */
        if (argc != 2 || (strcmp(argv[1], "up") && strcmp(argv[1], "down"))) {
            g_string_append(out, "err usage: link up|down\n");
            return true;
        }
        k->phy->cable = !strcmp(argv[1], "up");
    }
    g_string_append(out, "ok\n");
    return true;
}

static void kit_init(Tc4xBoard *board, Tc4xSoc *soc)
{
    KitTc4d7Lite *k = KIT_TC4D7_LITE(board);

    k->soc = soc;
    tc4x_gpio_add_listener(soc, KIT_LED_PORT, kit_led_changed, k);
    tc4x_adc_set_volts(soc, KIT_POTI_KERNEL, KIT_POTI_CH, KIT_POTI_VOLTS);

    memset(k->eeprom, 0xFF, sizeof(k->eeprom));
    memcpy(&k->eeprom[KIT_EEPROM_EUI48], kit_eui48, sizeof(kit_eui48));
    tc4x_i2c_attach(soc, KIT_EEPROM_BUS, KIT_EEPROM_ADDR, &kit_eeprom_ops, k);
}

/* The PHY on GETH0 port 0 */
static void kit_wire(Tc4xBoard *board, Tc4xSoc *soc)
{
    KitTc4d7Lite *k = KIT_TC4D7_LITE(board);
    DeviceState *phy = qdev_new(TYPE_KIT_PHY);
    Object *geth = OBJECT(tc4x_soc_geth(soc));

    object_property_add_child(OBJECT(board), "phy", OBJECT(phy));
    qdev_realize_and_unref(phy, NULL, &error_fatal);
    qemu_register_resettable(OBJECT(phy));
    k->phy = KIT_PHY(phy);
    object_property_set_link(geth, "phy0", OBJECT(phy), &error_fatal);
    object_property_set_uint(geth, "phy0-addr", KIT_PHY_ADDR, &error_fatal);
}

static const Tc4xBoardInput kit_inputs[] = {
    { KIT_BUTTON_PORT, KIT_BUTTON_PIN, 1 },     /* released */
};

static const char *const kit_sim_commands[] = { "button", "link", NULL };

static void kit_class_init(ObjectClass *oc, const void *data)
{
    Tc4xBoardClass *bc = TC4X_BOARD_CLASS(oc);
    Tc4xSimCommandsClass *sc = TC4X_SIM_COMMANDS_CLASS(oc);

    bc->desc = "Infineon AURIX TC4D7 lite kit: LEDs, button, potentiometer, "
               "EEPROM, Ethernet PHY";
    bc->console = 0;
    bc->inputs = kit_inputs;
    bc->n_inputs = ARRAY_SIZE(kit_inputs);
    bc->wire = kit_wire;
    bc->init = kit_init;
    sc->commands = kit_sim_commands;
    sc->run = kit_sim_run;
}

/* -M kit-tc4d7-lite: the tc4x machine with board=kit-tc4d7-lite */
static void kit_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Tc4xMachineClass *tmc = TC4X_MACHINE_CLASS(oc);

    mc->desc = "Infineon AURIX TC4D7 lite kit (KIT_A3G_TC4D7_LITE)";
    tmc->board = KIT_NAME;
}

static const TypeInfo kit_types[] = {
    {
        .name = TYPE_KIT_PHY,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(KitPhy),
        .instance_init = kit_phy_init,
        .class_init = kit_phy_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_GETH_PHY },
            { }
        },
    }, {
        .name = TYPE_KIT_TC4D7_LITE,
        .parent = TYPE_TC4X_BOARD,
        .instance_size = sizeof(KitTc4d7Lite),
        .class_init = kit_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_TC4X_SIM_COMMANDS },
            { }
        },
    }, {
        .name = MACHINE_TYPE_NAME(KIT_NAME),
        .parent = TYPE_TC4X_MACHINE,
        .class_init = kit_machine_class_init,
    },
};

DEFINE_TYPES(kit_types)
