---
layout: post
title: "SMP in UBoot for RiscV: the hart lottery"
date: 2025-09-14
categories: u-boot riscv
---

## Introduction

Compared with other architectures where usually you have a single core that starts executing code at power on, RiscV does not impose any restrictions on this.

This means that all cores can start executing instructions from the same reset vector.
But of course, several CPUs cannot initialize a single device at the same time, so a mechanism for mutual exclusion needs to be put in place.
This mechanism in Uboot is called the **Hart lottery**

**Hart Lottery** = if multiple cores start executing from the same reset vector, only one can initialize the Global Data structure and run U-boot, while the rest must enter a wait state until an Inter Processor Interrupt is sent to them.


## SMP related configuration options
- `CONFIG_SMP` = enable Symmetric Multiprocessing 
- `CONFIG_XIP` = execute in place from NOR flash memory, not from RAM
    - it will not be possible to use global locks (*hart_lottery*, *available_harts_lock*   ) to synchronize the cores
    - in this case only hart 0 will initialize and run U-boot, all the others will enter a loop waiting for an *Inter Processor Interrupt*
- `CONFIG_AVAILABLE_HARTS` keep a mask of available cores in the Global Data structure
   - e..g 2 cores → mask 0x3, 5 cores → 0x1F
- `CONFIG_NR_CPUS` = the maximum number of active CPUs (can be less or more than the physical number CPUs)

## SMP flow

![U-boot SMP flow](/assets/Uboot_SMP_flow.png)

## Code analysis

Since the SMP handling must be done very early in the boot flow, SMP related code is in assembly code in `arch/riscv/cpu/start.S`.

```
#if CONFIG_IS_ENABLED(SMP)
    /* check if hart is within range /
    / tp: hart id */
    li  t0, CONFIG_NR_CPUS
    bge tp, t0, hart_out_of_bounds_loop
```
- register `tp` contains the hard ID saved at the very beginning
- load into `t0` the configured number of CPUs for this platform
- if the current hart ID >= number of CPUs, jump to the `hart_out_of_bounds_loop` label and halt core

```
#if CONFIG_IS_ENABLED(RISCV_MMODE)
    li  t0, MIE_MSIE
#else
    li  t0, SIE_SSIE
#endif
    csrs    MODE_PREFIX(ie), t0
#endif // CONFIG_SMP
```
- load the Software Interrupt Enable constant value in register in `t0`, either machine or supervisor mode (bits 1 or 3 of the `xIE` register). This will enable Inter Processor Interrupts.
- Control and Status Register Set (`csrs`): `MIE = t0` to set the Software Interrupt Enable bit

```
/* setup stack /
#if CONFIG_IS_ENABLED(SMP)
    / tp: hart id */
    slli    t1, tp, CONFIG_STACK_SIZE_SHIFT
    sub sp, t0, t1
#else
    mv  sp, t0
#endif
```
- register `t0` contains the value of the initial stack pointer
- compute in register `t1` the location for this particular CPU in the global stack space
    - e.g. `t1 = hartid << 14` and `t0 = 0x8020_0000`
        - CPU0 will have stack at `0x80200000`
        - CPU1 will have stack at `0x801F_C000 ($t0 - 0x4000)`
        - CPU2 will have stack at `0x801F_8000 ($t0 - 0x8000)`
- Set the Stack Pointer register (`sp`) for each CPU to its computed value

```

#if !CONFIG_IS_ENABLED(XIP)
    la  t0, hart_lottery
    li  t1, 1
    amoswap.w s2, t1, 0(t0)
    bnez    s2, wait_for_gd_init
#else
```
- if execute in place (XIP) is not enabled, meaning the U-boot is running from RAM (not from NOR flash) we need to pick a single CPU to perform initialization of Global Data. 
- load the address of the `hart_lottery` variable defined in the .data section and set `$t1 = 1`
- perform an *Atomic Swap* operation to exchange a register value with a memory location (*AMO = Atomic Memory Operation*)
   - `.w` = 32bit word
   - `s2` = destination register, that will receive the value of the memory location
   - `t1` = source register, the value we want to write into memory
   - `0(t0)` = address of t0 + offset 0
- all cores will perform this atomic operation, but only the fastest one will find the value of the memory location to be 0
    - others will swap 1 with 1
- if register s2 is not equal to 0 jump to the `wait_for_gd_init` label, you did not won the lottery

Now the winning core performs the initialization of the Global Data structure.

```
#ifdef CONFIG_AVAILABLE_HARTS
    la  t0, available_harts_lock
    amoswap.w.rl zero, zero, 0(t0)
#endif
```
- the `available_harts_lock` is initialized with 1 in the .data section, meaning that the lock is held by the boot core until Global Data is initialized
- atomically write 0 into the `available_harts_lock` variable and discard the old value: release the lock, GD is initialized now and other cores can use it
    - the `.rl` suffix offers release semantics, meaning that all memory operations in program order must be visible before this write is observed by other cores.

```
wait_for_gd_init:
    mv  gp, s0
#ifdef CONFIG_AVAILABLE_HARTS
// see section below ...
#endif
bnez    s2, secondary_hart_loop
```
- register `s0` contains the address of the Global Data, so copy it into the hardware register `gp`
- if register `s2` is not 0 (you are not the boot hart), just to branch out to the `secondary_hart_loop` label

```
secondary_hart_loop:
    wfi
#if CONFIG_IS_ENABLED(SMP)
    csrr    t0, MODE_PREFIX(ip)
#if CONFIG_IS_ENABLED(RISCV_MMODE)
    andi    t0, t0, MIE_MSIE
#else
    andi    t0, t0, SIE_SSIE
#endif
    beqz    t0, secondary_hart_loop
mv  a0, tp
jal handle_ipi
#endif
j   secondary_hart_loop
```
- `wfi` = *Wait For Interrupt*; this will wake the core when an Inter Processor Interrupt is received
- `csrr` = Control and Status Register read
- `t0` = value of MIP or SIP register, depending on current running mode (machine or supervisor)
    - *MIP/SIP = Machine Interrupt register*, holds the information on pending interrupts
    - check if a machine software interrupt or a supervisor software interrupt is pending:
       - t0 = t0 & MIE_MSIE or SIE_SSIE mask
- branch on equal to 0 - if bit is not set, go back to sleep until IPI comes
- in case we have an IPI. the `tp` register contains the hart id, pass it as argument to the `handle_ipi()` function
   - `a0` - first argument register
- call `handle_ipi(ulong hart`) to handle the IPI
- go back to wait for other IPI

#### CONFIG_AVAILABLE_HARTS
`CONFIG_AVAILABLE_HARTS` adds a mask to the GD that is used to keep track of what cores are currently alive.
This logic is inside the `wait_for_gd_init` label.

```
wait_for_gd_init:
    mv  gp, s0
#ifdef CONFIG_AVAILABLE_HARTS

#ifdef CONFIG_AVAILABLE_HARTS
    la  t0, available_harts_lock
    li  t1, 1
1:  amoswap.w.aq t1, t1, 0(t0)
    bnez    t1, 1b
/* register available harts in the available_harts mask */
li  t1, 1
sll t1, t1, tp
LREG    t2, GD_AVAILABLE_HARTS(gp)
or  t2, t2, t1
SREG    t2, GD_AVAILABLE_HARTS(gp)
amoswap.w.rl zero, zero, 0(t0)
#endif
 
#endif
bnez    s2, secondary_hart_loop
#endif
```
- `t0` = address of `available_harts_lock`, `t1 = 1`
- perform atomic swap between `t1` and `available_harts_lock` variable with acquire semantics
    - all loads/stores after this instruction in program order cannot be reordered before 
- if the value of `available_harts_lock` was not zero (the lock is held by someone else) jump to the label `1` and retry the swap
- `t1` will contain the bit mask of the current hart/core: `t1 = t1 << hart id`
- `t2` = the value of `arch.available_harts` variable in the Global Data
- update the mask: `t2 = t2 | t1`
- store the updated mask back in the `arch.available_harts` variable inside Global Data
- release the lock: perform an *atomic swap* between `available_harts_lock` and register zero