# Generic HW/ASSY Version Reporting Driver

This is a generic driver that utilizes four GPIOs to read a given board's hardware
and/or assembly revision. The four gpios must be set in the board's device tree
and identified as bit[3..0]. These four bits form a binary number with 16 unique 
values. The device tree node will also have a property string list whose index
will give the appropriate 'name' for the version you wish to report. 

An example device tree entry would be:

    board_name {
        compatible = "hwassy_rev";
        gpios = <&gpio5 27 GPIO_ACTIVE_HIGH>, <&gpio6 1 GPIO_ACTIVE_HIGH>, 
                <&gpio5 18 GPIO_ACTIVE_HIGH>, <&gpio5 21 GPIO_ACTIVE_HIGH>;
        ref-bits = "addr0", "addr1", "addr2", "addr3";
        lookup-table = "Rev_1-0", Rev_1-1.2", "Rev_2.1";
    }; 

* @board_name: this is the actual dev name that will be given to this sysfs entry in the hwmon class
* @gpios: you must define four triple's as shown above. The ACTIVE_HIGH/ACTIVE_LOW is ignored
* @ref-bits: a string list as shown above that must match the order of the gpios property
* @lookup-table: a string list with zero based index reference. Empty strings can be used to 'skip' indexes


In the example above our board will register with the hwmon class in sysfs and be given a dev name of
'board_name'. Our binary number is calulated as 0b{gpio5_21}{gpio5_18}{gpio6_1}{gpio5_27}. With the 
binary number we look at the lookup-table and retrun the string whose index matches that binary number.
If the binary number does NOT have an entry (i.e. greater than the largest index) we will report 
"INVALID HW / ASSY REVISION VALUE". Lets assume gpio5_27 is HIGH and the other three are LOW; when iterogated
the device will report a HW/ASSY Revision: *Rev_1-1.2*
