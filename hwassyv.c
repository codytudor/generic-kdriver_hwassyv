/*
 * Generic HW/ASSY Version Reporting Driver
 *
 * Copyleft 2016 Tudor Design Systems, LLC.
 *
 * Author: Cody Tudor <cody.tudor@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>     
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/string.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/sysfs.h>

enum hwassyv_bits {
    BIT0 = 0,
    BIT1,
    BIT2,
    BIT3,
    MAX_BITS,
};

struct hwassyv_platform_data {
    unsigned int gpios[MAX_BITS];   // array of gpios where index = bit
    unsigned int table_index;       // 4-bit number created from gpio's
    const char *revision;           // string text holding board revision
    char name[PLATFORM_NAME_SIZE];
};

struct hwassyv_data {

    struct device *hwmon_dev;
    struct hwassyv_platform_data *pdata;
    struct device *dev;    
    int use_count;
};

const char *const bit_names[] = {
    [BIT0]   = "addr0",
    [BIT1]   = "addr1",
    [BIT2]   = "addr2",
    [BIT3]   = "addr3",

};

static ssize_t hwassyv_show_version(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct hwassyv_platform_data *data = dev_get_drvdata(dev);
    
    return sprintf(buf, "%s\n", data->revision);
}

static ssize_t hwassyv_show_index(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct hwassyv_platform_data *data = dev_get_drvdata(dev);
    
    return sprintf(buf, "lookup-table index: %d\n", data->table_index);
}

static ssize_t hwassyv_show_name(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct hwassyv_platform_data *data = dev_get_drvdata(dev);

    return sprintf(buf, "%s\n", data->name);
}

static DEVICE_ATTR(board_rev, S_IRUGO, hwassyv_show_version, NULL);
static DEVICE_ATTR(list_index, S_IRUGO, hwassyv_show_index, NULL);
static DEVICE_ATTR(name, S_IRUGO, hwassyv_show_name, NULL);

static struct of_device_id hwassyv_of_match[] = {
    { .compatible = "hwassy-rev" },
    { }
};

MODULE_DEVICE_TABLE(of, hwassyv_of_match);

static struct hwassyv_platform_data *hwassyv_parse_dt(struct platform_device *pdev)
{  
    struct device_node *node = pdev->dev.of_node;
    struct hwassyv_platform_data *pdata;
    int length;
    int index;
    unsigned int gpio_num;
    int cntr;
    int retval = 0;

    if (!node)
        return ERR_PTR(-ENODEV);
    
    length = of_property_count_strings(node, "lookup-table");

    if (length < 1) {
        dev_err(&pdev->dev, "there should be AT LEAST one revision...\n");
        return ERR_PTR(-ENODATA); 
    }
    
    length = of_property_count_strings(node, "ref-bits");
    
    if (length != 4) {
        dev_err(&pdev->dev, "four names required to identify our bits, no more, no less...\n"); 
        return ERR_PTR(-EINVAL);
    }

    length = of_count_phandle_with_args(node, "gpios", "#gpio-cells");
    
    if (length != 4) {
        dev_err(&pdev->dev, "four gpios required to make our index, no more, no less...\n"); 
        return ERR_PTR(-EINVAL);
    }
    
    pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
    if (!pdata)
        return ERR_PTR(-ENOMEM);

    memset(pdata, 0, sizeof(*pdata));
    
    for (cntr = BIT0; cntr < MAX_BITS; cntr++) {
        index = of_property_match_string(node, "ref-bits", bit_names[cntr]);
        if (index >= 0) {
            gpio_num = of_get_named_gpio_flags(node, "gpios", index, NULL);
            retval = gpio_request(gpio_num, "hwassyv");
            if (retval < 0)
                goto err;
            retval = gpio_direction_input(gpio_num);
            if (retval < 0) {
                gpio_free(gpio_num);
                goto err;
            }
            pdata->gpios[cntr] = gpio_num;           
            dev_dbg(&pdev->dev, "found %s for our hwassy version index\n", bit_names[cntr]);
            index = -ENODATA;
        }
        else {
            dev_err(&pdev->dev, "couldn't find a matching name for %s\n", bit_names[cntr]); 
            goto err;
        }
    }
        
    pdata->table_index = 0;
    cntr = BIT3;
    do {
        if (gpio_get_value(pdata->gpios[cntr]))
            pdata->table_index |= 1;
        pdata->table_index = (cntr != BIT0) ? pdata->table_index << 1 : pdata->table_index;
    } while (cntr-- > BIT0);
      
    if (pdata->table_index > 15) {
        dev_err(&pdev->dev, "something went wrong determining our table index\n"); 
        retval = -EINVAL;
        goto err;
    }
    
    retval = of_property_read_string_index(node, "lookup-table", pdata->table_index, &pdata->revision);
    
    if (retval < 0)
        pdata->revision = "INVALID HW / ASSY REVISION VALUE";

    return pdata;
    
err:

    for (cntr = BIT0; cntr < MAX_BITS; cntr++) {
        if (pdata->gpios[cntr] > 0)
            gpio_free(pdata->gpios[cntr]);   
    }
    kfree(pdata);
    return ERR_PTR(retval);
}

static int hwassyv_dt_probe(struct platform_device *pdev)
{
    struct hwassyv_data *data;
    struct hwassyv_platform_data *pdata;
    int ret;
    
    pdata = hwassyv_parse_dt(pdev);
    
    if (IS_ERR(pdata))
        return PTR_ERR(pdata);
    else if (pdata == NULL)
        pdata = pdev->dev.platform_data;

    if (!pdata) {
        dev_err(&pdev->dev, "No platform init data supplied.\n");
        return -ENODEV;
    }
  
    data = devm_kzalloc(&pdev->dev, sizeof(struct hwassyv_data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
        
    data->dev = &pdev->dev;
    data->pdata = pdata;
    strlcpy(pdata->name, dev_name(&pdev->dev), sizeof(pdata->name));
    
    platform_set_drvdata(pdev, data);

    data->hwmon_dev = hwmon_device_register(data->dev);
    if (IS_ERR(data->hwmon_dev)) {
        dev_err(data->dev, "failed to register hw/assy version reporting driver\n\n");
        return PTR_ERR(data->hwmon_dev);
    }
    
    dev_set_drvdata(data->hwmon_dev, pdata);
    
    ret = device_create_file(data->hwmon_dev, &dev_attr_name);
    if (ret) {
        dev_err(data->dev, "unable to create dev_attr_name sysfs file\n");
        goto err_free_mem;
    }
    
    ret = device_create_file(data->hwmon_dev, &dev_attr_board_rev);
    if (ret) {
        dev_err(data->dev, "unable to create dev_attr_board_rev sysfs file\n");
        goto unregister_name;
    }
    
    ret = device_create_file(data->hwmon_dev, &dev_attr_list_index);
    if (ret) {
        dev_err(data->dev, "unable to create dev_attr_list_index sysfs file\n");
        goto unregister_board_rev;
    }

    dev_info(&pdev->dev, "HW/ASSY driver successfully probed.\n");

    return 0;
    
unregister_board_rev:
    device_remove_file(data->hwmon_dev, &dev_attr_board_rev);
    
unregister_name:
    device_remove_file(data->hwmon_dev, &dev_attr_name);
    
err_free_mem:
    hwmon_device_unregister(data->hwmon_dev);
    kfree(data);
    kfree(pdata);
    return ret;
    
}

static int hwassyv_remove(struct platform_device *pdev)
{
    struct hwassyv_data *data = platform_get_drvdata(pdev);

    device_remove_file(data->hwmon_dev, &dev_attr_name);
    device_remove_file(data->hwmon_dev, &dev_attr_board_rev);
    device_remove_file(data->hwmon_dev, &dev_attr_list_index);
    hwmon_device_unregister(data->hwmon_dev);
    dev_set_drvdata(data->hwmon_dev, NULL);
    platform_set_drvdata(pdev, NULL);

    return 0;
}

static struct platform_driver hwassyv_driver = {
    .driver     = {
        .name       = "hwassy-rev",
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
