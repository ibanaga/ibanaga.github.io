---
layout: post
title: "U-boot initialization flow"
date: 2025-09-13
categories: u-boot
---

## Introduction

The post will explore the initialization flow of U-boot, specifically the last stage of the U-boot flow, also known as *U-boot proper*.

We will see the initialization flow on a Qemu RiscV Virt machine, when U-boot starts running from RAM in supervisor mode.
The execute-in-place (XIP) use case, where U-boot executes directly from NOR flash will be described in a separate post.

First, a quick recap on a typical boot sequence:
1. The CPU core starts executing the BootROM code in the silicon
2. BootROM code performs basic initialization of boot device (e.g. sdcard, emmc, nand flash)
3. BootROM loads U-boot SPL into internal SRAM (the System RAM, DDR, is not yet initialized)
4. U-boot SPL does basic initialization and configures the DDR controller to enable System RAM
5. U-boot SPL finds U-boot proper on storage and loads it into System RAM
6. U-boot proper starts executing from RAM
7. U-boot proper relocates itself from the initial low-DRAM address to the top of DRAM.

![U-boot SPL boot sequence](/assets/Uboot_SPL_boot_sequence.png)

## Loading U-boot into RAM

U-boot proper is loaded from storage at a fixed address in RAM, defined at build time in `CONFIG_TEXT_BASE`. Uboot will start running from RAM at this address: function addresses, pointers, variable accesses, etc, are all assuming this start address.

For RiscV, DRAM is mapped at `0x8000_0000` in the address space.
The default Qemu RiscV config (`qemu-riscv64_smode_defconfig`) defines `CONFIG_TEXT_BASE` at address `0x8020_0000`, so at 2MB from the start of DRAM.

```console
$ cat .config | grep CONFIG_TEXT_BASE
CONFIG_TEXT_BASE=0x80200000
```

We will see later why putting the code at this offset makes sense for U-boot. Other bootloaders, like the BBL used in the real SiFive Unleashed target is placed right at the beginning of DRAM at `0x8000_0000`.

We can confirm this address by reading the `_start` symbol from the elf binary.
```console
$ readelf -s u-boot | grep " _start$"
   85: 0000000080200000     0 NOTYPE  GLOBAL DEFAULT    1 _start
```

But there’s a catch: the memory at the beginning of RAM where U-boot is copied will also be used by the Linux kernel.

To facilitate this, U-boot will relocate itself at the top of the DRAM early in the boot process. This will leave all low-DRAM memory available for the kernel, thus preventing any clashes or wasted memory.

Let’s see some other options related to memory in `qemu-riscv64_smode_defconfig`:
```
CONFIG_SYS_MALLOC_LEN=0x800000
CONFIG_SYS_MALLOC_F_LEN=0x4000
CONFIG_STACK_SIZE_SHIFT=14
```

These options specify the stack and heap sizes that U-boot can use. The “_F_” in the malloc options stands for *“first stage“*, as in *before relocation*.
- stack size = `1 << CONFIG_STACK_SIZE_SHIFT` which gives us 16KB of stack
- malloc area before relocation: 16KB
- malloc area after relocation: 8MB

## Execution flow

### Initialization before relocation
Now, let’s see how U-boot starts executing from the very first instruction at address `0x8020_0000`.
The previous boot stage (e.g. U-boot SPL) passes two arguments to the U-boot proper via registers `a0` and `a1`: the hartid (core id) and a pointer to the DTB.

| Step          | Function name | Description |
| -----------   | -----------   | ----------- |
| Save args passed by SPL | `start.S: _start`   | Save hartid and pointer to DTB |
| Set `GP` register = 0 | `start.S: _start` | `GP` register is used to hold the Global Data which is not yet initialized
| Set trap handler in `stvec` control register | `start.S: _start`,<br/>`handle_trap()` | Code to execute in case a trap happens: handle breakpoint, external and internal interrupts |
| Set stack pointer to TEXT_BASE | `start.S`: call_board_init_f | SP is `0x8020_0000`, same as Uboot load address, because stack grows downwards |
| Reserve memory for stack, heap and Global Data | `board_init_f`<br/>`_alloc_reserve()` | CONFIG_TEXT_BASE - stack_size - CONFIG_SYS_MALLOC_F_LEN - roundup(sizeof(gd), 16) |
| Initialize space reserved in previous step | `board_init_f`<br/>`_init_reserve()`| Load pointer to Global Data space in `GP` register and <br/>set `gd->malloc_base` |
| Choose one CPU that will perform the initialization and run U-boot | `start.S: hart_lottery` | The rest of the cores will call `wfi` (wait for interrupt) to enter a low power state until woken up |
| Initialize debug uart | `debug_uart`<br/>`_init()` | `drivers/serial/serial_sbi.c: _debug_uart_init()` |
| Call board initialization “first stage” code | `initcall_run_f()` | This will call a long list of init functions without any arguments (see separate table below) |

The *“first stage“* part of the board initialization logic is responsible for basic setup in order for U-boot to relocate itself to the top of the DRAM, thus freeing the path for the Linux kernel.

All the logic is in the `board_init_f()` -> `initcall_run_f()` functions. After setting up the environment for relocation, it will perform the actual copy of data (`relocate_code()` in `start.S`), then jump into the new location and call `board_init_r()` to continue the second part of the board initialization.
- board_init_f() - *_f()* as in *first stage*
- board_init_r() - *_r()* as in *after relocation*

The memory map of U-boot before relocation looks like this:

![U-boot SPL boot sequence](/assets/Uboot_memory_map_before_relocation.png)

Now it is clear why U-boot is not compiled with the load address at the beginning of the DRAM: it needs some spare area below it for internal tasks before relocating.

Depending on the setup, the beginning of the RAM can be occupied by other firmware like OpenSBI which runs before U-boot proper.

Let’s now move on and see what `initcall_run_f()` is doing:

| Initialization Step | Function name | Description |
| ----------- | ----------- | ----------- |
| Get uboot length | `setup_mon_len()` | Compute the total size of uboot in memory (text + rodata + data + bss). <br/>It needs this info to know how much memory to reserve in the top of DRAM for relocation<br/>`gd->mon_len = (ulong)(__bss_end) - (ulong)(_start)` |
|Set up device tree (FTD)|`fdtdec_setup()`,<br/>`fdtdec_board_setup()`| Find a valid FDT, either at the end of uboot or passed in by the previous boot stage |
|Init malloc area|`initf_malloc()`|Init malloc info in the GD; `gd->malloc_ptr` gives the used bytes (currently 0)|
|Initialize basic devices|`initf_dm()`|Calls `device_probe()` on devices that should be initialized before relocation (`DM_FLAG_PRE_RELOC` flag set): serial port, riscv_timer, cpus|
|Call board specific early init code|`board_early_init_f()`| Not used on the Qemu virt implementation|
|Initialize environment|`env_init()`|Load environment from storage, if available, else use the default environment|
|Start serial device|`serial_init()`|Call `start()` on the current serial device|
|Init first part of the console|`console_init_f()`||
|DRAM initialization|`dram_init()`|DRAM initialization is already done by the SPL, just update the ram_size and ram_base in the Global Data from the FDT’s `/memory` node|
|Setup destination address for relocation|`setup_dest_addr()`|Update ram_top and relocaddr in Global Data based on arch and board specifications. <br/>For Qemu virt machine, `ram_top` is set to `0x8000_0000 + amount of RAM`|
|Reserve needed area at the top of DRAM|`reserve_video()`<br/>`reserve_trace()`<br/>`reserve_uboot()`<br/>`reserve_malloc()`<br/>`reserve_board()`<br/>`reserve_global_data()`<br/>`reserve_fdt()`| See memory map below |
|Relocate FDT|`reloc_fdt()`|`memcpy()` the FDT to the new location|
|Setup relocation data|`setup_reloc()`|Calculate reloc_off and store it in Global Data<br/>`memcpy()` the GD to the new location|
|Actual relocation|`jump_to_copy()`,<br/>`relocate_code()`|This function performs the actual relocation of U-boot and jumps into it, it never returns.|
|Prepare to call the second part of the board initialization code|`start.S`: `call_board_init_r`||
|Continue initialization after relocation|`board_init_r()`,<br/>`initcall_run_r()`|Second part of board initialization, now running after relocation:<br/>- set address of Global Data in hardware GP register<br/>- call `initcall_run_r()`|

The memory map of uboot after relocation (`qemu-riscv64_smode_defconfig`):

![U-boot SPL boot sequence](/assets/Uboot_memory_map_after_relocation.png)

### Initialization after relocation

Second part of board initialization, `initcall_run_r()`, after relocating to top of DRAM:

| Initialization Step | Function name | Description |
| ----------- | ----------- | ----------- |
| Tell others: relocation done| `initr_reloc()` | `gd->flags |= GD_FLG_RELOC | GD_FLG_FULL_MALLOC_INIT;` |
| Init CPU caches | `initr_caches()` | |
| Init global data after relocation (e.g. environment), init malloc area | | |
| Init drivers | `initr_dm()` | |
| Call board specific code | `board_init()` | |
| Init early devices | `initr_dm_devices()` | Init timers and multiplexer devices |
| Initialize serial devices | `serial_initialize()` | Probe all serial devices defined in the DTB: `uclass_probe_all(UCLASS_SERIAL);` |
| Initialize storage | `initr_mmc()` | |
| Initialize console | `console_init_r()` | Set default input/output handlers (`serial_getc()`, `serial_putc()`...)<br/>Check if environment overwrites them |
| Enable interrupts | `interrupt_init()` | Arch specific code to enable interrupts |
| Final board specific initialization | `board_late_init()` | |
| Run main loop | `run_main_loop()` | Start receiving data from the user via the prompt and CLI |