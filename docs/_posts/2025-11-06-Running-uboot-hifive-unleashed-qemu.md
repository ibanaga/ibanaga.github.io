---
layout: post
title: "Booting U-boot and Linux on the Sifive Unleashed in Qemu"
date: 2025-11-06
categories: u-boot
---

## Introduction

The [SiFive Unleashed](https://www.sifive.com/boards/hifive-unleashed) is a now discontinued RiscV development board capable of running Linux which has also [support in Qemu](https://www.qemu.org/docs/master/system/riscv/sifive_u.html).

We will use this board to see how Uboot SPL, Uboot proper and Linux boot on RiscV. The boot flow on the real hardware is described in the [getting started guide](https://static.dev.sifive.com/HiFive-Unleashed-Getting-Started-Guide-v1p1.pdf). Qemu has a few tweaks since it does not implement the same BootROM logic.

The SiFive Unleashed can boot from two storage devices:
 - from a 32MB SPI NOR flash
 - from an external SD Card

### Building OpenSBI

```console
$ git clone --depth=1 https://github.com/riscv/opensbi.git && cd opensbi
$ make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- PLATFORM=generic
```

The output is in the `build` directory. We're mostly interested in `build/platform/generic/firmware/fw_dynamic.bin`

### Building Uboot

```console
$ git clone https://github.com/u-boot/u-boot.git && cd u-boot
$ make ARCH=riscv sifive_unleashed_defconfig
$ make -j$(nproc) CROSS_COMPILE=riscv64-linux-gnu- OPENSBI=../opensbi/build/platform/generic/firmware/fw_dynamic.bin
```

U-boot SPL is under: `spl/u-boot-spl.bin`

U-boot proper together with DTB and OpenSBI is in the FIT image: `u-boot.itb`

```
dumpimage -l u-boot.itb
FIT description: Configuration to load OpenSBI before U-Boot
 Image 0 (uboot)
  Description:  U-Boot
  Type:         Standalone Program
  Compression:  uncompressed
  Data Size:    581872 Bytes = 568.23 KiB = 0.55 MiB
  Architecture: RISC-V
  Load Address: 0x80200000
  Entry Point:  unavailable
 Image 1 (opensbi)
  Description:  OpenSBI fw_dynamic Firmware
  Type:         Firmware
  Compression:  uncompressed
  Data Size:    273080 Bytes = 266.68 KiB = 0.26 MiB
  Architecture: RISC-V
  OS:           RISC-V OpenSBI
  Load Address: 0x80000000
 Image 2 (fdt-1)
  Description:  hifive-unleashed-a00
  Type:         Flat Device Tree
  Compression:  uncompressed
  Data Size:    19464 Bytes = 19.01 KiB = 0.02 MiB
  Architecture: Unknown Architecture
 Default Configuration: 'conf-1'
 Configuration 0 (conf-1)
  Description:  hifive-unleashed-a00
  Kernel:       unavailable
  Firmware:     opensbi
  FDT:          fdt-1
  Loadables:    uboot
```

### Building Linux

Building a Linux `defconfig` will produce a large binary which will not fit into the 32MB flash available. A non-standard trimmed down config file can be used which will generate a uImage file of about 15MB. The defconfig used can be [found here](https://github.com/ibanaga/ibanaga.github.io/blob/main/src/sifive_unleashed_build/hifive_unleashed_uart_defconfig).

```console
$ git clone --depth=1 https://github.com/torvalds/linux.git && cd linux
$ make ARCH=riscv hifive_unleashed_uart_defconfig
$ make olddefconfig
$ make -j$(nproc) ARCH=riscv  CROSS_COMPILE=riscv64-linux-gnu-
```

### Building busybox

To be used as an initrd, busybox must be statically linked. This is achieved by setting `CONFIG_STATIC=y` in the `.config` file. Also for recent Linux kernel `CONFIG_TC` must be disabled to avoid errors during the *make install* stage.

```console
$ git clone --depth=1 https://github.com/mirror/busybox.git && cd busybox
$ make defconfig
$ cat .config | grep CONFIG_STATIC
# CONFIG_STATIC is not set
// >>>> set CONFIG_STATIC=y

$ cat .config | grep CONFIG_TC
CONFIG_TC=y
// >>>> disable CONFIG_TC


$ make -j$(nproc) ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu-
$ make -j$(nproc) ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- install

$ ls -1 _install/
bin
linuxrc
sbin
usr

$ cd _install
$ mkdir dev etc proc sys

$ cat << EOF > init                              
#!/bin/sh

mount -t proc proc /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

exec setsid sh -c "exec /bin/sh </dev/ttySIF0 >/dev/ttySIF0 2>&1"
EOF

$ chmod +x init
$ find . | cpio -o -H newc | gzip > ../initrd.cpio.gz
```


### Booting Linux directly with Qemu

This approach allow booting Linux directly, without any bootloader stage. We can quickly verify that the Linux image and the initrd are working fine:

```console
$ qemu-system-riscv64 -machine sifive_u -smp 5 -m 8G \
-display none -serial stdio \
-kernel linux/arch/riscv/boot/Image \
-initrd busybox/initrd.cpio.gz \
-append "earlycon console=ttySIF0,115200"
```

We should see the kernel booting, mounting the ramdisk and then displaying the prompt:

```
...
Freeing unused kernel image (initmem) memory: 2140K
Run /init as init process
~ #

```

### Booting from the SPI flash

#### Generating the flash image

To boot from the on-board SPI NOR, we must first create a binary image that would be flashed on the physical flash as [described here](https://www.qemu.org/docs/master/system/riscv/sifive_u.html).

We specify the image layout in a config file, then use the `genimage` tool to create the actual binary.

The `u-boot-spl` partition is not needed in Qemu (we will use the -bios flag) but will include it to be as close as possible to the real hardware.

The *offset of the `u-boot` partition is fixed*, since Qemu does not replicate the real BootROM which scans the Partition table for a certain GUID. Instead it gets this *fixed offset* from the SPL's DTB.

Also the U-boot environment is fixed in the board configuration: `CONFIG_ENV_OFFSET=0x505000`, `CONFIG_ENV_SIZE=0x20000`

```
image spi-nor.img {
        size = 32M

        hdimage {
                gpt = true
        }

        partition u-boot-spl {
                image = "u-boot/spl/u-boot-spl.bin"
                offset = 17K
                partition-type-uuid = 5B193300-FC78-40CD-8002-E86C45580B47
        }

        partition u-boot {
                image = "u-boot/u-boot.itb"
                offset = 1041K
                partition-type-uuid = 2E54B353-1271-4842-806F-E436D6AF6985
        }

        partition linux-dtb {
                image = "linux/arch/riscv/boot/dts/sifive/hifive-unleashed-a00.dtb"
                offset = "3092K"
                size = 1M
        }

        partition linux-kernel {
                image = "linux/arch/riscv/boot/Image"
                offset = "5268K"
                size = "15M"
        }

        partition linux-initrd {
                image = "busybox/initrd.cpio.gz"
                offset = "20628K"
                size = "2M"
        }
}
```

Now that we have defined the layout, we can build the binary image:
```console
$ genimage --config genimage_linux_spi.cfg --inputpath .
```

The output will be in the images folder: `images/spi-nor.img`

#### Boot into U-boot

To boot from the SPI flash, we need to use the `msel=6` option:

```
qemu-system-riscv64 -M sifive_u,msel=6 -smp 5 -m 8G \
-display none -serial stdio \
-bios u-boot/spl/u-boot-spl.bin \
-drive file=images/spi-nor.img,if=mtd
```

#### Load and start Linux from U-boot

To load and start Linux from SPI flash several steps are needed:
- initialize the flash device, otherwise U-boot will bail out with:<br/>`No SPI flash selected. Please run sf probe `

```
=> sf probe
SF: Detected is25wp256 with page size 256 Bytes, erase size 4 KiB, total 32 MiB
```
- use `sf read` to load from flash into RAM:
   - the Linux kernel: 15MB @ 0x80200000
    ```
    => sf read 0x80200000 0x525000 0xF00000
    device 0 offset 0x525000, size 0xf00000
    SF: 15728640 bytes @ 0x525000 Read: OK
    ```
   - the initrd: 2MB @ 0x88400000
    ```
    => sf read 0x88400000 0x1425000 0x200000
    device 0 offset 0x1425000, size 0x200000
    SF: 2097152 bytes @ 0x1425000 Read: OK
    ```
   - the DTB: 1MB @ 0x88000000 from offset
    ```
    => sf read 0x88000000 0x305000 0x100000
    device 0 offset 0x305000, size 0x100000
    SF: 1048576 bytes @ 0x305000 Read: OK
    ```

- use `booti <linux> <initrd>:<size> <dtb>` to start Linux (note that *size* is mandatory here)
    ```
    => booti 0x80200000 0x88400000:0x200000 0x88000000
    ## Flattened Device Tree blob at 88000000
    Booting using the fdt blob at 0x88000000
    Working FDT set to 88000000
    Using Device Tree in place at 0000000088000000, end 0000000088005033
    Working FDT set to 88000000

    Starting kernel ...

    Linux version 6.17.0-rc2-g8d245acc1e88
    Machine model: SiFive HiFive Unleashed A00
    SBI specification v3.0 detected
    ...
    ```

### Booting from the SD Card

#### Generating the SD card image

The SD CARD is very similar, just the offsets of the first two partition are different.

We can keep the rest of the partitions the same to avoid dealing with different offsets to keep things simpler.
The `uboot-env ` partition can be removed as the config files says `CONFIG_ENV_IS_IN_SPI_FLASH=y`. We could store the environment on SD Card also, but it would imply enabling `CONFIG_ENV_IS_IN_MMC` and rebuilding U-boot.

```
image sdcard.img {
        size = 32M

        hdimage {
                gpt = true
        }

        partition u-boot-spl {
                image = "u-boot/spl/u-boot-spl.bin"
                offset = 17K
                partition-type-uuid = 5B193300-FC78-40CD-8002-E86C45580B47
        }

        partition u-boot {
                image = "u-boot/u-boot.itb"
                offset = 1041K
                partition-type-uuid = 2E54B353-1271-4842-806F-E436D6AF6985
        }

        partition linux-dtb {
                image = "linux/arch/riscv/boot/dts/sifive/hifive-unleashed-a00.dtb"
                offset = "3092K"
                size = 1M
        }

        partition linux-kernel {
                image = "linux/arch/riscv/boot/Image"
                offset = "5268K"
                size = "15M"
        }

        partition linux-initrd {
                image = "busybox/initrd.cpio.gz"
                offset = "20628K"
                size = "2M"
        }
}
```

We build the SD Card image using the `genimage` tool:
```console
$ genimage --config genimage_linux_sdcard.cfg --inputpath .
```

The resulting binary will be in: `images/sdcard.img`

#### Boot into U-boot

To boot from the SD Card, we need to use the `msel=11` option:

```
qemu-system-riscv64 -M sifive_u,msel=11 -smp 5 -m 8G \
-display none -serial stdio \
-bios u-boot/spl/u-boot-spl.bin \
-drive file=images/sdcard.img,if=sd
```

#### Load Linux from U-boot

To load and start Linux from SD Card, we use the `mmc read` command to load the Linux, initrd, and DTB from storage into RAM. `mmc read` operates on blocks of data instead of bytes.

We can find out the read block length via `mmc info`:
```console
=> mmc info
Device: spi@10050000:mmc@0
Manufacturer ID: aa
OEM: 5859
Name: QEMU!
Bus Speed: 20000000
Mode: MMC legacy
Rd Block Len: 512
...

```

Our block length is 512 bytes, so every address and size from the SD Card will need to be divided by this block size.
- use `mmc read` to load from the SD Card into RAM:
   - the Linux kernel: 15MB @ 0x80200000
    ```
    => mmc read 0x80200000 0x2928 0x7800
    MMC read: dev # 0, block # 10536, count 30720 ... 30720 blocks read: OK
    ```
   - the initrd: 2MB @ 0x88400000
    ```
    => mmc read 0x88400000 0xA128 0x1000
    MMC read: dev # 0, block # 41256, count 4096 ... 4096 blocks read: OK
    ```
   - the DTB: 1MB @ 0x88000000
    ```
    => mmc read 0x88000000 0x1828 0x800
    MMC read: dev # 0, block # 6184, count 2048 ... 2048 blocks read: OK
    ```
- use `booti <linux> <initrd>:<size> <dtb>` to start Linux (note that *size* is mandatory here)
    ```
    => booti 0x80200000 0x88400000:0x200000 0x88000000
    ## Flattened Device Tree blob at 88000000
    Booting using the fdt blob at 0x88000000
    Working FDT set to 88000000
    Using Device Tree in place at 0000000088000000, end 0000000088005033
    Working FDT set to 88000000

    Starting kernel ...

    Linux version 6.17.0-rc2-g8d245acc1e88
    Machine model: SiFive HiFive Unleashed A00
    SBI specification v3.0 detected
    ...
    ```

### Booting from a FIT image

A FIT image (Flat Image Tree) bundles together all the images (kernel, dtb, rootfs) into a single file.
Each component has metadata which describes the type of the file, its destination address in RAM, entry point, etc.
Different combinations of the available images can be defined in different configurations.

The text file which describes the layout is called an *Image Tree Source* file (`.its` extension).

The sample source image looks like:
```
/dts-v1/;

/ {
    description = "HiFive RiscV Linux FIT";
    #address-cells = <1>;

    images {
        kernel {
            description = "Linux kernel";
            data = /incbin/("linux/arch/riscv/boot/Image");
            type = "kernel";
            arch = "riscv";
            os = "linux";
            compression = "none";
            load = <0x80200000>;
            entry = <0x80200000>;
        };

        fdt {
            description = "Device Tree Blob";
            data = /incbin/("linux/arch/riscv/boot/dts/sifive/hifive-unleashed-a00.dtb");
            type = "flat_dt";
            arch = "riscv";
            compression = "none";
            load = <0x88000000>;
        };

        initrd {
            description = "initramfs";
            data = /incbin/("busybox/initrd.cpio.gz");
            type = "ramdisk";
            arch = "riscv";
            os = "linux";
            compression = "none";
            load = <0x88400000>;
        };
    };

    configurations {
        default = "conf";
        conf {
            description = "Boot Linux";
            kernel = "kernel";
            fdt = "fdt";
            ramdisk = "initrd";
        };
    };
};
```

The `mkimage` tool is used to compile an .its file into an Image Tree Blob (`.itb` extension).
```
mkimage -f fit-linux.its fit-linux.itb
```

Now that we have a FIT image which contains all necessary components for Linux to boot, we need to get it into the target's RAM:
- via network transfer (e.g. tftp)
- reading it from persistent storage, etc.

For the Qemu use-case, we can make use of the `--device loader` argument which will just place a file at a specified memory address.
Uboot will just find it at the expected address in DRAM and can use it (in this case offset 1GB from start of RAM).
```
qemu-system-riscv64 -M sifive_u,msel=6 -smp 5 -m 8G \
-display none -serial mon:stdio \
-bios u-boot/spl/u-boot-spl.bin \
-drive file=images/spi-nor.img,if=mtd \
-device loader,file=fit-linux.itb,addr=0xc0000000
```

After getting to the uboot prompt, we can use the `bootm <address>` command.

This will parse the FIT image header and will place the embedded binary images at their destination addresses in RAM.
After this it will update the FDT with runtime data needed by the kernel (the `/chosen` node), and will jump in the kernel.
```
=> bootm 0xc0000000
## Loading kernel (any) from FIT Image at c0000000 ...
...
Freeing unused kernel image (initmem) memory: 2156K
Run /init as init process
~ #
```


The `bootm` command can be split into several individual steps that can also be executed manually.

The main steps executed by the `bootm` command are:
 - `start` : set the address of the ITB image and parse the headers
 - `loados`, `ramdisk`, `fdt`: load components at their destination address in RAM
 - `prep`: update the DTB with runtime info needed by the kernel
 - `go` : jump into the kernel

Let's now see how the individual steps look like when executed manually:

```
=> bootm start 0xc0000000
## Loading kernel (any) from FIT Image at c0000000 ...
...
=> bootm loados
   Loading Kernel Image to 80200000
=> bootm ramdisk
=> bootm fdt
   Using Device Tree in place at 0000000088000000, end 0000000088005033
Working FDT set to 88000000
```

The `/chosen` node (which is used to pass boot information to the kernel) has a single property:
```
=> fdt addr 88000000
Working FDT set to 88000000
=> fdt print /chosen
chosen {
	stdout-path = "serial0";
};
```

The `bootm prep` will populate the `/chosen` node with runtime data needed by the kernel, the most important being the start and end of the initrd.
```
=> bootm prep
   Using Device Tree in place at 0000000088000000, end 0000000088008033
Working FDT set to 88000000
=> fdt print /chosen
chosen {
	linux,initrd-end = <0x00000000 0x885311ca>;
	linux,initrd-start = <0x00000000 0x88400000>;
	u-boot,bootconf = "conf";
	boot-hartid = <0x00000003>;
	smbios3-entrypoint = <0x00000000 0xff75e000>;
	u-boot,version = "2026.01-rc4-gff80e95fed18";
	stdout-path = "serial0";
};
```

You can also add arguments to the kernel cmdline by setting the `bootargs` environment variable before `bootm prep`:
```
=> setenv bootargs "loglevel=7"
=> bootm prep
   Using Device Tree in place at 0000000088000000, end 0000000088008033
Working FDT set to 88000000
=> fdt print /chosen
chosen {
	linux,initrd-end = <0x00000000 0x885311ca>;
	linux,initrd-start = <0x00000000 0x88400000>;
	u-boot,bootconf = "conf";
	boot-hartid = <0x00000004>;
	smbios3-entrypoint = <0x00000000 0xff75e000>;
	u-boot,version = "2026.01-rc4-gff80e95fed18";
	bootargs = "loglevel=7";
	stdout-path = "serial0";
};
```

`bootm go` will put the address of the current core in the `a0` register, the address of the DTB into the `a1` register and will jump into the kernel code.