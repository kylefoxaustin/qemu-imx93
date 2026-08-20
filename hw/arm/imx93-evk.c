/*
 * NXP i.MX 93 11x11 Evaluation Kit (LPDDR4X) - QEMU machine
 *
 * Modeled on hw/arm/imx8mp-evk.c (Bernhard Beschow) and the i.MX 95 port.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Instantiates the FSL_IMX93 SoC, attaches LPDDR4X, wires the per-FlexCAN
 * can-bus links, and boots a kernel (or firmware) on the stock
 * imx93-11x11-evk device tree with no DT modification.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/boot.h"
#include "hw/arm/fsl-imx93.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/kvm.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "net/can_emu.h"

#define TYPE_IMX93_EVK_MACHINE MACHINE_TYPE_NAME("imx93-11x11-evk")
OBJECT_DECLARE_SIMPLE_TYPE(Imx93EvkMachineState, IMX93_EVK_MACHINE)

struct Imx93EvkMachineState {
    MachineState parent_obj;

    /* Optional CAN buses, attached via -machine canbus0=...,canbus1=... */
    CanBusState *canbus[FSL_IMX93_NUM_FLEXCAN];
};

/*
 * Inject device-tree nodes for the virtio-mmio transports the SoC instantiates
 * (see fsl-imx93.c). The kernel's CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES is off, so
 * it only binds these via DT. The stock DTB root carries interrupt-parent =
 * <&gic> and #address-cells/#size-cells = <2>, so a root-level node inherits
 * the GIC and uses 2-cell addresses. interrupts = <SPI N LEVEL_HIGH>.
 */
static void imx93_evk_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    for (int i = FSL_IMX93_NUM_VIRTIO_MMIO - 1; i >= 0; i--) {
        hwaddr base = FSL_IMX93_VIRTIO_MMIO_BASE +
                      i * FSL_IMX93_VIRTIO_MMIO_SIZE;
        int irq = FSL_IMX93_VIRTIO_MMIO_IRQ + i;
        g_autofree char *node = g_strdup_printf("/virtio_mmio@%" PRIx64, base);

        qemu_fdt_add_subnode(fdt, node);
        qemu_fdt_setprop_string(fdt, node, "compatible", "virtio,mmio");
        qemu_fdt_setprop_cells(fdt, node, "reg",
                               0, base, 0, FSL_IMX93_VIRTIO_MMIO_SIZE);
        /* GIC_FDT_IRQ_TYPE_SPI = 0, IRQ_TYPE_LEVEL_HIGH = 4 */
        qemu_fdt_setprop_cells(fdt, node, "interrupts", 0, irq, 4);
        qemu_fdt_setprop(fdt, node, "dma-coherent", NULL, 0);
    }
}

static void imx93_evk_init(MachineState *machine)
{
    Imx93EvkMachineState *m = IMX93_EVK_MACHINE(machine);
    static struct arm_boot_info boot_info;
    FslImx93State *s;
    int i;

    /*
     * The SoC instantiates a Cortex-M33 real-time core, which is M-profile and
     * has no KVM support, so the whole machine is TCG-only. Reject -accel kvm
     * up front with a clear message rather than aborting later in M33 realize.
     */
    if (kvm_enabled()) {
        error_report("The imx93-11x11-evk machine requires TCG: it emulates a "
                     "Cortex-M33 real-time core that KVM cannot run");
        exit(1);
    }

    if (machine->ram_size > FSL_IMX93_RAM_SIZE_MAX) {
        error_report("RAM size " RAM_ADDR_FMT
                     " above max supported (0x%" PRIx64 ")",
                     machine->ram_size, (uint64_t)FSL_IMX93_RAM_SIZE_MAX);
        exit(1);
    }

    boot_info = (struct arm_boot_info) {
        .loader_start = FSL_IMX93_RAM_START,
        .board_id     = -1,
        .ram_size     = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .modify_dtb   = imx93_evk_modify_dtb,
    };

    s = FSL_IMX93(object_new(TYPE_FSL_IMX93));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(s));

    /* Forward any user-attached CAN buses to the SoC's FlexCAN controllers. */
    for (i = 0; i < FSL_IMX93_NUM_FLEXCAN; i++) {
        if (m->canbus[i]) {
            g_autofree char *name = g_strdup_printf("canbus%d", i);
            object_property_set_link(OBJECT(s), name, OBJECT(m->canbus[i]),
                                     &error_abort);
        }
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);

    memory_region_add_subregion(get_system_memory(), FSL_IMX93_RAM_START,
                                machine->ram);

    /* Attach an SD/MMC card to any uSDHC fed by a -drive if=sd,index=N. */
    for (i = 0; i < FSL_IMX93_NUM_USDHCS; i++) {
        DriveInfo *di = drive_get(IF_SD, i, 0);
        BlockBackend *blk;
        DeviceState *carddev;
        BusState *bus;

        if (!di) {
            continue;
        }
        blk = blk_by_legacy_dinfo(di);
        bus = qdev_get_child_bus(DEVICE(&s->usdhc[i]), "sd-bus");
        carddev = qdev_new(TYPE_SD_CARD);
        qdev_prop_set_drive_err(carddev, "drive", blk, &error_fatal);
        qdev_realize_and_unref(carddev, bus, &error_fatal);
    }

    if (!qtest_enabled()) {
        arm_load_kernel(&s->cpu[0], machine, &boot_info);
    }
}

static const char *imx93_evk_get_default_cpu_type(const MachineState *ms)
{
    /* TCG-only machine (M33 core has no KVM); the A55 cluster is Cortex-A55. */
    return ARM_CPU_TYPE_NAME("cortex-a55");
}

static void imx93_11x11_evk_machine_init(MachineClass *mc)
{
    /*
     * The A55 cluster is a fixed Cortex-A55; reject any other -cpu with a
     * clean error rather than piping it into the A55/GIC/EL3 wiring - e.g.
     * "-cpu cortex-m33" would otherwise abort on the missing cntfrq property,
     * and a wrong A-core would silently build the wrong silicon.
     */
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a55"),
        NULL
    };

    mc->desc                  = "NXP i.MX 93 11x11 EVK (LPDDR4X)";
    mc->init                  = imx93_evk_init;
    mc->valid_cpu_types       = valid_cpu_types;
    /*
     * The A55 cluster plus the always-present Cortex-M33 real-time core. TCG
     * sizes its per-CPU context table from smp.max_cpus, so both the default
     * and the max must count the M33 or the M33's tcg_register_thread()
     * asserts. The SoC fixes the A55 cluster size regardless of -smp.
     */
    mc->default_cpus          = FSL_IMX93_NUM_A55_CPUS + FSL_IMX93_NUM_M33;
    mc->max_cpus              = FSL_IMX93_NUM_A55_CPUS + FSL_IMX93_NUM_M33;
    mc->default_ram_id        = "imx93-11x11-evk.ram";
    mc->default_ram_size      = 2 * GiB;   /* 11x11 EVK: 2 GiB LPDDR4X */
    mc->get_default_cpu_type  = imx93_evk_get_default_cpu_type;
}

static void imx93_evk_machine_instance_init(Object *obj)
{
    int i;

    /*
     * Per-FlexCAN CAN-bus links, settable from the command line, e.g.
     *   -object can-bus,id=canbus0 -machine canbus0=canbus0
     * The machine forwards each to the matching SoC FlexCAN controller.
     */
    for (i = 0; i < FSL_IMX93_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("canbus%d", i);
        object_property_add_link(obj, name, TYPE_CAN_BUS,
                                 (Object **)&IMX93_EVK_MACHINE(obj)->canbus[i],
                                 object_property_allow_set_link, 0);
    }
}

static void imx93_11x11_evk_class_init(ObjectClass *oc, const void *data)
{
    imx93_11x11_evk_machine_init(MACHINE_CLASS(oc));
}

static const TypeInfo imx93_11x11_evk_machine_types[] = {
    {
        .name          = TYPE_IMX93_EVK_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(Imx93EvkMachineState),
        .instance_init = imx93_evk_machine_instance_init,
        .class_init    = imx93_11x11_evk_class_init,
        .interfaces    = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(imx93_11x11_evk_machine_types)
