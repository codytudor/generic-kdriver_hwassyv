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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "hwassyv.h"
#include <linux/string.h>
#include <linux/delay.h>

module_init();
module_exit();

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cody Tudor <cody.tudor@gmail.com>");
MODULE_DESCRIPTION("Generic HW/ASSY Revision Reporting");
MODULE_ALIAS("platform:hwassy-rev");
