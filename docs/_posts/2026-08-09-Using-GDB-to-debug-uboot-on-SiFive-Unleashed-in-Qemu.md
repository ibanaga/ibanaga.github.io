Qemu allows debugging by remote connecting to the virtual machine via GDB.

Two Qemu command line options support GDB debugging:
* `-s` open a tcp server on localhost:1234 and wait for incoming GDB connections
* `-S` do not start the virtual machine; wait for a remote GDB connection and explicit run command


Let's try to debug the SiFive Unleashed Qemu machine running U-boot from the previous posts.

First we start the Qemu emulation, adding `-s` argument to start a GDB server (details on how to build and boot U-boot [here]({% post_url 2025-11-06-Running-uboot-hifive-unleashed-qemu %})):
```
qemu-system-riscv64 -M sifive_u,msel=6 -smp 5 -m 8G -display none -serial stdio -bios u-boot/spl/u-boot-spl.bin -drive file=images/spi-nor.img,if=mtd -s
```

...and try to connect via GDB, then hit `continue`.
```
$ gdb-multiarch ./u-boot/u-boot -q
Reading symbols from ./u-boot/u-boot...
(gdb) target remote :1234
Remote debugging using :1234
0x0000000080016412 in ?? ()

(gdb) continue
Continuing.
```
The GDB connection succeeds, but after resuming execution U-Boot remains frozen and is not reacting to any input.
As soon as the GDB connection is closed, U-Boot reacts to keyboard input again...

**So what is happening here?**

Things get a bit more complicated when dealing with heterogeneous hardware, where multiple CPU clusters are present and not all cores are the same.

The SiFive Unleashed has 5 CPU cores:
- 1 x SiFive E51: management core, can handle real-time tasks
- 4 x SiFive U54 application cores: can run Linux

![SiFive Unleashed Cores](/assets/gdb-uboot/sifive_u_cpu_clusters.drawio.png)

When we connected to the virtual machine's GDB server, we attached to the E51 core, but execution on all cores was stopped.

![SiFive Unleashed Cores](/assets/gdb-uboot/sifive_u_cpu_clusters_gdb_e51.drawio.png)

This is why, when we resumed execution via `continue`, the U-Boot emulation remained frozen: the four application cores remained halted while only the E51 core resumed execution.  
But the E51 is not running U-boot.

![SiFive Unleashed Cores](/assets/gdb-uboot/sifive_u_cpu_clusters_gdb_e51_continue.drawio.png)

To deal with complex CPU topologies, we need to let GDB know about it.  
Each CPU cluster in the target machine is identified by GDB as an `inferior`.  
Individual CPUs inside a cluster are identified as `threads`.

It is up to the user to explicitly define the topology.

![SiFive Unleashed Cores](/assets/gdb-uboot/sifive_u_cpu_clusters_inferiors.drawio.png)

First we need to connect via `target remote-extended :1234` instead of the normal `target remote`.

Then we need to explicitly add a new `inferior` via `add-inferior` and attach to it via `attach 2`

```
gdb-multiarch -q
(gdb) target extended-remote :1234
Remote debugging using :1234
warning: No executable has been specified and target does not support
determining executable automatically.  Try using the "file" command.
0x0000000080016412 in ?? ()

(gdb) add-inferior
[New inferior 2]
Added inferior 2 on connection 1 (extended-remote :1234)

(gdb) inferior 2
[Switching to inferior 2 [<null>] (<noexec>)]

(gdb) attach 2
Attaching to process 2
warning: No executable has been specified and target does not support
determining executable automatically.  Try using the "file" command.
[New Thread 2.3]
[New Thread 2.4]
[New Thread 2.5]
0x00000000fff96344 in ?? ()

(gdb) info threads
  Id   Target Id                                            Frame
  1.1  Thread 1.1 (sifive-e51-riscv-cpu harts[0] [halted ]) 0x0000000080016412 in ?? ()
* 2.1  Thread 2.2 (sifive-u54-riscv-cpu harts[0] [running]) 0x00000000fff96344 in ?? ()
  2.2  Thread 2.3 (sifive-u54-riscv-cpu harts[1] [halted ]) 0x0000000080016412 in ?? ()
  2.3  Thread 2.4 (sifive-u54-riscv-cpu harts[2] [halted ]) 0x0000000080016412 in ?? ()
  2.4  Thread 2.5 (sifive-u54-riscv-cpu harts[3] [halted ]) 0x0000000080016412 in ?? ()
(gdb)
```

Now the `info threads` will correctly show the heterogeneous topology of one E51 core plus four U54 cores.

Next, we need to load the U-boot ELF file containing the debug symbols.  
But there's a catch: U-boot relocates itself from its load address to the top of DRAM during early initialization stage ([explained here]({% post_url 2025-09-13-Uboot-proper-init-flow %})).

Reading the symbols from the ELF file will give us the *old addresses* before relocation.  
If we set a breakpoint it will never be hit since the real `.text` section *has been relocated to a different address*.

First we need to get the relocation offset and relocation address in U-boot via the `bdinfo` command:
```
=> bdinfo
...
relocaddr   = 0x00000000fff6a000
reloc off   = 0x000000007fd6a000
...
```

We can fix the ELF symbol addresses in two ways:
* load the ELF file specifying an offset:
```
(gdb) add-symbol-file -o 0x7fd6a000 ./u-boot/u-boot
add symbol table from file "./u-boot/u-boot" with all sections offset by 0x7fd6a000
```

* load the ELF file specifying the absolute address of the `.text` section:
```
(gdb) add-symbol-file ./u-boot/u-boot 0x00000000fff6a000
add symbol table from file "./u-boot/u-boot" at
	.text_addr = 0xfff6a000
```

After this step, a breakpoint will be hit as expected:
```
(gdb) br do_version
Breakpoint 1 at 0xfff748f6: file cmd/version.c, line 21.

(gdb) c
Continuing.
[Switching to Thread 2.2]

Thread 2.1 hit Breakpoint 1, do_version (cmdtp=0xfffe3ec8 <_u_boot_list_2_cmd_2_version>, flag=0, argc=1, argv=0xff75fb20) at cmd/version.c:21
21		printf(display_options_get_banner(false, buf, sizeof(buf)));
```


