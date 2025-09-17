---
layout: post
title: "U-boot SPL Device Tree analysis"
date: 2025-09-14
categories: u-boot SPL
---

## Introduction

U-boot SPL does not use the full-fledged DTS for the HiFive Unleashed board, as it contains many devices that are of no interest for this early boot stage.

Instead, it uses a trimmed down version of the DTS, only with nodes that are used during the boot stage (CPUs, clocks, DDR memory, SPI controller, Flash and MMC for storage, etc).

This post analyses the SPL DTS in order the understand what the SPL actually sees and needs during the the low level initialization of the target. It also explains some basic concepts about reading a DTS file. 

## Diagram of the SPL DTS

![U-boot SPL DTS devices](/assets/Uboot_SPL_DTS_devices.png)

## Analysis

Let’s look now at every node and see how it is defined and configured.
There are some common properties that will be explained here to avoid repetitions:

| DTS key-value pair | Description |
| ----------- | ----------- |
| `compatible=“some_string“` |  a string describing the device; must match the string in the driver implementation during the probing stage|
| `#address-cells=<N>` | defines how many 32bit words make up an address (e.g 2 address-cells -> each address within this node and its children is 64 bits: 2 x 32bits words) |
| `#size-cells=<N>` | similar to address-cells, defines how many 32bit words within this node defines a size (1 size cell -> each size is a 32bit word) |
| `reg=<address size>` | used in correlation with address-cells and size-cells, it describes the address space occupied by a device (e.g. 2 address-cells and 2 size cells):<br/> - `reg = <0x00 0x10000000 0x00 0x1000>`<br/>- this device is mapped at address 0x10000000 and uses 0x1000 bytes |
| `phandle=<N>` | a numeric ID identifying or referencing a device |
| `clocks=<provider index>` | References a clock provider used; each provider can expose multiple clocks, thus an index is needed to identify each one |
| `resets=<provider index>` | References the device which controls the reset line for this device. A device can control several reset lines, thus an index is needed to identify a particular line |

Let’s now look at individual nodes.

### Clock handling and reset lines:

Common properties:
- `#clock-cells`: defines how many arguments a consumer must provide when referencing a clock, besides the `phandle` number
   - `#clock-cells = <0x00>` means that for a consumer to reference this clock, only `phandle` is needed; e.g. `clocks = <0x01>`
   - `#clock-cells = <0x01>` means that for a consumer to reference this lock, 2 arguments are needed; e.g. `clocks = <0x01 0x03>` (clock with phandle 0x01, slot/index 0x03)
- `#reset-cells`: same as clock-cells above

We have two clocks in the DTS: a real time clock `rtcclk` and a high frequency clock `hfclk`.

#### The RTC clock

```
rtcclk {
    #clock-cells = <0x00>;
    compatible = "fixed-clock";
    clock-frequency = <0xf4240>;
    clock-output-names = "rtcclk";
    phandle = <0x09>;
};
```

- this clock can be referenced by its `phandle` alone, no extra arguments needed
- it has a fixed frequency clock of 1MHz
- it is not memory mapped (no `reg` property), thus it cannot be configured

#### The high frequency clock

```
hfclk {
    #clock-cells = <0x00>;
    compatible = "fixed-clock";
    clock-frequency = <0x1fca055>;
    clock-output-names = "hfclk";
    phandle = <0x08>;
};
```

Similar to the previous `rtcclk`, only the frequency differs: 33MHz

#### The clock-controller

```
clock-controller@10000000 {
    compatible = "sifive,fu540-c000-prci";
    reg = <0x00 0x10000000 0x00 0x1000>;
    clocks = <0x08 0x09>;
    #clock-cells = <0x01>;
    #reset-cells = <0x01>;
    resets = <0x01 0x00 0x01 0x01 0x01 0x02 0x01 0x03 0x01 0x05>;
    reset-names = "ddr_ctrl\\\\0ddr_axi\\\\0ddr_ahb\\\\0ddr_phy\\\\0gemgxl_reset";
    phandle = <0x01>;
};
```
The clock controller generates and distributes clocks to other devices in the system:
- the controller is memory mapped at address 0x10000000 and uses 4096 bytes of MMIO space.
- it uses the fixed clocks with phandles 0x08 and 0x09 (`hfclk` and `rtcclk`)
- one extra argument must be passed to use a clock or reset from this provider (`clock-cells = 0x01`, `reset-cells = 0x01`)
- the *reset lines*:
    - 5 reset lines are defined by name: ddr_ctrl, ddr_axi, ddr_ahb, ddr_phy, gemgxl_reset
    - resets property value contains 5 tuples of the form (`phandle reset_line`)
        - phandle is always 0x01 (this device)

### Core local interrupt controller

```
clint@2000000 {
    compatible = "riscv,clint0";
    interrupts-extended = <0x03 0x03 0x03 0x07 0x04 0x03 0x04 0x07 0x05 0x03 0x05 0x07 0x06 0x03 0x06 0x07 0x07 0x03 0x07 0x07>;
    reg = <0x00 0x2000000 0x00 0x10000>;
    clocks = <0x09>;
};
```

The Core Local Interrupt Controller is used to generate interrupts within a core.

CLINT generates 2 interrupts per core (IRQ 3 and IRQ 7):
- machine mode software interrupt (MSIP): write to msip register
- machine mode time interrupt (MTIP): used to generate periodic interrupts via a built-in timer

It uses the `rtcclock` which has phandle 0x09.

The `interrupts-extended` property defines which interrupts are triggered on each core.
There are 5 cores x 2 interrupts → 10 tuples of the form (`core irq`). E.g.
- 0x03 0x03: core with phandle 0x03 is receiving IRQ 3
- 0x03 0x07: core with phandle 0x03 is receiving IRQ 7
- same for all other cores

### SPI Controllers

There are two SPI controllers: one that handled the NOR flash and the other one which handles the SD card.

#### The NOR flash SPI controller

```
spi@10040000 {
    compatible = "sifive,fu540-c000-spi\0sifive,spi0";
    reg = <0x00 0x10040000 0x00 0x1000 0x00 0x20000000 0x00 0x10000000>;
    clocks = <0x01 0x03>;
    #address-cells = <0x01>;
    #size-cells = <0x00>;
    status = "okay";
    flash@0 {
        compatible = "issi,is25wp256\\0jedec,spi-nor";
        reg = <0x00>;
        spi-max-frequency = <0x2faf080>;
        m25p,fast-read;
        spi-tx-bus-width = <0x04>;
        spi-rx-bus-width = <0x04>;
    };
};
```

This SPI controller is memory mapped at address 0x10040000 and uses 4096 bytes.

But we see the reg property contains another (`address range`) tuple: it is also memory mapped at address 0x20000000 and uses 256MB of memory.

This is because the flash on the SiFive Unleashed board is NOR flash with *execute in place* capabilities. This means that code can be executed directly from flash, and data can be fetched as if it was in RAM.

This is why the contents of the NOR flash is memory mapped into the address space and can be accessed just like normal RAM.

This SPI controller uses the clock index 0x03 from the clock-controller device with phandle 0x01
 
The `address-cells` and `size-cells` defined here will affect child nodes: so `flash@0` can be adressed with a single argument and doesn’t define any size.
 
`flash@0` is the child device attached to this controller:
- reg = <0x00> denoted the ChipSelect line 0 used to access this device over SPI
- max frequency is 50MHz

#### The MMC SPI controller

```
spi@10050000 {
    compatible = "sifive,fu540-c000-spi\\\\0sifive,spi0";
    reg = <0x00 0x10050000 0x00 0x1000>;
    clocks = <0x01 0x03>;
    #address-cells = <0x01>;
    #size-cells = <0x00>;
    status = "okay";
    mmc@0 {
        compatible = "mmc-spi-slot";
        reg = <0x00>;
        spi-max-frequency = <0x1312d00>;
        voltage-ranges = <0xce4 0xce4>;
        disable-wp;
    };
}
```

Similar to the above above, this second SPI controller is mapped to 0x10050000 and uses 4096 bytes of MMIO space.

Here we have a single MMIO region, for mmc control. MMC content cannot be memory mapped.

It uses the same clock index 3 on the clock-controller and has a MMC controller accessed by a single value (Chip Select line 0)

`disable-wp` tells the driver to ignore the hardware write protect gpio use to prevent accidental writes on the mmc, basically always allowing writes to the mmc.

### The Serial module
```
serial@10010000 {
    compatible = "sifive,fu540-c000-uart\\0sifive,uart0";
    reg = <0x00 0x10010000 0x00 0x1000>;
    clocks = <0x01 0x03>; 	
    status = "okay";
};
```

Very simple node:
- a single memory mapped area at address 0x1001_0000 of size 4096 bytes.
- uses clock with index 3 of the clock-controller@10000000


### The GPIO module

```
gpio@10060000 {
	compatible = "sifive,fu540-c000-gpio\0sifive,gpio0";
	reg = <0x00 0x10060000 0x00 0x1000>;
	gpio-controller;
	#gpio-cells = <0x02>;
	interrupt-controller;
	#interrupt-cells = <0x02>;
	clocks = <0x01 0x03>;
	status = "okay";
	phandle = <0x0c>;
};
```
- defines a single memory mapped area of 4096 bytes
- it is marked as a `gpio-controller` that provides GPIO functionality to other devices
- `#gpio-cells = <0x02>` defines that each GPIO pin is defined by 2 values: gpio_pin_number and flags (e.g. active high, active low, etc)
- it is also marked as an `interrupt-controller`: it can generate interrupts based on the GPIO pin state
- `#interrupt-cells` defines that each interrupt in this controller is defined by 2 values: gpio_pin_number and trigger_type

### The DDR memory

#### The memory controller

```
dmc@100b0000 {
	compatible = "sifive,fu540-c000-ddr";
	reg = <0x00 0x100b0000 0x00 0x800 0x00 0x100b2000 0x00 0x2000 0x00 0x100b8000 0x00 0x1000>;
	clocks = <0x01 0x01>;
	clock-frequency = <0x37a1894c>;
	sifive,ddr-params = <…>
};
```

This is the DDRAM memory controller, which talks to the main System RAM.

It has three memory mapped areas for different functional blocks:
- general controller configuration
- DDR PHY registers
- memory scheduler / perf monitor block

The `sifive, ddr-params` property (omitted here for brevity) contains a list of values to be written into register to properly initialize the controllers. This is vendor specific and the SPL just writes the values in the proper registers.

#### The DDR memory

```
memory@80000000 {
	device_type = "memory";
	reg = <0x00 0x80000000 0x02 0x00>;
};
```

This is the actual DDR memory. It is mapped at 0x8000_0000 and of size 0x2_0000_0000 (8GB)

### The CPUs

```
cpu@0 {
    compatible = "sifive,e51\\\\0sifive,rocket0\\\\0riscv";
    device_type = "cpu";
    i-cache-block-size = <0x40>;
    i-cache-sets = <0x80>;
    i-cache-size = <0x4000>;
    reg = <0x00>;
    riscv,isa = "rv64imac";
    status = "okay";
    clocks = <0x01 0x00>;
    interrupt-controller {
        #interrupt-cells = <0x01>;
        compatible = "riscv,cpu-intc";
        interrupt-controller;
        phandle = <0x03>;
    };
};
```

The CPU node declares:
- the cache topology
- its ISA: rv64imac = riscv64biti + integer + multiply/divide + atomics + compressed instructions

It uses clock index 0 from the clock-controller

It has a per-hart interrupt-controller with `#interrupt-cells = <0x01>` → each interrupt handled by this controller is defined by one 32bit word

The phandle 0x03 will be referenced by the CLINT when defining the interrupts generated for this core.


### Configuration options

```
	config {
		u-boot,spl-payload-offset = <0x105000>;
	};
```

On the real hardware, the BootROM will inspect the partition table of the storage medium (nand or sdcard) and will look for a specific partition GUID to identify the second stage bootloader.

The Qemu emulation of the HiFive Unleashed does not reproduce the BootROM of the real hardware, and it uses this configuration option in the device tree to specify at which offset the SPL is stored.

## The full decompiled DTS from u-boot-spl.bin

```
/dts-v1/;

/ {
	#address-cells = <0x02>;
	#size-cells = <0x02>;
	compatible = "sifive,hifive-unleashed-a00\0sifive,fu540-c000";
	model = "SiFive HiFive Unleashed A00";

	aliases {
		serial0 = "/soc/serial@10010000";
		cpu1 = "/cpus/cpu@1";
		cpu2 = "/cpus/cpu@2";
		cpu3 = "/cpus/cpu@3";
		cpu4 = "/cpus/cpu@4";
		spi0 = "/soc/spi@10040000";
		spi2 = "/soc/spi@10050000";
	};

	chosen {
		stdout-path = "serial0";
	};

	cpus {
		#address-cells = <0x01>;
		#size-cells = <0x00>;
		timebase-frequency = <0xf4240>;
		assigned-clocks = <0x01 0x00>;
		assigned-clock-rates = <0x3b9aca00>;

		cpu@0 {
			compatible = "sifive,e51\0sifive,rocket0\0riscv";
			device_type = "cpu";
			i-cache-block-size = <0x40>;
			i-cache-sets = <0x80>;
			i-cache-size = <0x4000>;
			reg = <0x00>;
			riscv,isa = "rv64imac";
			status = "okay";
			clocks = <0x01 0x00>;

			interrupt-controller {
				#interrupt-cells = <0x01>;
				compatible = "riscv,cpu-intc";
				interrupt-controller;
				phandle = <0x03>;
			};
		};

		cpu@1 {
			compatible = "sifive,u54-mc\0sifive,rocket0\0riscv";
			d-cache-block-size = <0x40>;
			d-cache-sets = <0x40>;
			d-cache-size = <0x8000>;
			d-tlb-sets = <0x01>;
			d-tlb-size = <0x20>;
			device_type = "cpu";
			i-cache-block-size = <0x40>;
			i-cache-sets = <0x40>;
			i-cache-size = <0x8000>;
			i-tlb-sets = <0x01>;
			i-tlb-size = <0x20>;
			mmu-type = "riscv,sv39";
			reg = <0x01>;
			riscv,isa = "rv64imafdc";
			tlb-split;
			next-level-cache = <0x02>;
			clocks = <0x01 0x00>;

			interrupt-controller {
				#interrupt-cells = <0x01>;
				compatible = "riscv,cpu-intc";
				interrupt-controller;
				phandle = <0x04>;
			};
		};

		cpu@2 {
			compatible = "sifive,u54-mc\0sifive,rocket0\0riscv";
			d-cache-block-size = <0x40>;
			d-cache-sets = <0x40>;
			d-cache-size = <0x8000>;
			d-tlb-sets = <0x01>;
			d-tlb-size = <0x20>;
			device_type = "cpu";
			i-cache-block-size = <0x40>;
			i-cache-sets = <0x40>;
			i-cache-size = <0x8000>;
			i-tlb-sets = <0x01>;
			i-tlb-size = <0x20>;
			mmu-type = "riscv,sv39";
			reg = <0x02>;
			riscv,isa = "rv64imafdc";
			tlb-split;
			next-level-cache = <0x02>;
			clocks = <0x01 0x00>;

			interrupt-controller {
				#interrupt-cells = <0x01>;
				compatible = "riscv,cpu-intc";
				interrupt-controller;
				phandle = <0x05>;
			};
		};

		cpu@3 {
			compatible = "sifive,u54-mc\0sifive,rocket0\0riscv";
			d-cache-block-size = <0x40>;
			d-cache-sets = <0x40>;
			d-cache-size = <0x8000>;
			d-tlb-sets = <0x01>;
			d-tlb-size = <0x20>;
			device_type = "cpu";
			i-cache-block-size = <0x40>;
			i-cache-sets = <0x40>;
			i-cache-size = <0x8000>;
			i-tlb-sets = <0x01>;
			i-tlb-size = <0x20>;
			mmu-type = "riscv,sv39";
			reg = <0x03>;
			riscv,isa = "rv64imafdc";
			tlb-split;
			next-level-cache = <0x02>;
			clocks = <0x01 0x00>;

			interrupt-controller {
				#interrupt-cells = <0x01>;
				compatible = "riscv,cpu-intc";
				interrupt-controller;
				phandle = <0x06>;
			};
		};

		cpu@4 {
			compatible = "sifive,u54-mc\0sifive,rocket0\0riscv";
			d-cache-block-size = <0x40>;
			d-cache-sets = <0x40>;
			d-cache-size = <0x8000>;
			d-tlb-sets = <0x01>;
			d-tlb-size = <0x20>;
			device_type = "cpu";
			i-cache-block-size = <0x40>;
			i-cache-sets = <0x40>;
			i-cache-size = <0x8000>;
			i-tlb-sets = <0x01>;
			i-tlb-size = <0x20>;
			mmu-type = "riscv,sv39";
			reg = <0x04>;
			riscv,isa = "rv64imafdc";
			tlb-split;
			next-level-cache = <0x02>;
			clocks = <0x01 0x00>;

			interrupt-controller {
				#interrupt-cells = <0x01>;
				compatible = "riscv,cpu-intc";
				interrupt-controller;
				phandle = <0x07>;
			};
		};
	};

	soc {
		#address-cells = <0x02>;
		#size-cells = <0x02>;
		compatible = "sifive,fu540-c000\0sifive,fu540\0simple-bus";
		ranges;

		clock-controller@10000000 {
			compatible = "sifive,fu540-c000-prci";
			reg = <0x00 0x10000000 0x00 0x1000>;
			clocks = <0x08 0x09>;
			#clock-cells = <0x01>;
			#reset-cells = <0x01>;
			resets = <0x01 0x00 0x01 0x01 0x01 0x02 0x01 0x03 0x01 0x05>;
			reset-names = "ddr_ctrl\0ddr_axi\0ddr_ahb\0ddr_phy\0gemgxl_reset";
			phandle = <0x01>;
		};

		serial@10010000 {
			compatible = "sifive,fu540-c000-uart\0sifive,uart0";
			reg = <0x00 0x10010000 0x00 0x1000>;
			clocks = <0x01 0x03>;
			status = "okay";
		};

		spi@10040000 {
			compatible = "sifive,fu540-c000-spi\0sifive,spi0";
			reg = <0x00 0x10040000 0x00 0x1000 0x00 0x20000000 0x00 0x10000000>;
			clocks = <0x01 0x03>;
			#address-cells = <0x01>;
			#size-cells = <0x00>;
			status = "okay";

			flash@0 {
				compatible = "issi,is25wp256\0jedec,spi-nor";
				reg = <0x00>;
				spi-max-frequency = <0x2faf080>;
				m25p,fast-read;
				spi-tx-bus-width = <0x04>;
				spi-rx-bus-width = <0x04>;
			};
		};

		spi@10050000 {
			compatible = "sifive,fu540-c000-spi\0sifive,spi0";
			reg = <0x00 0x10050000 0x00 0x1000>;
			clocks = <0x01 0x03>;
			#address-cells = <0x01>;
			#size-cells = <0x00>;
			status = "okay";

			mmc@0 {
				compatible = "mmc-spi-slot";
				reg = <0x00>;
				spi-max-frequency = <0x1312d00>;
				voltage-ranges = <0xce4 0xce4>;
				disable-wp;
			};
		};

		gpio@10060000 {
			compatible = "sifive,fu540-c000-gpio\0sifive,gpio0";
			reg = <0x00 0x10060000 0x00 0x1000>;
			gpio-controller;
			#gpio-cells = <0x02>;
			interrupt-controller;
			#interrupt-cells = <0x02>;
			clocks = <0x01 0x03>;
			status = "okay";
			phandle = <0x0c>;
		};

		clint@2000000 {
			compatible = "riscv,clint0";
			interrupts-extended = <0x03 0x03 0x03 0x07 0x04 0x03 0x04 0x07 0x05 0x03 0x05 0x07 0x06 0x03 0x06 0x07 0x07 0x03 0x07 0x07>;
			reg = <0x00 0x2000000 0x00 0x10000>;
			clocks = <0x09>;
		};

		dmc@100b0000 {
			compatible = "sifive,fu540-c000-ddr";
			reg = <0x00 0x100b0000 0x00 0x800 0x00 0x100b2000 0x00 0x2000 0x00 0x100b8000 0x00 0x1000>;
			clocks = <0x01 0x01>;
			clock-frequency = <0x37a1894c>;
			sifive,ddr-params = <0xa00 0x00 0x00 ...>
		};
	};

	memory@80000000 {
		device_type = "memory";
		reg = <0x00 0x80000000 0x02 0x00>;
	};

	hfclk {
		#clock-cells = <0x00>;
		compatible = "fixed-clock";
		clock-frequency = <0x1fca055>;
		clock-output-names = "hfclk";
		phandle = <0x08>;
	};

	rtcclk {
		#clock-cells = <0x00>;
		compatible = "fixed-clock";
		clock-frequency = <0xf4240>;
		clock-output-names = "rtcclk";
		phandle = <0x09>;
	};

	config {
		u-boot,spl-payload-offset = <0x105000>;
	};
};
```
