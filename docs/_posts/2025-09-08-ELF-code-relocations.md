---
layout: post
title: "ELF code relocations"
date: 2025-09-08
categories: ELF compilers
---

## Introduction

In the [previous article]({% post_url 2025-09-07-Dissecting-an-ELF-binary %}) we saw how a very basic ELF life looks like and how it is mapped into memory.
In this new post we will look at slightly more complicated use cases in order to understand how code relocation works.

Let’s take the simple example from the last time and split the source code into two files, also adding a new `global_sum()` function.

Now we have:

<table>
  <tr>
    <th>File name</th>
    <th>Content</th>
  </tr>
  <tr>
    <td>main.c</td>
    <td>
      {% highlight C %}
extern int global1;
extern int global2;
char global_array[512];

extern int sum(int a, int b);
extern int global_sum();

int main() {
    int res1 = sum(global1, global2);
    res1 += global_sum();
    return res1;
}
      {% endhighlight %}
    </td>
  </tr>
  <tr>
    <td>sum.c</td>
    <td>
      {% highlight C %}
int global1 = 10;
int global2 = 20;

int sum(int a, int b) {
        return a + b;
}

int global_sum(void) {
	return global1 + global2;
}
      {% endhighlight %}
    </td>
  </tr>
</table>

To avoid some implicit sections added by the toolchain, we will also:
 - remove the `.eh_frame` that we previously ignored by adding `-fno-asynchronous-unwind-tables` to the `gcc` command line
 - remove a `.note.gnu.property` section added by the toolchain via:<br/> `objcopy --remove-section .note.gnu.property`

This will keep us focused and prevent cluttering.

The full source code for this post can be found [here](https://github.com/ibanaga/ibanaga.github.io/tree/main/src/elf_code_relocations).

## Linking against a shared object

 A quick look at the symbols of the new object file `sum.o` reveals what we expect: our 2 global variables and the 2 global functions.
 ```console
$ readelf -s sum.o

Symbol table '.symtab' contains 7 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS sum.c
     2: 0000000000000000     0 SECTION LOCAL  DEFAULT    1 .text
     3: 0000000000000000     4 OBJECT  GLOBAL DEFAULT    3 global1
     4: 0000000000000004     4 OBJECT  GLOBAL DEFAULT    3 global2
     5: 0000000000000000    24 FUNC    GLOBAL DEFAULT    1 sum
     6: 0000000000000018    24 FUNC    GLOBAL DEFAULT    1 global_sum
```

And of course there are no program segments in this object file, since it is not yet executable.
```console
$ readelf -l sum.o
There are no program headers in this file.
```

But how does `main.o` look like now, when we puled out some data out of it? Previously it knew the addresses of the global variables and of the `sum()` function, but how is it handling it now that they are not in the same file anymore?
```console
$ objdump -d main.o

main.o:     file format elf64-x86-64


Disassembly of section .text:

0000000000000000 <main>:
   0:	f3 0f 1e fa          	endbr64
   4:	55                   	push   %rbp
   5:	48 89 e5             	mov    %rsp,%rbp
   8:	48 83 ec 10          	sub    $0x10,%rsp
   c:	c7 05 00 00 00 00 64 	movl   $0x64,0x0(%rip)        # 16 <main+0x16>
  13:	00 00 00
  16:	8b 15 00 00 00 00    	mov    0x0(%rip),%edx        # 1c <main+0x1c>
  1c:	8b 05 00 00 00 00    	mov    0x0(%rip),%eax        # 22 <main+0x22>
  22:	89 d6                	mov    %edx,%esi
  24:	89 c7                	mov    %eax,%edi
  26:	e8 00 00 00 00       	call   2b <main+0x2b>
  2b:	89 45 fc             	mov    %eax,-0x4(%rbp)
  2e:	b8 00 00 00 00       	mov    $0x0,%eax
  33:	e8 00 00 00 00       	call   38 <main+0x38>
  38:	01 45 fc             	add    %eax,-0x4(%rbp)
  3b:	8b 45 fc             	mov    -0x4(%rbp),%eax
  3e:	c9                   	leave
  3f:	c3                   	ret
```

Aha! Where were previously memory addresses, now we have only `00`s since the `main.o` object file does not yet know where in memory those external dependencies will be placed (lines 16, 1c, 26, 33).

The sections list reveals a new section called `.rela.text` of size 96 bytes.
```console
$ readelf -S main.o
There are 11 section headers, starting at offset 0x288:

Section Headers:
  [Nr] Name              Type             Address           Offset
       Size              EntSize          Flags  Link  Info  Align
   ...
  [ 2] .rela.text        RELA             0000000000000000  000001c8
       0000000000000060  0000000000000018   I       8     1     8
   ...
```

This is a *code relocation* section, which describes the external dependencies of this file to other symbols.

We can see more details via `readelf -r`:
```console
$ readelf -r main.o

Relocation section '.rela.text' at offset 0x1c8 contains 4 entries:
  Offset          Info           Type           Sym. Value    Sym. Name + Addend
00000000000e  000400000002 R_X86_64_PC32     0000000000000000 global2 - 4
000000000014  000500000002 R_X86_64_PC32     0000000000000000 global1 - 4
00000000001d  000600000004 R_X86_64_PLT32    0000000000000000 sum - 4
00000000002a  000700000004 R_X86_64_PLT32    0000000000000000 global_sum - 4
```

We have 4 relocation entries that must be resolved before being able to run the code.

There are multiple relocation types supported, and in this simple example we use two of them:
- `R_X86_64_PC32`: *Program Counter relative 32bit relocation*
    - the address is calculated based on the currently executed instruction
    - 32bit -> it can jump +/- 2GB relative to the PC
- `R_X86_64_PLT32:` *Procedure Linkage Table relocation*
    - PLT is a table used as a placeholder for an unknown value
    - usually used for linking against shared libraries, but the compiler assumes dynamic linking by default for external symbols
        - it works for both static and dynamic linking
    - for statically linked binaries, the linker can use directly the function's address since it is known at link time

Let's disassembly the final executable:
```console
$ objdump -d sample_exe

sample_exe:     file format elf64-x86-64


Disassembly of section .text:

0000000000401000 <main>:
  401000:	f3 0f 1e fa          	endbr64
  401004:	55                   	push   %rbp
  401005:	48 89 e5             	mov    %rsp,%rbp
  401008:	48 83 ec 10          	sub    $0x10,%rsp
  40100c:	8b 15 f2 0f 00 00    	mov    0xff2(%rip),%edx        # 402004 <global2>
  401012:	8b 05 e8 0f 00 00    	mov    0xfe8(%rip),%eax        # 402000 <global1>
  401018:	89 d6                	mov    %edx,%esi
  40101a:	89 c7                	mov    %eax,%edi
  40101c:	e8 23 00 00 00       	call   401044 <sum>
  401021:	89 45 fc             	mov    %eax,-0x4(%rbp)
  401024:	b8 00 00 00 00       	mov    $0x0,%eax
  401029:	e8 2e 00 00 00       	call   40105c <global_sum>
  40102e:	01 45 fc             	add    %eax,-0x4(%rbp)
  401031:	8b 45 fc             	mov    -0x4(%rbp),%eax
  401034:	c9                   	leave
  401035:	c3                   	ret

0000000000401036 <_start>:
  401036:	e8 c5 ff ff ff       	call   401000 <main>
  40103b:	89 c7                	mov    %eax,%edi
  40103d:	b8 3c 00 00 00       	mov    $0x3c,%eax
  401042:	0f 05                	syscall

0000000000401044 <sum>:
  401044:	f3 0f 1e fa          	endbr64
  401048:	55                   	push   %rbp
  401049:	48 89 e5             	mov    %rsp,%rbp
  40104c:	89 7d fc             	mov    %edi,-0x4(%rbp)
  40104f:	89 75 f8             	mov    %esi,-0x8(%rbp)
  401052:	8b 55 fc             	mov    -0x4(%rbp),%edx
  401055:	8b 45 f8             	mov    -0x8(%rbp),%eax
  401058:	01 d0                	add    %edx,%eax
  40105a:	5d                   	pop    %rbp
  40105b:	c3                   	ret

000000000040105c <global_sum>:
  40105c:	f3 0f 1e fa          	endbr64
  401060:	55                   	push   %rbp
  401061:	48 89 e5             	mov    %rsp,%rbp
  401064:	8b 15 96 0f 00 00    	mov    0xf96(%rip),%edx        # 402000 <global1>
  40106a:	8b 05 94 0f 00 00    	mov    0xf94(%rip),%eax        # 402004 <global2>
  401070:	01 d0                	add    %edx,%eax
  401072:	5d                   	pop    %rbp
  401073:	c3                   	ret
```

The `global1` and `global2` variables have been resolved to relative addresses in the .data section.
- address `0x402004` for `global2` variable (line `40100c`)
- address `0x402000` for `global1` variable (line `401012`)


Let's see how these relative addresses are calculated...

The Program Counter moves one disassembled code line at a time, as it can be seen in a debugger:
```console
Breakpoint 1, 0x0000000000401008 in main ()
(gdb) stepi
0x000000000040100c in main ()
(gdb) stepi
0x0000000000401012 in main ()
(gdb) stepi
0x0000000000401018 in main ()
```

In x86-64 architecture, when the current instruction is being executed, the PC will already point to the next instruction (when the instruction at address `0x40100c` is executed, the PC already points to `0x401012`). So the addresses for the global variables will be calculated as:
- `mov    0xff2(%rip),%edx` == `0xff2 + current PC value`
- for `global2`: `mov  0xff2(%rip),%edx`: 0x401012 + 0xff2 => `0x402004`
- for `global1`: `mov  0xfe8(%rip),%eax`: 0x401018 + 0xfe8 => `0x402000`

For the `sum()` function the absolute address is used since the linker already knows it. No actual PLT relocation is needed here.

In the final linked executable file there are no relocations, all symbols have been resolved (this is a statically linked binary):
```console
$ readelf -r sample_exe

There are no relocations in this file.
```

## Linking against a shared library

Let's now create a shared library of out the `sum.o` object.

Shared libraries are PIC that can be placed anywhere in memory by the OS.
To do this, we need to compile `sum.c` with the `-fPIC` option.

```console
$ readelf -h libsum.so
ELF Header:
  ...
  Type:                              DYN (Shared object file)
  ...
```

The new shared library now exports *dynamic symbols* for other programs to link against *at runtime*. They are stored in a new section called `.dynsym`:
```console
$ readelf -s libsum.so

Symbol table '.dynsym' contains 9 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
   ...
     5: 0000000000001111    30 FUNC    GLOBAL DEFAULT   10 global_sum
     6: 00000000000010f9    24 FUNC    GLOBAL DEFAULT   10 sum
     7: 0000000000004008     4 OBJECT  GLOBAL DEFAULT   18 global1
     8: 000000000000400c     4 OBJECT  GLOBAL DEFAULT   18 global2
```

Being compiled as Position Independent Code (`-fPIC`), the library code does not know where in memory its code and data section will be placed.

So how does it know from where to read the global variables in the `global_sum()` function? By using relocations, of course!
```console
$ readelf -r libsum.so

Relocation section '.rela.dyn' at offset 0x330 contains 2 entries:
  Offset          Info           Type           Sym. Value    Sym. Name + Addend
000000003fd8  000400000006 R_X86_64_GLOB_DAT 0000000000004004 global2 + 0
000000003fe0  000300000006 R_X86_64_GLOB_DAT 0000000000004000 global1 + 0
```

The *shared library also needs to patch its code at runtime* based on where the `.data` section will be placed into memory.

We now see another relocation type: `R_X86_64_GLOB_DAT`. This relocation type is used where data is access indirectly through the **Global Offset Table** or GOT.

#### What is the GOT?

To be able to execute the program, the real address of the global variables needs to be replaced in the compiled code (the `.text` section).

But since the address can be different in each program, this implies that the `.text` section cannot not be shared anymore, which breaks the fundamental reason for building *shared libraries*.

So how can we resolve the address of global variables while keeping the `.text` section read only? This is where the *GOT* comes into play.
Instead of patching the `.text` section at runtime with the correct addresses, the source code will load the variable values indirectly via a section that is writable at runtime: the GOT.

The following diagram illustrates this concept:

![ELF GOT relocation](/assets/ELF_GOT_relocation.png)


At runtime the dynamic linker will populate the GOT entries with the real addresses only once and then the code will use the correct values.
This allows the `.text` section to remain shareable between multiple programs.

In our example, the `.got` section in the ELF file is at address `0x3fd8` and has 16 bytes (2 x 64bit addresses).
```console
  [10] .got              PROGBITS         0000000000003fd8  00002fd8
       0000000000000010  0000000000000008  WA       0     0     8
```

Disassembling the `global_sum()` functions reveals loading values from `0x3fe0` and `0x3fd8` (start of `.got` section):
```console
$ objdump -d libsum.so
...
0000000000001018 <global_sum>:
    1018:	f3 0f 1e fa          	endbr64
    101c:	55                   	push   %rbp
    101d:	48 89 e5             	mov    %rsp,%rbp
    1020:	48 8b 05 b9 2f 00 00 	mov    0x2fb9(%rip),%rax        # 3fe0 <global1-0x20>
    1027:	8b 10                	mov    (%rax),%edx
    1029:	48 8b 05 a8 2f 00 00 	mov    0x2fa8(%rip),%rax        # 3fd8 <global2-0x2c>
    1030:	8b 00                	mov    (%rax),%eax
    1032:	01 d0                	add    %edx,%eax
    1034:	5d                   	pop    %rbp
    1035:	c3                   	ret
```

We clearly see the indirect memory access:
- read the value from the GOT into register `%rax`
- read the value of the memory pointed to by `%rax` into `%edx` or `%eax`

Let's now turn our attention to the resulting executable:
```console
$ readelf -r sample_shared

Relocation section '.rela.dyn' at offset 0x338 contains 2 entries:
  Offset          Info           Type           Sym. Value    Sym. Name + Addend
000000403020  000300000005 R_X86_64_COPY     0000000000403020 global2 + 0
000000403024  000400000005 R_X86_64_COPY     0000000000403024 global1 + 0

Relocation section '.rela.plt' at offset 0x368 contains 2 entries:
  Offset          Info           Type           Sym. Value    Sym. Name + Addend
000000403000  000100000007 R_X86_64_JUMP_SLO 0000000000000000 sum + 0
000000403008  000200000007 R_X86_64_JUMP_SLO 0000000000000000 global_sum + 0
```

Here we see two other relocation types:
- `R_X86_64_COPY`: copy relocation
    - the linker allocates space for the variable in the .data section and copies the initial value from the shared library
    - after that, it references this address
- `R_X86_64_JUMP_SLOT`: it is a relocation type used for functions in dynamically linked programs
    - similar to resolving variable addresses via the GOT, we don't want to patch the `.text` section at runtime for function calls
    - the code in the `.text` section makes an indirect jump into a *Procedure Link Table* that will get the real function address from the GOT
    - allows lazy binding: the dynamic linker doesn't fill in all the addresses at the beginning, but only when the function is actually called

Let's look at the disassembled code:
```console
$ objdump -d sample_shared

sample_shared:     file format elf64-x86-64


Disassembly of section .plt:

0000000000401000 <sum@plt-0x10>:
  401000:	ff 35 ea 1f 00 00    	push   0x1fea(%rip)        # 402ff0 <_GLOBAL_OFFSET_TABLE_+0x8>
  401006:	ff 25 ec 1f 00 00    	jmp    *0x1fec(%rip)        # 402ff8 <_GLOBAL_OFFSET_TABLE_+0x10>
  40100c:	0f 1f 40 00          	nopl   0x0(%rax)

0000000000401010 <sum@plt>:
  401010:	ff 25 ea 1f 00 00    	jmp    *0x1fea(%rip)        # 403000 <sum>
  401016:	68 00 00 00 00       	push   $0x0
  40101b:	e9 e0 ff ff ff       	jmp    401000 <sum@plt-0x10>

0000000000401020 <global_sum@plt>:
  401020:	ff 25 e2 1f 00 00    	jmp    *0x1fe2(%rip)        # 403008 <global_sum>
  401026:	68 01 00 00 00       	push   $0x1
  40102b:	e9 d0 ff ff ff       	jmp    401000 <sum@plt-0x10>

Disassembly of section .text:

0000000000401030 <main>:
  401030:	f3 0f 1e fa          	endbr64
  401034:	55                   	push   %rbp
  401035:	48 89 e5             	mov    %rsp,%rbp
  401038:	48 83 ec 10          	sub    $0x10,%rsp
  40103c:	8b 15 de 1f 00 00    	mov    0x1fde(%rip),%edx        # 403020 <global2>
  401042:	8b 05 dc 1f 00 00    	mov    0x1fdc(%rip),%eax        # 403024 <global1>
  401048:	89 d6                	mov    %edx,%esi
  40104a:	89 c7                	mov    %eax,%edi
  40104c:	e8 bf ff ff ff       	call   401010 <sum@plt>
  401051:	89 45 fc             	mov    %eax,-0x4(%rbp)
  401054:	b8 00 00 00 00       	mov    $0x0,%eax
  401059:	e8 c2 ff ff ff       	call   401020 <global_sum@plt>
  40105e:	01 45 fc             	add    %eax,-0x4(%rbp)
  401061:	8b 45 fc             	mov    -0x4(%rbp),%eax
  401064:	c9                   	leave
  401065:	c3                   	ret
  ...
```
When calling the `sum()` symbol, the code actually calls `<sum@plt>` at address `0x401010`.

Let's see what happens in this PLT entry:
- `jmp    *0x1fea(%rip)        # 403000 <sum>` which jumps to the address stored at location 0x403000
- if we look with gdb at the initial value of this address we see that it contains the address of the next asm instruction in `sum@plt`:
 ```console
 (gdb) x/gx 0x403000
0x403000 <sum@got.plt>: 0x0000000000401016
```
- so this first jump is basically a no-op
- the next instruction pushes a relocation index onto the stack: 0 for sum, 1 for global_sum
- jump to `jmp    401000 <sum@plt-0x10>` transfers control to the dynamic linker to resolve the symbol
- after this, if we look again at the memory address 0x403000, we see the resolved entry with the actual address of the function:

```console
Breakpoint 1, main () at main.c:9
9	    int res1 = sum(global1, global2);

(gdb) x/gx 0x403000
0x403000 <sum@got.plt>:	0x0000000000401016  # next instruction in the PLT entry

(gdb) next
10		res1 += global_sum();

(gdb) x/gx 0x403000
0x403000 <sum@got.plt>:	0x00007ffff7fb9000  # actual address of sum() in memory

(gdb) p &sum
$1 = (int (*)(int, int)) 0x7ffff7fb9000 <sum>   # matches the address printed by gdb
```

- at this point, the next entry in the GOT is the address for `global_sum()`; exactly the same flow applies here also: the PLT entry calls the dynamic linker to update the GOT with the real address of the function

```console
(gdb)  x/gx 0x403008
0x403008 <global_sum@got.plt>:	0x0000000000401026

(gdb) next
11		return res1;

(gdb)  x/gx 0x403008
0x403008 <global_sum@got.plt>:	0x00007ffff7fb9018

(gdb) p &global_sum
$3 = (int (*)(void)) 0x7ffff7fb9018 <global_sum>
```

- on the next calls to `sum()` and `global_sum()` the first instruction in the corresponding PLT entry will directly jump to the real function address

The following diagram illustrates this concept:

![ELF PLT relocation](/assets/ELF_PLT_relocation.png)

Let us now summarize the relocation types we encountered so far:

| Relocation type  | Description |
| -----------   | ----------- |
| R_X86_64_PC32 | Used at link time when stitching together multiple object files, relocation relative to PC |
| R_X86_64_COPY | Used at runtime, copy the initial variable value from a shared library in the process's address space |
| R_X86_64_PLT32 | Used at link time to decide which PLT to call instead of a real function |
| R_X86_64_JUMP_SLOT | Relocation for dynamically linked symbols, uses PLT at runtime to update an entry in the GOT with the real function address |

