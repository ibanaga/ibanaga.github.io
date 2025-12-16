---
layout: post
title: "Flacon mode on the Sifive Unleashed in Qemu"
date: 2025-12-16
categories: u-boot SPL falcon
---

## Introduction

U-boot Falcon mode allows starting the Linux kernel directly from the U-boot SPL, bypassing U-boot proper entirely.

The flexibility offered by the full bootloader is lost, but the system bootup time is reduced. This is useful in production projects where an interactive bootloader is not necessary, but system boot-up time is crucial.

## Falcon mode in RiscV

In RiscV, the kernel needs a single parameter to boot: the address of the DTB.
The Linux kernel will get all its needed configuration options from the DTB (including command line arguments, the location of the initrd, etc).

As a particularity of RiscV, Linux needs OpenSBI to be initialized before starting to run the kernel.

The SPL must:
- load the OpenSBI binary first
- load Linux, DTB, initrd
- jump into OpenSBI which will jump into the Linux kernel after initializing hardware

So to boot Linux directly from the SPL a FIT image containing at least 4 components in needed:
- OpenSBI
- the Linux kernel
- DTB with all the needed arguments by the kernel (cannot be modified at runtime)
- initrd

When writing this (end of 2025), Falcon mode has been disabled for platforms which have not all the hooks in place ([commit 66873b9](https://github.com/u-boot/u-boot/commit/66873b9ef0b447d9b0f9da81eb9867e1f19b99cf)). This includes also RiscV, which is missing some functions to support Falcon mode out of the box. We will need to locally revert this commit and add the missing hooks.

## Falcon mode on HiFive Unleashed in Qemu

We start from the default defconfig file:
```
make ARCH=riscv sifive_unleashed_defconfig
```

To enable Falcon mode on RiscV, several config options must be enabled:
- `CONFIG_SPL_OS_BOOT`: activates Falcon mode
- `CONFIG_SPL_LOAD_FIT_OPENSBI_OS_BOOT`: Enable SPL to load Linux and OpenSBI from FIT image


Selecting `CONFIG_SPL_LOAD_FIT_OPENSBI_OS_BOOT` will modify the ITS source file for uboot (`arch/riscv/dts/binman.dtsi` ) to produce not a `u-boot.itb` but a `linux.itb` FIT image instead.

It will also need a kernel binary to pack into the FIT image, so we need either to copy the `Image` file into the uboot source code folder or edit `arch/riscv/dts/binman.dtsi` and update the path to the `Image` file (e.g. `filename = "../linux/arch/riscv/boot/Image"`).

Then we can compile uboot:
```
make  -j$(nproc) CROSS_COMPILE=riscv64-linux-gnu- OPENSBI=../opensbi/build/platform/generic/firmware/fw_dynamic.bin
```

At the end of the compilation we end up with a `linux.itb` image.
```
u-boot$ ls -s linux.itb
15356 linux.itb
```

Dumping this FIT image reveals that it contains the Linux kernel, OpenSBI and the DTB:
```
u-boot$ dumpimage -l linux.itb
FIT description: Configuration to load OpenSBI before U-Boot
Created:         Thu Dec 11 23:14:13 2025
 Image 0 (linux)
  Description:  Linux
  Created:      Thu Dec 11 23:14:13 2025
  Type:         Standalone Program
  Compression:  uncompressed
  Data Size:    15425024 Bytes = 15063.50 KiB = 14.71 MiB
  Architecture: RISC-V
  Load Address: 0x80200000
  Entry Point:  unavailable
 Image 1 (opensbi)
  Description:  OpenSBI fw_dynamic Firmware
  Created:      Thu Dec 11 23:14:13 2025
  Type:         Firmware
  Compression:  uncompressed
  Data Size:    275424 Bytes = 268.97 KiB = 0.26 MiB
  Architecture: RISC-V
  OS:           RISC-V OpenSBI
  Load Address: 0x80000000
 Image 2 (fdt-1)
  Description:  hifive-unleashed-a00
  Created:      Thu Dec 11 23:14:13 2025
  Type:         Flat Device Tree
  Compression:  uncompressed
  Data Size:    19480 Bytes = 19.02 KiB = 0.02 MiB
  Architecture: Unknown Architecture
 Default Configuration: 'conf-1'
 Configuration 0 (conf-1)
  Description:  hifive-unleashed-a00
  Kernel:       unavailable
  Firmware:     opensbi
  FDT:          fdt-1
  Loadables:    linux
```

We will deal later with the ramdisk which is currently missing.

OK, now that we have a FIT image with the needed components to boot the Linux kernel, we must place it somewhere on storage (SPI-NOR or SDCARD) where SPL can find it.

### Falcon mode from SPI flash

Let's define a new `genimage` configuration file for Falcon mode which will contain U-boot SPL and the new ITB image:
```
image spi-nor-falcon.img {
        size = 32M

        hdimage {
                gpt = true
        }

        partition u-boot-spl {
                image = "u-boot/spl/u-boot-spl.bin"
                offset = 20K
                size = 1M
                partition-type-uuid = 5B193300-FC78-40CD-8002-E86C45580B47
        }

        partition os {
                image = "u-boot/linux.itb"
                offset = 1044K
        }
}
```

The image is generated via:
```
$ genimage --config genimage_spi_linux_falcon.cfg --inputpath .
$ ls images/spi-nor-falcon.img
```

Now we also need to tell SPL that the boot image sits at offset 1044KB or `0x105000`.<br/>
This is done by defining the `CONFIG_SYS_SPI_KERNEL_OFFS` property in the `.config ` file:
```
$ cat u-boot/.config | grep CONFIG_SYS_SPI_KERNEL_OFFS
CONFIG_SYS_SPI_KERNEL_OFFS=0x105000
```
and recompiling uboot.

Let's try to use our new image:
```
$ qemu-system-riscv64 -M sifive_u,msel=6 -smp 5 -m 8G -display none -serial mon:stdio -bios u-boot/spl/u-boot-spl.bin -drive file=images/spi-nor-falcon.img,if=mtd
...
Trying to boot from SPI
Could not get FIT buffer of 15721888 bytes
	check CONFIG_SPL_SYS_MALLOC_SIZE
```

SPL correctly handles the Falcon mode and attempts to load our FIT image, but it fails due to not having enough memory.

This error message comes from `spl_get_fit_load_buffer()` inside `common/spl/spl_fit.c`. It tries to allocate a buffer large enough to fit the whole FIT image via `malloc_cache_aligned()` and it fails.

Uboot SPL is assumed to run from SRAM, before DRAM is initialized. Even after SPL initializes DRAM, it has to know where to load the FIT image from flash.

One way is for board/platform specific code to define a custom load address by overriding a *weak* symbol in the generic uboot. In a [previous post](https://ibanaga.github.io/u-boot/2025/11/06/Running-uboot-hifive-unleashed-qemu.html) we loaded the FIT image at address `0xc0000000` (1GB offset from start of DRAM), so we'll reuse this value also in Falcon mode (it can be any available address in RAM).<br/>
We need to redefine the function:
```
void *board_spl_fit_buffer_addr(ulong fit_size, int sectors, int bl_len)
{
       return (void *)0xc0000000;
}
```
in the board specific source file `board/sifive/unleashed/spl.c`.

A couple of other function will need to be defined for Falcon mode to work correctly on RiscV:
- `int spl_start_uboot(void)`: returns whether to start uboot or go into Falcon mode; in more complex use case you could select the desired flow at runtime (e.g. via a GPIO or jumper), but for now we will always start Falcon mode
- `void __noreturn jump_to_image_linux(struct spl_image_info *spl_image)`: the generic Falcon mode code needs this symbol defined; it is not applicable to RiscV since we cannot jump directly into Linux from SPL, we must pass through OpenSBI first.

The added code inside `board/sifive/unleashed/spl.c` looks like:
```C
#if CONFIG_IS_ENABLED(OS_BOOT)
__weak int spl_start_uboot(void)
{
    return 0;
}

void *board_spl_fit_buffer_addr(ulong fit_size, int sectors, int bl_len)
{
       return (void *)0xc0000000;
}

void __noreturn jump_to_image_linux(struct spl_image_info *spl_image)
{
       panic("jump to linux not supported - use openSBI");
}
#endif
```

Now Falcon mode is enabled and U-boot compiles fine.
It will properly load the FIT image, copy the blobs to their destination addresses and jump into OpenSBI.
But something is wrong since no output is available from the Linux kernel.

Debugging leads to the following code inside `common/spl/spl_opensbi.c`:

```
	/*
	 * Originally, u-boot-spl will place DTB directly after the kernel,
	 * but the size of the kernel did not include the BSS section, which
	 * means u-boot-spl will place the DTB in the kernel BSS section
	 * causing the DTB to be cleared by kernel BSS initializtion.
	 * Moving DTB in front of the kernel can avoid the error.
	 */
#if CONFIG_IS_ENABLED(LOAD_FIT_OPENSBI_OS_BOOT) && \
    CONFIG_VAL(PAYLOAD_ARGS_ADDR)
	memcpy((void *)CONFIG_SPL_PAYLOAD_ARGS_ADDR, spl_image->fdt_addr,
	       fdt_totalsize(spl_image->fdt_addr));
	spl_image->fdt_addr = map_sysmem(CONFIG_SPL_PAYLOAD_ARGS_ADDR, 0);
#endif
```
So we need to also enable `CONFIG_SPL_PAYLOAD_ARGS_ADDR` and set an address where the FDT will be copied to. Let's use address `0X88000000` (128MB from start of RAM).

Now we see that uboot properly jumps into the Linux kernel which outputs startup logs, but fails to mount the filesystem. This is expected since we didn't define any initrd in the ITS file.

Let's add the following information to the `arch/riscv/dts/binman.dtsi` file:
- a new image for initrd, next to the Linux kernel:
```
initrd {
        description = "initramfs";
        type = "ramdisk";
        compression = "none";
        load = <0x88400000>;
        initrd_blob: blob-ext {
                filename = "../busybox/initrd.cpio.gz";
        };
};
```
- add the new initrd image to the `loadables` property inside the configuration node:
```
loadables = "linux", "initrd";
```

In a [previous post](https://ibanaga.github.io/u-boot/2025/11/06/Running-uboot-hifive-unleashed-qemu.html) we saw how `bootm` command loads the blobs from a FIT image, and performs some FDT fixups before jumping into Linux.
These FDT fixups include updating properties in the `/chosen` node, which is used to pass runtime information to the kernel.

All these fixups will need to be already inside the FDT when Falcon mode loads it. Let's add the following information to `arch/riscv/dts/hifive-unleashed-a00.dts`
```
chosen {
        linux,initrd-end = <0x00000000 0x88900000>;
        linux,initrd-start = <0x00000000 0x88400000>;
        bootargs = "loglevel=7";
        stdout-path = "serial0";
};
```
The size if the initrd was chosen 5MB which should be more than enough for a small busybox initrd.

Now we can recompile uboot and re-generate the falcon image for SPI-NOR and run qemu again:

```
$ qemu-system-riscv64 -M sifive_u,msel=6 -smp 5 -m 8G \
-display none -serial mon:stdio \
-bios u-boot/spl/u-boot-spl.bin \
-drive file=images/spi-nor-falcon.img,if=mtd
U-Boot SPL 2026.01-rc4-00014-g87d85139a96a-dirty (Dec 14 2025 - 09:19:10 +0200)
...
Jumping to OpenSBI at address: 0x80000000 FDT 0x88000000
Linux version 6.18.0-12930-gd358e5254674 ...
Machine model: SiFive HiFive Unleashed A00
...
Freeing unused kernel image (initmem) memory: 2156K
Run /init as init process
~ #
```


The system boots fine into Linux, which finds and mounts the initrd. All is looking good now, we finally got Falcon mode to run on the HiFive Unleashed Qemu board. 


To summarize, the following defconfig options have been enabled:
```
CONFIG_SPL_LOAD_FIT_OPENSBI_OS_BOOT=y
CONFIG_SPL_OS_BOOT=y
CONFIG_SPL_OS_BOOT_ARGS=y
CONFIG_SYS_SPI_KERNEL_OFFS=0x105000
CONFIG_SPL_PAYLOAD_ARGS_ADDR=0X88000000
```

### Falcon mode from SDCARD

Given the source code modifications in the previous section,
booting the itb image from SDCARD instead of SPI flash should be straightforward.

There are different options to tell SPL from where to load the `linux.itb` on the SDCARD. Addressing is done in sectors, one sector usually being 512bytes. So offset 1041K will translate to sector number 0x822
```
CONFIG_SYS_MMCSD_RAW_MODE_KERNEL_SECTOR=0x822
```

Loading an OS image from SDCARD supports only the Linux kernel currently, while for RiscV the OS is seen as OpenSBI. Thus a small patch is needed on `spl_mmc.c` to add support for OpenSBI also:
```
diff --git a/common/spl/spl_mmc.c b/common/spl/spl_mmc.c
index d8ce3a84614..419a85c020b 100644
--- a/common/spl/spl_mmc.c
+++ b/common/spl/spl_mmc.c
@@ -156,7 +156,7 @@ static int mmc_load_image_raw_os(struct spl_image_info *spl_image,
        if (ret)
                return ret;

-       if (spl_image->os != IH_OS_LINUX && spl_image->os != IH_OS_TEE) {
+       if (spl_image->os != IH_OS_OPENSBI && spl_image->os != IH_OS_LINUX && spl_image->os != IH_OS_TEE) {
                puts("Expected image is not found. Trying to start U-Boot\n");
                return -ENOENT;
        }
```

Let's define a new `genimage` configuration file:

```
image sdcard-falcon.img {
        size = 32M

        hdimage {
                gpt = true
        }

        partition u-boot-spl {
                image = "u-boot/spl/u-boot-spl.bin"
                offset = 17K
                partition-type-uuid = 5B193300-FC78-40CD-8002-E86C45580B47
        }

        partition os {
                image = "u-boot/linux.itb"
                offset = 1041K
                partition-type-uuid = 2E54B353-1271-4842-806F-E436D6AF6985
        }
}
```

And create a new image:
```
$ genimage --config genimage_linux_sdcard_falcon.cfg --inputpath .
```

We also need to update the qemu command line to boot from SDCARD:
```
$ qemu-system-riscv64 -M sifive_u,msel=11 -smp 5 -m 8G \
-display none -serial stdio \
-bios u-boot/spl/u-boot-spl.bin \
-drive file=images/sdcard-falcon.img,if=sd
```

Now Qemu will boot Linux, and we'll end up with the busybox shell, same as for SPI flash.