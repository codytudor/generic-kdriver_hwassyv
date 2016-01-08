/*
 * Generic HW/ASSY Version Reporting Driver
 *
 * Copyright 2015 Tudor Design Systems, LLC.
 *
 * Author: Cody Tudor <cody.tudor@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "hwassyv.h"
#include <linux/string.h>
#include <linux/delay.h>

static struct of_device_id hwassyv_of_match[] = {
    { .compatible = "hwassy-rev" },
    { }
};

static int hwassyv_dt_probe(struct platform_device *pdev)
{
    return 0;
}

static int hwassyv_remove(struct platform_device *pdev)
{
    return 0;
}

MODULE_DEVICE_TABLE(of, hwassyv_of_match);

static struct platform_driver hwassyv_driver = {
    .driver     = {
        .name       = "hwassy-vreport",
        .owner      = THIS_MODULE,
        .of_match_table = of_match_ptr(hwassyv_of_match),
    },
    .probe      = hwassyv_dt_probe,
    .remove     = hwassyv_remove,
};

module_platform_driver(hwassyv_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cody Tudor <cody.tudor@gmail.com>");
MODULE_DESCRIPTION("Generic HW/ASSY Revision Reporting");
MODULE_ALIAS("platform:hwassy-rev");
