/*
 * AST2400 SoC
 *
 * Andrew Jeffery <andrew@aj.id.au>
 * Jeremy Kerr <jk@ozlabs.org>
 *
 * Copyright 2016 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "hw/arm/ast2400.h"
#include "hw/char/serial.h"
#include "hw/boards.h"
#include "hw/i2c/aspeed_i2c.h"

#define AST2400_UART_5_BASE      0x00184000
#define AST2400_IOMEM_SIZE       0x00200000
#define AST2400_IOMEM_BASE       0x1E600000
#define CDNS_GEM_ADDR            0x1E680000
#define AST2400_VIC_BASE         0x1E6C0000
#define AST2400_SCU_BASE         0x1E6E2000
#define AST2400_TIMER_BASE       0x1E782000
#define AST2400_I2C_BASE         0x1E78A000

static const int uart_irqs[] = { 9, 32, 33, 34, 10 };
static const int timer_irqs[] = { 16, 17, 18, 35, 36, 37, 38, 39, };

#define CDNS_GEM_IRQ 3

/*
 * IO handlers: simply catch any reads/writes to IO addresses that aren't
 * handled by a device mapping.
 */

static uint64_t ast2400_io_read(void *p, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx " [%u]\n",
                  __func__, offset, size);
    return 0;
}

static void ast2400_io_write(void *opaque, hwaddr offset, uint64_t value,
                unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx " <- 0x%" PRIx64 " [%u]\n",
                  __func__, offset, value, size);
}

static const MemoryRegionOps ast2400_io_ops = {
    .read = ast2400_io_read,
    .write = ast2400_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ast2400_init(Object *obj)
{
    AST2400State *s = AST2400(obj);

    s->cpu = cpu_arm_init("arm926");

    object_initialize(&s->vic, sizeof(s->vic), TYPE_ASPEED_VIC);
    object_property_add_child(obj, "vic", OBJECT(&s->vic), NULL);
    qdev_set_parent_bus(DEVICE(&s->vic), sysbus_get_default());

    object_initialize(&s->gem, sizeof(s->gem), TYPE_CADENCE_GEM);
    qdev_set_parent_bus(DEVICE(&s->gem), sysbus_get_default());

    object_initialize(&s->timerctrl, sizeof(s->timerctrl), TYPE_ASPEED_TIMER);
    object_property_add_child(obj, "timerctrl", OBJECT(&s->timerctrl), NULL);
    qdev_set_parent_bus(DEVICE(&s->timerctrl), sysbus_get_default());

    object_initialize(&s->scu, sizeof(s->scu), TYPE_ASPEED_SCU);
    object_property_add_child(obj, "scu", OBJECT(&s->scu), NULL);
    qdev_set_parent_bus(DEVICE(&s->scu), sysbus_get_default());

    object_initialize(&s->i2c, sizeof(s->i2c), TYPE_ASPEED_I2C);
    object_property_add_child(obj, "i2c", OBJECT(&s->i2c), NULL);
    qdev_set_parent_bus(DEVICE(&s->i2c), sysbus_get_default());
}

static void ast2400_realize(DeviceState *dev, Error **errp)
{
    int i;
    AST2400State *s = AST2400(dev);
    Error *err = NULL;

    /* SRAM */
    memory_region_allocate_system_memory(&s->sram, NULL, "ast2400.sram",
                                         AST2400_SRAM_SIZE);
    memory_region_add_subregion(get_system_memory(), AST2400_SRAM_BASE,
                                &s->sram);

    /* IO space */
    memory_region_init_io(&s->iomem, NULL, &ast2400_io_ops, NULL,
            "ast2400.io", AST2400_IOMEM_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), AST2400_IOMEM_BASE,
            &s->iomem, -1);

    /* VIC */
    object_property_set_int(OBJECT(&s->vic),
			    0x00001F07FFF8FFFFULL, "sense", &err);
    object_property_set_int(OBJECT(&s->vic),
			    0x000000F800070000ULL, "dual_edge", &err);
    object_property_set_int(OBJECT(&s->vic),
			    0x00005F07FFF8FFFFULL, "event", &err);
    if (err) {
	    error_propagate(errp, err);
	    return;
    }
    object_property_set_bool(OBJECT(&s->vic), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->vic), 0, AST2400_VIC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->vic), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->vic), 1,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));

    /* Timer */
    object_property_set_bool(OBJECT(&s->timerctrl), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->timerctrl), 0, AST2400_TIMER_BASE);
    for (i = 0; i < ARRAY_SIZE(timer_irqs); i++) {
        qemu_irq irq = qdev_get_gpio_in(DEVICE(&s->vic), timer_irqs[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->timerctrl), i, irq);
    }

    /* FIXME: Add existing ethernet controller - Cadence GEM */
    qemu_check_nic_model(&nd_table[0], TYPE_CADENCE_GEM);
    qdev_set_nic_properties(DEVICE(&s->gem), &nd_table[0]);
    object_property_set_bool(OBJECT(&s->gem), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gem), 0, CDNS_GEM_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gem), 0,
                       qdev_get_gpio_in(DEVICE(&s->vic), CDNS_GEM_IRQ));

    /* SCU */
    object_property_set_int(OBJECT(&s->scu), 0x19FC3E8BU, "scu0c", &err);
    object_property_set_int(OBJECT(&s->scu), 0x01000000U, "scu88", &err);
    object_property_set_int(OBJECT(&s->scu), 0x000000FFU, "scu8c", &err);
    object_property_set_int(OBJECT(&s->scu), 0x003FFFF3U, "scu9c", &err);
    object_property_set_bool(OBJECT(&s->scu), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->scu), 0, AST2400_SCU_BASE);

    /* UART - attach an 8250 to the IO space as our UART5 */
    if (serial_hds[0]) {
        qemu_irq uart5 = qdev_get_gpio_in(DEVICE(&s->vic), uart_irqs[4]);
        serial_mm_init(&s->iomem, AST2400_UART_5_BASE, 2,
                       uart5, 38400, serial_hds[0], DEVICE_LITTLE_ENDIAN);
    }

    /* I2C */
    object_property_set_bool(OBJECT(&s->i2c), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c), 0, AST2400_I2C_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c), 0,
                       qdev_get_gpio_in(DEVICE(&s->vic), 12));

    /* add a TMP423 temperature sensor */
    dev = i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&s->i2c), 2),
                           "tmp423", 0x4c);
    object_property_set_int(OBJECT(dev), 31000, "temperature0", &err);
    object_property_set_int(OBJECT(dev), 28000, "temperature1", &err);
    object_property_set_int(OBJECT(dev), 20000, "temperature2", &err);
    object_property_set_int(OBJECT(dev), 110000, "temperature3", &err);

    /* The palmetto platform expects a ds3231 RTC but a ds1338 is
     * enough to provide basic RTC features. Alarms will be missing */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&s->i2c), 0), "ds1338", 0x68);
}

static void ast2400_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ast2400_realize;

    /*
     * Reason: creates an ARM CPU, thus use after free(), see
     * arm_cpu_class_init()
     */
    dc->cannot_destroy_with_object_finalize_yet = true;
}

static const TypeInfo ast2400_type_info = {
    .name = TYPE_AST2400,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AST2400State),
    .instance_init = ast2400_init,
    .class_init = ast2400_class_init,
};

static void ast2400_register_types(void)
{
    type_register_static(&ast2400_type_info);
}

type_init(ast2400_register_types)
