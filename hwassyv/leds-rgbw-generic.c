/*
 * RGB+W LED Generic Device Driver
 *
 * Copyright 2015 Tudor Design Systems, LLC.
 *
 * Author: Cody Tudor <cody.tudor@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * 
 * This driver is a re-implementation of the generic backlight class
 * device in ./drivers/video/backlight/backlight.c and pwm_bl.c; functions 
 * and general flow were used as a skeleton for the management and 
 * control operations. 
 * 
 * This driver requires a device tree entry with the compatible
 * string "pwm-rgbw" The driver will create a class device in /sys/class
 * that will provide the same functionality as a the backlight class
 * device. The DT entry must contain a PWM for red, grn, & blue channels in 
 * addition to a GPIO for the wht channel. Default brightness for each
 * is optional but brightness levels MUST be defined.  
 *
 */

#include "rgbw.h"

/* soft_pwm_device
 *
 * This structure maintains the information regarding a
 * single PWM signal for software PWM colors 
*/
struct soft_pwm_device {
    unsigned int gpio;          // gpio number
    int value;                  // current GPIO pin value (0 or 1 only)
    struct hrtimer pwm_timer;   // hrtimer struct for each soft pwm
};

/* pwm_rgbw_data
 *
 * This structure maintains the driver information for all colors
 * whether soft_pwm or hard_pwm.
*/
struct pwm_rgbw_data {
    struct pwm_device       *pwm[MAX_COLORS];       // array holding four possible pointers to four possible pwm_devices in [R,G,B,W] format
    struct soft_pwm_device  soft_pwm[MAX_COLORS];   // array holding four possible possible soft_pwm_devices in [R,G,B,W] format
    struct device           *dev;                   // parent dev
    enum rgbw_type          types[MAX_COLORS];      // array stating whether each color is soft_pwm OR hard_pwm in [R,G,B,W] format
    unsigned int            period;                 // period of PWM in ns
    unsigned int            lth_brightness;         // time period of smallest pwm pulse_width in ns
    unsigned int            *levels;                // array of values
    int                     (*notify)(struct device *, int brightness);
    void                    (*notify_after)(struct device *, int brightness);
    void                    (*exit)(struct device *);
};

struct platform_rgbw_data {
    unsigned int max_brightness;
    unsigned int lth_brightness;
    unsigned int pwm_period_ns;
    unsigned int *levels;
    int (*init)(struct device *dev);
    int (*notify)(struct device *dev, int brightness);
    void (*notify_after)(struct device *dev, int brightness);
    void (*exit)(struct device *dev);
};

const char *const color_names[] = {
    [COLOR_RED]     = "red",
    [COLOR_GREEN]   = "green",
    [COLOR_BLUE]    = "blue",
    [COLOR_WHITE]   = "white",

};

/* Values to set our PWM channel every 50ms. These values are calculated 
 * using the Excel VB script:
 * =TRUNC((EXP(SIN(B1 * 3.1415926 / 2)) - (1 / EXP(1)))*(255 / (EXP(1) - (1 / EXP(1)))))
 * which is an adaptation of the natural equation:
 * [e^sin(x * pi/2) - 1/e] * [255/(e - 1/e)] with x in seconds
 * 
 * This "breathing" function gives us set of values between 0 - 255 that
 * are cycled through at a fixed interval. In our case we set the
 * interval to be 50ms. The pi/2 expression lengthens our period which can
 * be decreased or increased by decreasing or increasing this multiplier
 * respectively. Since the Linux kernel cannot perform floating point
 * math we simply create a lookup table instead. For natural feel you
 * must include one FULL period - 1 value i.e. from zero to the value 
 * before the next zero. 
 */
const unsigned int pulse_val_table[] = {
    0, 1, 2, 3, 4, 6, 8, 10, 13, 16, 20,
    24, 28, 34, 39, 45, 52, 60, 68, 77, 
    86, 97, 107, 119, 130, 143, 155, 167,
    180, 192, 203, 214, 224, 233, 240, 246, 
    251, 254, 254, 254, 251, 246, 240, 233,
    224, 214, 203, 192, 180, 167, 155, 143,
    130, 119, 107, 97, 86, 77, 68, 60, 52,
    45, 39, 34, 28, 24, 20, 16, 13, 10, 8,
    6, 4, 3, 2, 1, 0, 0, 0, 0
};

struct rgbw_device *g_rgbw_dev;

static int pulse_color_update(struct rgbw_device *rgbw_dev, const int pcolor)
{
    struct pwm_rgbw_data *pb = rgbw_get_data(rgbw_dev);
    int brightness;
    int max;
    int duty_cycle;
    
    if (pcolor >= MAX_COLORS)
        return -EINVAL;
        
    brightness = rgbw_dev->props[pcolor].brightness;
    max = rgbw_dev->props[pcolor].max_brightness;
        
    if (pb->notify)
        brightness = pb->notify(pb->dev, brightness);
    
    if (pb->types[pcolor] == RGBW_PWM) {
        if (brightness == 0) {
            pwm_config(pb->pwm[pcolor], 0, pb->period);
            pwm_disable(pb->pwm[pcolor]);
        } else {
            duty_cycle = (pb->levels) ? pb->levels[brightness] : brightness;

            duty_cycle = pb->lth_brightness +
                 (duty_cycle * (pb->period - pb->lth_brightness) / max);
            pwm_config(pb->pwm[pcolor], duty_cycle, pb->period);
            pwm_enable(pb->pwm[pcolor]);
        }
    }
    
    if ((pb->types[pcolor] == RGBW_GPIO) && (!hrtimer_active(&pb->soft_pwm[pcolor].pwm_timer)))
            hrtimer_start(&pb->soft_pwm[pcolor].pwm_timer, ktime_set(0,1000), HRTIMER_MODE_REL);

    if (pb->notify_after)
        pb->notify_after(pb->dev, brightness);

    return 0;
}

static int rgbw_color_update(struct rgbw_device *rgbw_dev)
{
    struct pwm_rgbw_data *pb = rgbw_get_data(rgbw_dev);
    int brightness[MAX_COLORS];
    int max[MAX_COLORS];
    int duty_cycle;
    int cntr;
    
    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
        brightness[cntr] = rgbw_dev->props[cntr].brightness;
        max[cntr] = rgbw_dev->props[cntr].max_brightness;
    }
        
    if (pb->notify) {
        for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
            brightness[cntr] = pb->notify(pb->dev, brightness[cntr]);
        }
    }
    
    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
        if (pb->types[cntr] == RGBW_PWM) {
            if (brightness[cntr] == 0) {
                pwm_config(pb->pwm[cntr], 0, pb->period);
                pwm_disable(pb->pwm[cntr]);
            } else {
                duty_cycle = (pb->levels) ? pb->levels[brightness[cntr]] : brightness[cntr];

                duty_cycle = pb->lth_brightness +
                     (duty_cycle * (pb->period - pb->lth_brightness) / max[cntr]);
                pwm_config(pb->pwm[cntr], duty_cycle, pb->period);
                pwm_enable(pb->pwm[cntr]);
            }
        }
        if ((pb->types[cntr] == RGBW_GPIO) && (!hrtimer_active(&pb->soft_pwm[cntr].pwm_timer)))
            hrtimer_start(&pb->soft_pwm[cntr].pwm_timer, ktime_set(0,1000), HRTIMER_MODE_REL);
    }

    if (pb->notify_after) {
        for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
            pb->notify_after(pb->dev, brightness[cntr]);
        }
    }

    return 0;
}

static const struct rgbw_ops pwm_color_ops = {
    .update_status  = rgbw_color_update,
};

/* The rainbow timer callback is called only when the rainbow function
 * is enabled. We should have already saved the state of the RGBW prior
 * to starting so that we can restore it once the rainbow function is
 * stopped. 
 */
 
static enum hrtimer_restart rgbw_rb_hrtimer_callback(struct hrtimer *timer)
{
    int bstate; 
    
    bstate = g_rgbw_dev->acts.state;
    
    if (bstate & RGBW_RB_ON) { 
        bstate = g_rgbw_dev->acts.bstate;                      
        switch (bstate) {
            case 0: /*  Red 255, Green increasing */
                g_rgbw_dev->props[COLOR_GREEN].brightness++;
                if (g_rgbw_dev->props[COLOR_GREEN].brightness > (g_rgbw_dev->props[COLOR_GREEN].max_brightness - 1))
                    g_rgbw_dev->acts.bstate = 1;
                break;
            case 1: /*  Green 255, Red decreasing */
                g_rgbw_dev->props[COLOR_RED].brightness--;
                if (g_rgbw_dev->props[COLOR_RED].brightness < 1)
                    g_rgbw_dev->acts.bstate = 2;
                break;
            case 2: /*  Green 255, Blue increasing */
                g_rgbw_dev->props[COLOR_BLUE].brightness++;
                if (g_rgbw_dev->props[COLOR_BLUE].brightness > (g_rgbw_dev->props[COLOR_BLUE].max_brightness - 1))
                    g_rgbw_dev->acts.bstate = 3;
                break;
            case 3: /*  Blue 255, Green decreasing */
                g_rgbw_dev->props[COLOR_GREEN].brightness--;
                if (g_rgbw_dev->props[COLOR_GREEN].brightness < 1)
                    g_rgbw_dev->acts.bstate = 4;
                break;
            case 4: /*  Blue 255, Red increasing */
                g_rgbw_dev->props[COLOR_RED].brightness++;
                if (g_rgbw_dev->props[COLOR_RED].brightness > (g_rgbw_dev->props[COLOR_RED].max_brightness - 1))
                    g_rgbw_dev->acts.bstate = 5;
                break;
            case 5: /*  Red 255, Blue decreasing */
                g_rgbw_dev->props[COLOR_BLUE].brightness--;
                if (g_rgbw_dev->props[COLOR_BLUE].brightness < 1)
                    g_rgbw_dev->acts.bstate = 0;
                break;
            default:
                g_rgbw_dev->acts.bstate = 0;
                g_rgbw_dev->props[COLOR_RED].brightness = g_rgbw_dev->props[COLOR_RED].max_brightness;
                g_rgbw_dev->props[COLOR_GREEN].brightness = 0;
                g_rgbw_dev->props[COLOR_BLUE].brightness = 0;
                g_rgbw_dev->props[COLOR_WHITE].brightness = 0;
                break;
        };
      
        rgbw_color_update(g_rgbw_dev);
   
        hrtimer_forward(timer, ktime_get(), ns_to_ktime(PULSE_VALUE_PER_NS));
        
        return HRTIMER_RESTART;
    }
    else 
        return HRTIMER_NORESTART;
}

/* The heartbeat timer callback is called only when the heartbeat function
 * is enabled. We should have already saved the state of the RGBW prior
 * to starting so that we can restore it once the heartbeat function is
 * stopped. 
 */
 
static enum hrtimer_restart rgbw_hb_hrtimer_callback(struct hrtimer *timer)
{
    int bstate;
    int cntr;
    
    bstate = g_rgbw_dev->acts.state;
    
    if (bstate & RGBW_HB_ON) {               
        bstate = g_rgbw_dev->acts.bstate;
        
        for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
            g_rgbw_dev->props[cntr].brightness = (bstate % 2) ? 0 : g_rgbw_dev->acts.rgbw_values[cntr];          
        }
        
        g_rgbw_dev->acts.bstate = (bstate < 3) ? g_rgbw_dev->acts.bstate+1 : 0;
        
        rgbw_color_update(g_rgbw_dev);
        
        if (bstate < 3)
            hrtimer_forward(timer, ktime_get(), ns_to_ktime(100000000));
        else 
            hrtimer_forward(timer, ktime_get(), ns_to_ktime(700000000));  
        
        return HRTIMER_RESTART;
    }
    else 
        return HRTIMER_NORESTART;
}

/* The blink timer callback is called only when the blink function
 * is enabled. We should have already saved the state of the RGBW prior
 * to starting so that we can restore it once the blink function is
 * stopped. 
 */
 
static enum hrtimer_restart rgbw_blink_hrtimer_callback(struct hrtimer *timer)
{
    int bstate;
    int cntr;
    
    bstate = g_rgbw_dev->acts.state;
    
    if (bstate & RGBW_BLINK_ON) {               
        bstate = g_rgbw_dev->acts.bstate;                      
        
        for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
            g_rgbw_dev->props[cntr].brightness = (!bstate) ? 0 : g_rgbw_dev->acts.rgbw_values[cntr];   
        }
        
        g_rgbw_dev->acts.bstate = (!bstate) ? 1 : 0;
        
        rgbw_color_update(g_rgbw_dev);

        hrtimer_forward(timer, ktime_get(), ns_to_ktime(BLINK_STATE_PER_NS));
        
        return HRTIMER_RESTART;
    }
    else
        return HRTIMER_NORESTART;
}

/* The pulse timer callback is called only when the pulse function
 * is enabled. We should have already saved the state of the RGBW prior
 * to starting so that we can restore it once the pulse function is
 * stopped. 
 */
 
static enum hrtimer_restart rgbw_pulse_hrtimer_callback(struct hrtimer *timer)
{
    int pulse_val_size = ARRAY_SIZE(pulse_val_table);
    int bstate; 
    int pcolor;
    
    bstate = g_rgbw_dev->acts.state;
    
    if (bstate & RGBW_PULSE_ON) { 
        pcolor = g_rgbw_dev->acts.pcolor;           
        
        if (g_rgbw_dev->props[pcolor].cntr >= pulse_val_size)
            g_rgbw_dev->props[pcolor].cntr = 0;        
        g_rgbw_dev->props[pcolor].brightness = pulse_val_table[g_rgbw_dev->props[pcolor].cntr];
        g_rgbw_dev->props[pcolor].cntr++;
        
        pulse_color_update(g_rgbw_dev, pcolor);

        hrtimer_forward(timer, ktime_get(), ns_to_ktime(PULSE_VALUE_PER_NS));
        
        return HRTIMER_RESTART;
    }
    else
        return HRTIMER_NORESTART;
}

/* The gpio timer callback is called only when needed (which is to
 * say, at the earliest PWM signal toggling time) in order to
 * maintain the pressure on system latency as low as possible
 */
 
static enum hrtimer_restart rgbw_gpio_hrtimer_callback(struct hrtimer *timer)
{
    int cntr;
    enum hrtimer_restart ret;
    u64 next_toggle; // a nanosecond value
    struct pwm_rgbw_data *pb;
    ktime_t hrtimer_next_tick;
    
    pb = rgbw_get_data(g_rgbw_dev);
    hrtimer_next_tick = ktime_set(0,0);
    
    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
        if (pb->types[cntr] == RGBW_GPIO) {            
            if (hrtimer_callback_running(&pb->soft_pwm[cntr].pwm_timer)) {
                if (g_rgbw_dev->props[cntr].brightness >= g_rgbw_dev->props[cntr].max_brightness) {
                    pb->soft_pwm[cntr].value = 1;
                }
                else if (g_rgbw_dev->props[cntr].brightness == 0) {
                    pb->soft_pwm[cntr].value = 0;
                }
                else {
                    pb->soft_pwm[cntr].value = 1 - pb->soft_pwm[cntr].value;
                    
                    /* next_toggle is the pulse width in nsec at this point */
                    next_toggle = g_rgbw_dev->props[cntr].brightness * pb->lth_brightness;
                    
                    /* gpio is HIGH then we switch again at pulse width otherwise 
                     * we will toggle again at the beginning of the next period */
                    next_toggle = (pb->soft_pwm[cntr].value) ? next_toggle : (pb->period - next_toggle); 
                    hrtimer_next_tick = ns_to_ktime(next_toggle);
                }
                __gpio_set_value(pb->soft_pwm[cntr].gpio, pb->soft_pwm[cntr].value); 
                break;               
            }                  
        }
    }
    
    if (ktime_compare(hrtimer_next_tick, ktime_set(0,0)) > 0)  {
        hrtimer_forward(timer, ktime_get(), hrtimer_next_tick);
        ret = HRTIMER_RESTART;
    }
    else {
        dev_dbg(&g_rgbw_dev->dev, "Stopping the GPIO timer.\n");
        ret = HRTIMER_NORESTART;
    }
       
    return ret;   
}

static int rgbw_dt_validation(struct platform_device *pdev)
{
    int ret;
    int cntr;
    unsigned int num_def_names = 0, num_def_colors = 0;
    
    /* 
     * Perform our DT sanity check here-
     * Check for node property fields: "gpio-names" & "pwm-names"
     * If either exist verify the number of names matches the number
     * of defined colors (three minimum) and AT LEAST these three exist:
     * 1. "red"
     * 2. "green"
     * 3. "blue"
     * between the two property fields. The value:
     * 4. "white"
     * is an optional color but MUST have an associated pwm OR gpio if
     * it is defined.
     */
    cntr = of_count_phandle_with_args(pdev->dev.of_node, "pwms", "#pwm-cells");
    
    if (cntr > 0)
        num_def_colors += cntr;

    cntr = of_count_phandle_with_args(pdev->dev.of_node, "gpios", "#gpio-cells");
    
    if (cntr > 0) 
        num_def_colors += cntr;
      
    if (num_def_colors < 3) {
        dev_err(&pdev->dev, "not enough colors defined with pwm and gpio\n"); 
        return -ENODATA;
    }
    
    if (num_def_colors > 4) {
        dev_err(&pdev->dev, "too many colors defined with pwm and gpio\n"); 
        return -EINVAL;
    }
    
    cntr = of_property_count_strings(pdev->dev.of_node, "pwm-names");
    
    if (cntr > 0)
        num_def_names += cntr;
        
    cntr = of_property_count_strings(pdev->dev.of_node, "gpio-names");
    
    if (cntr > 0)
        num_def_names += cntr;
       
    if (num_def_names > 0) {
        if (num_def_names != num_def_colors) {
            if (num_def_names > num_def_colors)
                dev_err(&pdev->dev, "too many names defined: names=%d pwms=%d\n", num_def_names, num_def_colors);
            else
                dev_err(&pdev->dev, "not enough names defined: names=%d pwms=%d\n", num_def_names, num_def_colors);
            
            ret = -ENODATA;
            return ret;
        }
        for (cntr = COLOR_RED; cntr < num_def_names; cntr++) {
            ret = of_property_match_string(pdev->dev.of_node, "pwm-names", color_names[cntr]);
            if (ret >= 0);
                continue;
            ret = of_property_match_string(pdev->dev.of_node, "gpio-names", color_names[cntr]);
            if (ret >= 0);
                continue;
                
            dev_err(&pdev->dev, "could not find the name for color %s\n", color_names[cntr]);
            return ret;
        }
    }
    else { /* No names defined so we will add them in R-G-B-W order from pwms to gpios */
        return 0;
    }
    
    return num_def_colors;    
}

static int rgbw_parse_dt(struct device *dev,
                  struct platform_rgbw_data *data)
{
    struct device_node *node = dev->of_node;
    struct property *prop;
    int length;
    int ret;

    if (!node)
        return -ENODEV;

    memset(data, 0, sizeof(*data));

    /* determine the number of brightness levels */
    prop = of_find_property(node, "brightness-levels", &length);
    if (!prop)
        return -EINVAL;

    data->max_brightness = length / sizeof(u32);

    /* read brightness levels from DT property */
    if (data->max_brightness > 0) {
        size_t size = sizeof(*data->levels) * data->max_brightness;

        data->levels = devm_kzalloc(dev, size, GFP_KERNEL);
        if (!data->levels)
            return -ENOMEM;

        ret = of_property_read_u32_array(node, "brightness-levels",
                         data->levels,
                         data->max_brightness);
        if (ret < 0)
            return ret;       
            
        data->max_brightness--;
    }

    return 0;
}

static struct of_device_id rgbw_of_match[] = {
    { .compatible = "pwm-rgbw" },
    { }
};

MODULE_DEVICE_TABLE(of, rgbw_of_match);


/* This function is called by the system when a DT entry
 * has a platform_device with matching compatible string.
 * We can expect a single DT entry with one, multiple, or
 * none pwms defined; similarly we can have one, multiple
 * or none gpios defined. It is up to the user to properly 
 * give the pwm-names and/or gpio-names list in the DT to 
 * identify the REAL color for which the soft/hard_pwm 
 * controls. Only the valid names: "red", "green", "blue",
 * & "white" will allow this driver to correctly map. Otherwise,
 * invalid names will be given to the devices themselves
 * and r-g-b-w control will be in the following order: 
 * pwms first --> gpios second. 
 * */
 
static int rgbw_dt_probe(struct platform_device *pdev)
{
    struct platform_rgbw_data *data = pdev->dev.platform_data;
    struct platform_rgbw_data defdata;
    struct rgbw_properties props[MAX_COLORS];
    struct rgbw_actions acts;
    struct rgbw_device *rgbw_dev;
    struct pwm_rgbw_data *pb;
    unsigned int max, num_named_colors, gpio_api_num;
    int ret;
    unsigned int cntr = 0;
    int index = -ENODATA;

    if (!data) {
        ret = rgbw_parse_dt(&pdev->dev, &defdata);
        if (ret < 0) {
            dev_err(&pdev->dev, "failed to find platform data\n");
            return ret;
        }

        data = &defdata;
    }
    
    /* DT sanity check before we proceed */
    ret = rgbw_dt_validation(pdev);
    /* negative value is an ERROR */
    if (ret < 0) 
        return ret;
    else
        num_named_colors = ret;
    /* 
     * positive value greater than 0 is number of colors WITH 
     * names defined; zero means three OR four colors defined
     * but are not named in the DT which means we will add them to 
     * the device in R-G-B[-W] order starting with pwms then gpios if 
     * any. 
     */

    if (data->init) {
        ret = data->init(&pdev->dev);
        if (ret < 0)
            return ret;
    }

    pb = devm_kzalloc(&pdev->dev, sizeof(*pb), GFP_KERNEL);
    if (!pb) {
        dev_err(&pdev->dev, "no memory for state\n");
        ret = -ENOMEM;
        goto err_alloc;
    }

    if (data->levels) {
        max = data->levels[data->max_brightness];
        pb->levels = data->levels;
    } else
        max = data->max_brightness;

    pb->notify = data->notify;
    pb->notify_after = data->notify_after;
    pb->exit = data->exit;
    pb->dev = &pdev->dev;
    
    memset(props, 0, sizeof(struct rgbw_properties) * MAX_COLORS);
    
    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
        props[cntr].max_brightness = data->max_brightness;
    }
    
    if (num_named_colors > 0) {
        /* This conditional adds the colors based on names between the 
         * hw pwms and gpios. Since we already did a sanity check we loop 
         * the number of times for the given number of names in the DT.
         */
        for (cntr = COLOR_RED; cntr < num_named_colors; cntr++) {
            pb->types[cntr] = RGBW_TYPE_INVALID;
            props[cntr].type = RGBW_TYPE_INVALID;
            index = of_property_match_string(pdev->dev.of_node, "pwm-names", color_names[cntr]);
            if (index >= 0) {
                pb->pwm[cntr] = devm_of_pwm_get(&pdev->dev, pdev->dev.of_node, color_names[cntr]);
                //printk(KERN_INFO "pwms of property index for %s: %d\n", color_names[cntr], index);
                if (IS_ERR(pb->pwm[cntr])) {
                    dev_err(&pdev->dev, "unable to request PWM for color %s using devm_of_pwm_get\n", color_names[cntr]);
                    pb->pwm[cntr] = devm_pwm_get(&pdev->dev, color_names[cntr]);
                    if (IS_ERR(pb->pwm[cntr])) {
                        dev_err(&pdev->dev, "unable to request PWM for color %s\n", color_names[cntr]);
                        goto err_alloc;
                    }
                }
                dev_dbg(&pdev->dev, "got pwm for color %s\n", color_names[cntr]);
                pb->types[cntr] = RGBW_PWM;
                props[cntr].type = RGBW_PWM;
                index = -ENODATA; 
                continue;
            }
            
            index = of_property_match_string(pdev->dev.of_node, "gpio-names", color_names[cntr]);
            if (index >= 0) {
                gpio_api_num = of_get_named_gpio_flags(pdev->dev.of_node, "gpios", index, NULL);
                //printk(KERN_INFO "gpio number for %s: %d\n", color_names[cntr], gpio_api_num);
                ret = gpio_request(gpio_api_num, "rgbw-drv");
                if (ret < 0)
                    goto err_alloc;
                ret = gpio_direction_output(gpio_api_num, 0);
                if (ret < 0) {
                    gpio_free(gpio_api_num);
                    goto err_alloc;
                }
                pb->soft_pwm[cntr].gpio = gpio_api_num;
                pb->soft_pwm[cntr].value = 0;
                dev_dbg(&pdev->dev, "created soft pwm for color %s\n", color_names[cntr]);
                pb->types[cntr] = RGBW_GPIO;
                props[cntr].type = RGBW_GPIO;
                index = -ENODATA;
            }
            
        }
    
    }
    else {
        /* This conditional adds the colors in R-G-B-W order between the 
         * hw pwms and gpios. Since we already did a sanity check we loop 
         * the number of times for the given number of defined hw pwms
         * and gpios in the DT.
         */
        int num_hpwms = 0, num_spwms = 0, total_pwms;
        num_hpwms = of_count_phandle_with_args(pdev->dev.of_node, "pwms", "#pwm-cells");
        num_spwms = of_count_phandle_with_args(pdev->dev.of_node, "gpios", "#gpio-cells");
        total_pwms = num_hpwms + num_spwms;
        cntr = COLOR_RED;
        if (num_hpwms > 0) {
            for (; cntr < num_hpwms; cntr++) {
                pb->types[cntr] = RGBW_TYPE_INVALID;
                props[cntr].type = RGBW_TYPE_INVALID;
                pb->pwm[cntr] = devm_of_pwm_get(&pdev->dev, pdev->dev.of_node, color_names[cntr]);
                if (IS_ERR(pb->pwm[cntr])) {
                    dev_err(&pdev->dev, "unable to request PWM for color %s using devm_of_pwm_get\n", color_names[cntr]);
                    pb->pwm[cntr] = devm_pwm_get(&pdev->dev, color_names[cntr]);
                    if (IS_ERR(pb->pwm[cntr])) {
                        dev_err(&pdev->dev, "unable to request PWM for color %s\n", color_names[cntr]);
                        goto err_alloc;
                    }
                }
                dev_dbg(&pdev->dev, "got pwm for color %s\n", color_names[cntr]);
                pb->types[cntr] = RGBW_PWM;
                props[cntr].type = RGBW_PWM;
            }
        }
        
        if (num_spwms > 0) {
            for (; cntr < total_pwms; cntr++) {
                pb->types[cntr] = RGBW_TYPE_INVALID;
                props[cntr].type = RGBW_TYPE_INVALID;
                gpio_api_num = of_get_named_gpio_flags(pdev->dev.of_node, "gpios", cntr - num_hpwms, NULL);
                //printk(KERN_INFO "gpio number for %s: %d\n", color_names[cntr], gpio_api_num);
                ret = gpio_request(gpio_api_num, "rgbw-drv");
                if (ret < 0)
                    goto err_alloc;
                ret = gpio_direction_output(gpio_api_num, 0);
                if (ret < 0) {
                    gpio_free(gpio_api_num);
                    goto err_alloc;
                }
                pb->soft_pwm[cntr].gpio = gpio_api_num;
                pb->soft_pwm[cntr].value = 0;           
                dev_dbg(&pdev->dev, "created soft pwm for color %s\n", color_names[cntr]);
                pb->types[cntr] = RGBW_GPIO;
                props[cntr].type = RGBW_GPIO;
            }
        }
        
        if (cntr != total_pwms) {
            dev_err(&pdev->dev, "something went wrong when allocating our pwms\n");
            goto err_alloc;
        }
        
    }

    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
        if (pb->types[cntr] == RGBW_PWM) {
            /* 
             * Set our period for the gpios based on the 
             * first identified PWM period. We will then set the 
             * period of the other PWMs to match this period
             * in the event they were configured wrong.
             */
            pb->period = pwm_get_period(pb->pwm[cntr]);
            break;
        }
        else {
            /* 
             * If there are no configured PWMs then we will
             * set our period arbitrarily to be 7.65ms
             * which is a frequency of ~130Hz
             */
            pb->period = 7650000;
        }
    }
    
    pb->lth_brightness = (pb->period / max);
    
    acts.pcolor = INVALID_COLOR;
    acts.bstate = INVALID_COLOR;
    acts.state = 0;

    rgbw_dev = rgbw_device_register(dev_name(&pdev->dev), &pdev->dev, pb,
                       &pwm_color_ops, props, &acts);
    if (IS_ERR(rgbw_dev)) {
        dev_err(&pdev->dev, "failed to register rgbw channel\n");
        ret = PTR_ERR(rgbw_dev);
        goto err_alloc;
    }

    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {        
        rgbw_dev->props[cntr].brightness = 0;
        rgbw_dev->acts.rgbw_values[cntr] = 0;
        rgbw_dev->props[cntr].cntr = 0;
    }
    
    for (cntr = HRTIMER_PULSE; cntr < MAX_HRTIMER; cntr++) {
        hrtimer_init(&rgbw_dev->rgbw_hrtimer[cntr], CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    }
    
    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
        if (pb->types[cntr] == RGBW_GPIO) {
            hrtimer_init(&pb->soft_pwm[cntr].pwm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
            pb->soft_pwm[cntr].pwm_timer.function = &rgbw_gpio_hrtimer_callback;
        }
    }
    
    rgbw_dev->rgbw_hrtimer[HRTIMER_PULSE].function = &rgbw_pulse_hrtimer_callback;
    rgbw_dev->rgbw_hrtimer[HRTIMER_BLINK].function = &rgbw_blink_hrtimer_callback;
    rgbw_dev->rgbw_hrtimer[HRTIMER_HEARTBEAT].function = &rgbw_hb_hrtimer_callback;
    rgbw_dev->rgbw_hrtimer[HRTIMER_RAINBOW].function = &rgbw_rb_hrtimer_callback;
    
    rgbw_update_status(rgbw_dev);

    platform_set_drvdata(pdev, rgbw_dev);
    g_rgbw_dev = rgbw_dev;
        
    return 0;

err_alloc:
    if (data->exit)
        data->exit(&pdev->dev);
    return ret;
}

static int rgbw_color_remove(struct platform_device *pdev)
{
    struct rgbw_device *rgbw_dev = platform_get_drvdata(pdev);
    struct pwm_rgbw_data *pb = rgbw_get_data(rgbw_dev);
    int cntr;
    
    rgbw_dev->acts.pcolor = INVALID_COLOR;
    rgbw_dev->acts.bstate = INVALID_COLOR;
    for (cntr = HRTIMER_PULSE; cntr < MAX_HRTIMER; cntr++) {
        dev_err(&pdev->dev, "cancelling our hrtimers\n");
        hrtimer_cancel(&rgbw_dev->rgbw_hrtimer[cntr]);
    }
    rgbw_device_unregister(rgbw_dev);
    for (cntr = COLOR_RED; cntr < MAX_COLORS; cntr++) {
        if (pb->types[cntr] == RGBW_PWM) {
            pwm_config(pb->pwm[cntr], 0, pb->period);
            pwm_disable(pb->pwm[cntr]);
        }
        if (pb->types[cntr] == RGBW_GPIO) {
            __gpio_set_value(pb->soft_pwm[cntr].gpio, 0);
            gpio_free(pb->soft_pwm[cntr].gpio);
        }
    }
    if (pb->exit)
        pb->exit(&pdev->dev);
    return 0;
}

static struct platform_driver pwm_rgbw_driver = {
    .driver     = {
        .name       = "rgbw-drv",
        .owner      = THIS_MODULE,
        .of_match_table = of_match_ptr(rgbw_of_match),
    },
    .probe      = rgbw_dt_probe,
    .remove     = rgbw_color_remove,
};

module_platform_driver(pwm_rgbw_driver);
