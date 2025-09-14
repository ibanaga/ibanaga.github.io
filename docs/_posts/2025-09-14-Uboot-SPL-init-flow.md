---
layout: post
title: "U-boot SPL initialization flow"
date: 2025-09-14
categories: u-boot
---

## Introduction

To illustrate the U-boot SPL flow, we will at how it works on a [SiFive Unleashed](https://www.qemu.org/docs/master/system/riscv/sifive_u.html) board in Qemu.

A typical boot sequence with U-boot SPL looks like this:
1. The CPU core starts executing the BootROM code in the silicon
2. BootROM code performs basic initialization of boot device (e.g. sdcard, emmc, nand flash)
3. BootROM loads U-boot SPL into internal SRAM (the System RAM, DDR, is not yet initialized)
4. U-boot SPL does basic initialization and configures the DDR controller to enable System RAM
5. U-boot SPL finds U-boot proper on storage and loads it into System RAM
6. U-boot proper starts executing from RAM
7. U-boot proper relocates itself from the initial low-DRAM address to the top of DRAM.

![U-boot SPL boot sequence](/assets/Uboot_SPL_boot_sequence.png)

This section will focus only on U-boot SPL operation.

## U-boot SPL initialization flow

| Initialization Step | Function name | Description |
| ----------- | ----------- | ----------- |
| Store the current hart id| `arch/riscv/cpu/start.S:`,<br/>`_start` | Read the `MHARTID` register |
| Set `GP` register to 0| `arch/riscv/cpu/start.S:`,<br/>`_start` | Will hold the address of Global Data structure once initialized |
| Set trap handler in `stvec` control register | `start.S: _start`,<br/>`handle_trap()` | Code to execute in case a trap happens: handle breakpoint, external and internal interrupts |
| Disable all interrupts except IPI |`arch/riscv/cpu/start.S:`,<br/>`_start` | `CONFIG_SMP` is set, so Inter Processor Interrupts are allowed |
| Set stack pointer in `SP` register | `arch/riscv/cpu/start.S:`,<br/>`_start`| SP value is defined in `.config`: `CONFIG_SPL_STACK=0x81cfe70` |
| Reserve stack space for all cores | `arch/riscv/cpu/start.S:`,<br/>`_start` | Even though it has only 5 physical cores, `CONFIG_NR_CPUS=8` is set in the .config; this has no practical impact, it would just allow more CPUs to be tracked during boot |
| Reserve space for malloc area and Global Data| `board_init_f_`<br>`alloc_reserve()`  | Space is allocated from the SP downwards, see diagram below |
| Configure proprietary settings and customized CSRs of harts | `harts_early_init()` | Does nothing in our case |
| Choose one CPU that will perform the initialization and run U-boot | `start.S:`<br/>`hart_lottery` | The rest of the cores will call `wfi` (wait for interrupt) to enter a low power state until woken up |
| Initialize Global data structure | `board_init_f_`<br/>`init_reserve()` | memset GD to 0, `arch_setup_gd()` sets up the address of GD into physical `GP` register, init `gd->malloc_base` |
| Track available harts | `arch/riscv/cpu/start.S`<br/>`wait_for_gd_init` |Set `GP` = address of Global Data for the rest of the harts (the boot hart already set it)<br/>Update the available_harts mask in the GD |
| Put secondary harts to sleep | `arch/riscv/cpu/start.S`<br/>`secondary_hart_loop` | All harts except the boot hart are put to sleep via `wfi` (wait for interrupt) |
| Initialize debug uart | `debug_uart_init()` | `drivers/serial/serial_sbi.c:` `_debug_uart_init()` |
| First stage board initialization | `board_init_f()`<br/>`spl_early_init()`<br/>`spl_common_init()` | Setup up malloc data in GD |
| Setup device tree | `fdtdec_setup()` | `CONFIG_OF_SEPARATE=y` which means the Device Tree is at the end of the image, right after uboot. End of uboot is determined by the `_end` linker symbol  |
| Device probing and initialization | `dm_autoprobe()`<br/>`dm_probe_devices()`| Scan the FDT for devices which have the `DM_FLAG_PRE_RELOC` flag set. The following devices are probed and initialized:<br/>- clint@2000000<br/>- rtcclk<br/>- serial@10010000<br/>- clock-controller@10000000<br/>- hfclk<br/>- dmc@100b0000<br/>- reset<br/>- gpio@10060000<br/>- spi@10040000<br/>-flash@0 |
| Setup CPUs | `riscv_cpu_setup()` | Enable hardware extensions based on the properties of the CPU node in DTB<br/>Setup IPI in `riscv_init_ipi()` |
| Init serial console | `preloader_console_init()` | Set baudrate, call `serial_init()`, print U-boot banner |
| Board init | `spl_board_init_f()` | Call board specific code in `board/sifive/unleashed/spl.c`:<br/>- DRAM init (which on Qemu does nothing)<br/>- reset Eth PHY |
| Relocate GD Stack in DDR (optional; in our case the it is not enabled in .config) | `spl_relocate_stack_gd()` | If `CONFIG_SPL_STACK_R=y` would be set, the stack and Global Data would be relocated from SRAM to main DDR, in order to have a larger stack size for complex things like MMC subsystem initialization. The same logic as in the first `board_init_f_init_reserve()`/` board_init_f_alloc_reserve()` is used: reserve space for core stacks, malloc area and GD then return the new address|
| Board initialization | `board_init_r()` | Init malloc area, `timer_init()` which in our case does nothing |
| Find boot device | `boot_from_devices()` | Select the boot device (SPI or MMC) according to board specific logic (e.g. jumpers) |
| Load image from boot device | `spl_load_image()`,<br/>`spl_spi_load_image()` | Load image from SPI flash:<br/>- probe flash<br/>- get offset in flash from where to load uboot proper<br/>- load the FIT image containing uboot + opensbi + DTB |
| Jump top the new image | `jumper(&spl_image)` | After the image is loaded, select proper jump functions:<br/>- jump to new location<br/>- jump to U-boot proper via OpenSBI<br/>- jump to linux | 

The memory map of the SPL looks like this:

![U-boot SPL memory map](/assets/Uboot_SPL_memory_map.png)

## U-boot SPL Device Tree

U-boot SPL does not use the full-fledged DTS for the HiFive Unleashed board, as it contains many devices that are of no interest for this early boot stage, and the amount on SRAM available is very limited.

Instead, it uses a trimmed down version of the Device Tree, only with nodes that are used during the boot stage (CPUs, clocks, DDR memory, SPI controller, Flash and EMMC for storage, etc).

In the defconfig file for the HiFive Unleashed, `CONFIG_OF_SEPARATE=y` is defined, meaning that the DTB is appended at the end of SPL.

We can easily verify this by decompiling the DTB:

- see where the SPL starts and ends by getting the address of the linker symbols `_start` and `_end`
```
$ riscv64-linux-gnu-readelf -s spl/u-boot-spl | grep -E " _start$| _end$"
  1243: 0000000008000000     0 NOTYPE  GLOBAL DEFAULT    1 _start
  1408: 00000000080134f8     0 NOTYPE  GLOBAL DEFAULT    7 _end
```
- get the size of SPL extract the compiled DTB from the SPL binary
    - 0x80134f8 - 0x8000000 = 0x134F8 -> 79096 bytes
- extract the compiled DTB from the SPL binary
    ```
    $ dd if=spl/u-boot-spl.bin of=u-boot-spl.dtb bs=79096 skip=1
    $ file u-boot-spl.dtb
    u-boot-spl.dtb: Device Tree Blob version 17, size=11299, boot CPU=0, string block size=671, DT structure block size=10572
    ```
- decompile the binary blob (DTB) into it’s text form (DTS)
    ```
    $ dtc -I dtb -O dts u-boot-spl.dtb > u-boot-spl.dts
    $ file u-boot-spl.dts
    u-boot-spl.dts: Device Tree File (v1), ASCII text, with very long lines (9263)
    ```

A more detailed analysis of the SPL's DTS file can be found [here]({% post_url 2025-09-14-Uboot-SPL-DTS-analysis %})
