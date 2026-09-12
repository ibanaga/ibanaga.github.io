---
layout: post
title: "U-Boot sandbox build"
date: 2026-09-12
categories: u-boot sandbox
---

# Introduction

U-Boot sandbox is a version of U-Boot that runs as a normal program on the host machine, without needing a development board. It is a regular application linked against the host C library. It starts from a regular `main()` function provided by the sandbox glue code in `arch/sandbox` and `board/sandbox`.

Sandbox provides custom DTS files with emulated devices that allow testing of high-level code without relying on hardware.

Official sandbox documentation can be found [here](https://docs.u-boot.org/en/v2023.10/arch/sandbox/sandbox.html).

In this post we'll see how the sandbox works internally.

A very high level diagram of the sandbox's layers and components:

![U-Boot sandbox layers](/assets/uboot_sandbox/uboot_sandbox_layers.drawio.svg)

# Quick steps to try it
```shell
$ make sandbox_defconfig
$ make -j $(nproc)
$ ./u-boot


U-Boot 2026.10-rc3-00083-gc46bd5b21361

DRAM:  256 MiB
Core:  30 devices, 15 uclasses, devicetree: board, universal payload active
NAND:  0 MiB
MMC:
Loading Environment from nowhere... OK
Warning: device tree node '/config/environment' not found
In:    serial,cros-ec-keyb,usbkbd
Out:   serial,vidconsole
Err:   serial,vidconsole
Net:         eth_initialize() No ethernet found.

Hit any key to stop autoboot: 0
=>
```

U-Boot is now running on my host PC instead of a development board!
Before we see how this happens internally, let's take a look at higher level details...

# Device tree sources

OK, we saw that we can run U-boot on our host machine...but what can we do with it?  
U-Boot inherently works with real hardware.

U-Boot sandbox provides device-tree sources under `arch/sandbox/dts/`:
 - `sandbox.dts` - the default sandbox device tree, selected when `-D` is used
 - `sandbox64.dts` - 64-bit sandbox version (used when `CONFIG_SANDBOX64=y`)
 - `test.dts` - a larger device tree with more special-purposed emulated devices used in testing, selected when by `-T`

Shared include files are also provided such as `sandbox.dtsi` or `sandbox_pmic.dtsi`.

Running `./u-boot` with no arguments uses the device tree embedded in the u-boot executable which contains no emulated devices.
```
=> fdt addr -c
Control fdt: 09bbbc90
=> fdt addr 09bbbc90
Working FDT set to 9bbbc90
=> fdt print /
/ {
	binman {
	};
};
=> 
```

Running with the test device tree will provide a large number of DTS nodes.

```
$ ./u-boot -T

....

=> fdt addr -c
Control fdt: 08baef50
=> fdt addr 08baef50
Working FDT set to 8baef50
=> fdt print /
/ {
	model = "sandbox";
	compatible = "sandbox";
	#address-cells = <0x00000001>;
	#size-cells = <0x00000001>;
    ...
=> 
```

# Emulated RAM

U-Boot needs RAM to run. In the sandbox, that RAM is simply a region of the host’s memory. For testing, its contents can be loaded from a file at startup to reproduce an existing memory state, then saved back to disk on exit so that any changes persist across runs (when `-m` argument is passed).

RAM size is set in the defconfig via `SANDBOX_RAM_SIZE_MB`.

The emulated RAM driver is `drivers/ram/sandbox_ram.c`; it is very lightweight since it has nothing to initialize.

On a real target, U-Boot can use physical memory addresses to access devices or memory.
In sandbox, this is not possible: accessing a physical address (e.g. 0x10000) will crash the application.

To cope with this situation, U-Boot core exposes the functions `map_sysmem()` / `unmap_sysmem()` which can be used when dealing with physical addresses inside the code.

For (most?!) real platforms, these functions are no-ops, they just return back the physical address received as input.
But in a sandbox environment, they will convert a physical address to a pointer inside the malloc'ed host memory (`CONFIG_ARCH_MAP_SYSMEM` offers this support).



![U-Boot map_sysmem](/assets/uboot_sandbox/uboot_sandbox_map_sysmem.drawio.png)

For our sandbox target the flow is: `map_sysmem()` -> `map_physmem()` -> `phys_to_virt()`
```
void *phys_to_virt(phys_addr_t paddr) {
    ...
    /* If the address is within emulated DRAM, calculate the value */
    if (paddr < gd->ram_size)
        return (void *)(gd->arch.ram_buf + paddr);
	...
}
```

# The entry point

The `u-boot` application's  `main()` function is defined in `arch/sandbox/cpu/os.c` which just calls `sandbox_main()` defined in `arch/sandbox/cpu/start.c`

Then we have the following flow in `sandbox_main()`:
   - set up memory to emulate target RAM: allocate memory with `malloc()`; `CONFIG_SANDBOX_RAM_SIZE_MB` determines the size
   - if the user specified a file for the RAM content in the command line arguments, that file is read in the newly allocated memory area
   - sets up signal handlers
   - calls `board_init_f()` for first stage board initialization
   - calls `board_init_r` for second stage initialization which never returns

# Emulated devices

## Emulated i2c

Emulating I2C devices is particularly interesting since it involves several layers:
 - the emulated I2C bus - since we don't have physical wires
 - the I2C device drivers, which can be:
	- real driver code + emulated device: tests the production driver code against an emulated device
	- sandbox driver code + emulated device: tests the upper layer code using the driver

### Real I2C setup

![U-boot real I2C](/assets/uboot_sandbox/uboot_sandbox_i2c.drawio.svg)

### Emulated I2C setup

The normal I2C path is retained while the controller and attached devices are implemented by the sandbox emulation.

![U-boot emulated I2C](/assets/uboot_sandbox/uboot_sandbox_i2c_emulated.drawio.svg)

U-Boot's sandbox `test.dts` defines the following I2C devices inside the emulated I2C controller of type `sandbox,i2c`:

| Device | Address | Compatible string | Emulated node | Emulated compatible string |
|--------|---------|------------|----------------------|----------------------------|
| eeprom | 0x2C    | i2c-eeprom | emul_eeprom  | sandbox,i2c-eeprom |
| rtc    | 0x43    | sandbox-rtc | emul0  | sandbox,i2c-rtc-emul |
| rtc    | 0x61    | sandbox-rtc | emul1  | sandbox,i2c-rtc-emul |
| pmic   | 0x40    | sandbox,pmic | emul_pmic0  | sandbox,i2c-pmic |
| pmic   | 0x41    | fsl,mc34708 | emul_pmic1  | sandbox,i2c-pmic |
| pmbus  | 0x70    | pmbus | emul_pmbus  | sandbox,i2c-pmbus |

The Compatible string selects the user facing driver, while the Emulated compatible string selects the sandbox emulator.

The test DTS keeps the emulator implementations under a virtual I2C device named `emul@7f`.  
This node uses the `sandbox,i2c-emul-parent` compatible and groups all emulated devices.
The `0x7f` address belongs to this internal parent node; it is not the address of a physical device being emulated.
Each I2C device node refers to its emulator through the `sandbox,emul` phandle.


### EEPROM (Electrically Erasable Programmable Read-Only Memory)

An EEPROM is a small non-volatile memory device commonly connected over I2C. From U-Boot's point of view, it looks like a byte array: it writes data to an address and reads it back later. The sandbox emulates this device, so we can exercise the regular EEPROM driver without connecting a real chip.

The basic protocol used by an EEPROM is:

 - a **write transaction**: `START condition + (device address + write bit) + address bytes + data bytes to write + STOP condition`
     - EEPROM devices write internally in pages; a single write transaction cannot cross a page boundary
	 - typical page sizes can be 16, 32 or 64 bytes
 - a **read transaction** is composed of a sequence of 2 messages:
   - a dummy write to set the address: `START condition + (device address + write bit) + address bytes`
   - a repeated START: `START condition + (device address + read bit), read byte1, read byte2, ..., + STOP condition`
   - the EEPROM increments its internal address pointer after each byte, allowing sequential reads.

The U-Boot EEPROM driver is located in `drivers/misc/i2c_eeprom.c`
It exposes a standard set of operations supported by the EEPROM.  
 - `i2c_eeprom_std_probe()`: check if the chip is usage
 - `i2c_eeprom_std_read()`: read bytes from a given address
 - `i2c_eeprom_std_write()`: write bytes to a given address
 - `i2c_eeprom_std_size()`: return the size of the EEPROM

Let's look at the probe operation: `i2c_eeprom_std_probe()`:  this will call `i2c_eeprom_read()` to read 1 byte from the chip. If the read succeeds, the chip is functional.

![U-Boot emulated EEPROM read flow](/assets/uboot_sandbox/uboot_sandbox_i2c_eeprom_read.drawio.svg)

A write operation follows the same flow, and the emulator updates its simulated memory.

### RTC (Real-Time Clock)

An I2C RTC behaves like a small register device. The registers store seconds, minutes, hours, day, month, year.  
Users can read or write one or multiple registers (e.g. read just the year register or write only the day and month registers).  
For this emulated chip, the read and write transactions are similar to the previously discussed EEPROM.

The sandbox environment provides a generic sandbox RTC driver (`drivers/rtc/sandbox_rtc.c`) instead of a real driver used in production.  
This allows testing higher-level code without emulating hardware specific details.  
Calling `date` on the command line will read the time from the sandbox-RTC device.


![U-Boot emulated RTC read flow](/assets/uboot_sandbox/uboot_sandbox_i2c_rtc_read.drawio.svg)


## MMC (Multimedia Card)

The sandbox environment provides 3 emulated MMC devices.
```
=> mmc list
mmc2: 2 (SD)
mmc1: 1 (SD)
mmc0: 0 (SD)
```

The `mmc1` device can be backed by a file on disk named `mmc1.img`.  
The file can be created via: `qemu-img create -f raw mmc1.img 32M` and can then be available in U-boot.
```
=> mmc dev 1
switch to partitions #0, OK
mmc1 is current device
=> mmc info
Device: mmc1
Manufacturer ID: 0
OEM: 0
Name: Bus Speed: 1000000
Mode: MMC legacy
Rd Block Len: 512
SD version 3.0
High Capacity: Yes
Capacity: 32 MiB
Bus Width: 1-bit
Erase Group Size: 512 Bytes
```

The driver that handles these devices is `drivers/mmc/sandbox_mmc.c`.
It exposes generic operations of the MMC device:
 - `sandbox_mmc_probe()` to probe the device (either maps a file on disk or allocates heap memory)
 - `sandbox_mmc_send_cmd()` to send a command to the MMC device (fills the response buffer with test data)
 - `sandbox_mmc_set_ios()` to set I/O settings (no-op in the emulated device)
 - `sandbox_mmc_get_cd()` to return the card detect status (always present in this case)


![U-Boot emulated MMC read flow](/assets/uboot_sandbox/uboot_sandbox_mmc_read.drawio.svg)

## Serial

Sandbox provides an emulated Serial driver (`compatible = "sandbox,serial"`) that reads input from `stdin` and writes output to `stdout`.

The driver is under `drivers/serial/sandbox.c` and exposes common operations for a Serial device:
 - `sandbox_serial_putc()`: writes to `stdout`
 - `sandbox_serial_pending()`: read data from `stdin` and places it in the driver's input buffer
 - `sandbox_serial_getc()`: read one byte from the driver's input buffer

![U-Boot emulated Serial flow](/assets/uboot_sandbox/uboot_sandbox_serial_read.drawio.svg)


## Ethernet

Sandbox exposes several emulated network devices (`compatible = "sandbox,eth"`) in its `test.dts`; in this section we'll look at the first one: `eth@10002000`.

The driver is located at `drivers/net/sandbox.c` and defines the standard Ethernet operations:
 - `sb_eth_start()`: initialize the emulated device
 - `sb_eth_send`: send a packet
 - `sb_eth_recv`: receive a packet
 - `sb_eth_free_pkt`: release a received packet buffer after the network stack has processed it
 - `sb_eth_stop`: stop the transmission and reception until the next start

If we try to ping an IP address in U-Boot we will receive a success message:
```
=> ping 192.168.100.1
Using eth@10002000 device
host 192.168.100.1 is alive
```
The ping request is handled by the emulated device which injects a synthetic reply packet to be processed by the networking loop.

Let's look at the flow:

![U-Boot emulated ETH flow](/assets/uboot_sandbox/uboot_sandbox_eth_ping.drawio.svg)
