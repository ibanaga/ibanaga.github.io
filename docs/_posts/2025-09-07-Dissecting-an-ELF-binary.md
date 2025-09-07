---
layout: post
title: "Dissecting an ELF executable"
date: 2025-09-07
categories: ELF compilers
---
## Introduction

As the name suggests, an *Executable and Linkable Format* describes either:
 -  an *executable* that can be run by the Operating System
  - a file that can be *linked by other programs* (an object file or a library)

ELF uses a collection of headers and links between these headers to describe a binary file.

![ELF basic format](/assets/ELF_basic_format.png)

The *executable format* describes which *segments* from the ELF file shall be *loaded into memory*:
   - at which address
   - the allowed permissions.

The *linkable format* contains a list of *sections* for the linker to know where to find the code and data sections.

As can be seen from the diagram above, these two formats don’t overlap:
   - when describing an executable, the section table is optional and can be skipped
       - but usually the executable also contains information about the sections from which the loadable segments have been generated
   - when describing a linkable object, there are no program headers

The most important sections in an ELF file are:

| Section name  | Description |
| -----------   | ----------- |
| `.text`   | The executable machine code |
| `.data`   | The global variables |
| `.bss`  | The global variables uninitialized or initialized with 0  |
| `.dynsym`   | The list of symbols exported for other programs to use  |

The `readelf` tool can display information about an ELF file:

| Command  | Description |
| -----------   | ----------- |
| `readelf -h <binary>`   | Show the main ELF header of the file |
| `readelf -l <binary>`   | List the program headers |
| `readelf -S <binary>`   | List the section headers  |
| `readelf -s <binary>`   | List the symbols in the ELF file |


## Practical examples

Let’s compile a ridiculous `main.c` source code file into a statically linked executable.

```c
int global1 = 10;
int global2 = 20;
char global_array[512];

int sum(int a, int b) {
        return a + b;
}

int main() {
        return sum(global1, global2);
}
```

The straightforward way of compiling (`gcc -c main.c -o sample_exe`) will link against the `glibc` which will bring in a lot of code and data that is not relevant for our goal, it will just complicate things.

So let's build this simple source code file and not link it against the standard library:
    - `-nostdlib` instructs the toolchain not to link against the standard library

```console
$ gcc -c -nostdlib main.c -o main.o
$ ld -o sample_exe main.o
ld: warning: cannot find entry symbol _start; defaulting to 0000000000401000
```

 The commands succeeded despite warning. Let's try to run our newly built executable:
 ```console
 $ ./sample_exe
Segmentation fault (core dumped)
 ```

Ooops...something has gone wrong...

The issue is that the linker expects to find a symbol called `_start` as the entry point to this executable.
`glibc` provides this logic by default, but since we specified `-nostdlib` we are on our own here.

We can define a trivial `_start` logic in assembly:
   - the kernel has set up the stack pointer already, so we can call C functions
   - the C `main()` function is invoked
   - the *return address* of `main()` is passed as an argument to the `exit` system call
   - syscall `exit` is called to terminate the program

```
# start.s
.global _start
.extern main

_start:
    call main       # call C function main()

    # return value from main() is in %eax
    mov %eax, %edi  # main()'s return value is the argument to the syscall
                    # syscall args are: %rdi, %rsi, %rdx, %r10, %r8, %r9
                    # so put %eax in %rdi 
    mov $60, %eax   # syscall exit has numeric value 60
    syscall
```

We can now use the assembler to generate an object file...
```console
$ as -o start.o start.s
```

And then the linker to stitch `main.o` and `start.o` together into an executable:
```console
$ ld -o sample_exe main.o start.o
ld: warning: start.o: missing .note.GNU-stack section implies executable stack
ld: NOTE: This behavior is deprecated and will be removed in a future version of the linker
$ file main
main: ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked, not stripped
```

The flow worked fine, but the warning looks serious, an executable stack is not a good thing (we'll fix this later...).

A quick sanity check on the resulting binary...
```console
$ ./main
$ echo $?
30
```
... looks ok, exit code 30 is the expected one. We can now proceed to inspect the binary.


### The ELF header

```console
$ readelf -h sample_exe 
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00 
  Class:                             ELF64
  Data:                              2's complement, little endian
  Version:                           1 (current)
  OS/ABI:                            UNIX - System V
  ABI Version:                       0
  Type:                              EXEC (Executable file)
  Machine:                           Advanced Micro Devices X86-64
  Version:                           0x1
  Entry point address:               0x401037
  Start of program headers:          64 (bytes into file)
  Start of section headers:          12744 (bytes into file)
  Flags:                             0x0
  Size of this header:               64 (bytes)
  Size of program headers:           56 (bytes)
  Number of program headers:         5
  Size of section headers:           64 (bytes)
  Number of section headers:         9
  Section header string table index: 8
```

The ELF header gives us general information about the binary we’re looking at:
- it is compiled for an X86-64 machine using Unix ABI (Application Binary Interface)
- it is a statically linked executable (as opposed to a Position Independent Executable) 
- its entry point address is `0x401047`; this is where the first instruction of the program will be executed from memory (the `_start` symbol)
- it has 5 program headers and 9 section headers

### The ELF sections

```console
$ readelf -S sample_exe
There are 9 section headers, starting at offset 0x31c8:

Section Headers:
  [Nr] Name              Type             Address           Offset
       Size              EntSize          Flags  Link  Info  Align
  [ 0]                   NULL             0000000000000000  00000000
       0000000000000000  0000000000000000           0     0     0
  [ 1] .text             PROGBITS         0000000000401000  00001000
       0000000000000045  0000000000000000  AX       0     0     1
  [ 2] .eh_frame         PROGBITS         0000000000402000  00002000
       0000000000000058  0000000000000000   A       0     0     8
  [ 3] .data             PROGBITS         0000000000403000  00003000
       0000000000000008  0000000000000000  WA       0     0     4
  [ 4] .bss              NOBITS           0000000000403020  00003008
       0000000000000200  0000000000000000  WA       0     0     32
  [ 5] .comment          PROGBITS         0000000000000000  00003008
       000000000000002b  0000000000000001  MS       0     0     1
  [ 6] .symtab           SYMTAB           0000000000000000  00003038
       0000000000000108  0000000000000018           7     2     8
  [ 7] .strtab           STRTAB           0000000000000000  00003140
       0000000000000046  0000000000000000           0     0     1
  [ 8] .shstrtab         STRTAB           0000000000000000  00003186
       000000000000003f  0000000000000000           0     0     1
Key to Flags:
  W (write), A (alloc), X (execute), M (merge), S (strings), I (info),
  L (link order), O (extra OS processing required), G (group), T (TLS),
  C (compressed), x (unknown), o (OS specific), E (exclude),
  D (mbind), l (large), p (processor specific)
```

The first section is by design a special `null section` that contains no data.

Next, we see the expected `.text`, `.data` and `.bss` section, plus a few more.

| Section  | Description |
| -----------   | ----------- |
| `.text`   | Compiled code takes 69 bytes |
| `.eh_frame`   | Exception handling frame, used for stack unwinding. Can be ignored for now |
| `.data`   | 8 bytes as expected since we have 2 `int`s  |
| `.bss`   | 512 bytes as expected for the uninitialized array |
| `.comment`   | Some information about the toolchain that was automatically put it |
| `.symtab`   | Describes the symbols in this file |
| `.strtab`   | String table that holds a list of null separated strings for symbol names |
| `.shstrtab`   | String table that a list of null separated strings for section names |

We can inspect the symbols in this file via `readelf`:

```console
$ readelf -s sample_exe 

Symbol table '.symtab' contains 11 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND 
     1: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS main.c
     2: 0000000000403004     4 OBJECT  GLOBAL DEFAULT    3 global2
     3: 0000000000401000    24 FUNC    GLOBAL DEFAULT    1 sum
     4: 0000000000403000     4 OBJECT  GLOBAL DEFAULT    3 global1
     5: 0000000000401037     0 NOTYPE  GLOBAL DEFAULT    1 _start
     6: 0000000000403020     0 NOTYPE  GLOBAL DEFAULT    4 __bss_start
     7: 0000000000401018    31 FUNC    GLOBAL DEFAULT    1 main
     8: 0000000000403008     0 NOTYPE  GLOBAL DEFAULT    3 _edata
     9: 0000000000403220     0 NOTYPE  GLOBAL DEFAULT    4 _end
    10: 0000000000403020   512 OBJECT  GLOBAL DEFAULT    4 global_array
```

We see the names we defined in the source code plus some extra symbols:
- `OBJECT` type: our global variables
- `FUNC` type: our 2 functions, `sum()` and `main()`
- `NOTYPE` type: linker defined symbols
  - `__bss_start`: start of the `.bss` section
  - `_edata`: end of the `.data` section
  - `_end`: last address of this program

The linker defined symbols can be accessed in the running code to get the addresses of the different locations in memory.

We also see that the address of the symbol `_start` matches the *entry point address* reported in the main ELF header: `0x401037`.

### The ELF program segments

```console
$ readelf -l sample_exe 

Elf file type is EXEC (Executable file)
Entry point 0x401037
There are 5 program headers, starting at offset 64

Program Headers:
  Type           Offset             VirtAddr           PhysAddr
                 FileSiz            MemSiz              Flags  Align
  LOAD           0x0000000000000000 0x0000000000400000 0x0000000000400000
                 0x0000000000000158 0x0000000000000158  R      0x1000
  LOAD           0x0000000000001000 0x0000000000401000 0x0000000000401000
                 0x0000000000000045 0x0000000000000045  R E    0x1000
  LOAD           0x0000000000002000 0x0000000000402000 0x0000000000402000
                 0x0000000000000058 0x0000000000000058  R      0x1000
  LOAD           0x0000000000003000 0x0000000000403000 0x0000000000403000
                 0x0000000000000008 0x0000000000000220  RW     0x1000
  GNU_STACK      0x0000000000000000 0x0000000000000000 0x0000000000000000
                 0x0000000000000000 0x0000000000000000  RWE    0x10

 Section to Segment mapping:
  Segment Sections...
   00     
   01     .text 
   02     .eh_frame 
   03     .data .bss 
   04     

```

We see that this binary has 4 loadable segments:
- from file offset 0 to virtual address 0x400000, 344 bytes (0x158), read only
  - maps the ELF headers into memory
- from file offset 0x1000 to virtual address 0x401000, 69 bytes (0x45), read only and executable
  - this segment is executable, so it must be the compiled code
- from file offset 0x2000 to virtual address 0x402000, 58 bytes, read only
- from file offset 0x3000 to virtual address 0x403000, 544byes read write
  - notice how the *FileSize* field is 8 while the *MemSize* is 544
  - this makes sense, since our program contains 2 ints that are initialized and must be read from the binary into memory, and we also have 512 bytes of uninitialized data, summing up to 520 bytes
  - but why is the segment length 544 bytes then?
    - in the section headers we see that the `.bss` section is aligned at a 32bytes boundary
    - since we have only 8 byes of `.data`, it cannot start right after it; its address is rounded up to the next 32bytes
    - so we have 24 extra bytes due to alignment requirements
- the last segment is not a loadable segment, but a special one which tells the OS loader is the stack is executable or not
  - RWE marks the stack as executable, which is not safe; this is why the toolchain warned us above when we linked our object code
  - to make the stack non-executable `-z noexecstack` must be passed to the linker:
  ```console
  $ ld -z noexecstack -o sample_exe main.o start.o
  $ readelf -l sample_exe | grep GNU_STACK -A 1
  GNU_STACK      0x0000000000000000 0x0000000000000000 0x0000000000000000
                 0x0000000000000000 0x0000000000000000  RW     0x10
  ```
  - the *Executable bit* is now gone, and the linker doesn't complain anymore


### Dissecting the binary

We can use `objdump` to disassembly our binary:

```console
$ objdump -d sample_exe 

sample_exe:     file format elf64-x86-64


Disassembly of section .text:

0000000000401000 <sum>:
  401000:       f3 0f 1e fa             endbr64
  401004:       55                      push   %rbp
  401005:       48 89 e5                mov    %rsp,%rbp
  401008:       89 7d fc                mov    %edi,-0x4(%rbp)
  40100b:       89 75 f8                mov    %esi,-0x8(%rbp)
  40100e:       8b 55 fc                mov    -0x4(%rbp),%edx
  401011:       8b 45 f8                mov    -0x8(%rbp),%eax
  401014:       01 d0                   add    %edx,%eax
  401016:       5d                      pop    %rbp
  401017:       c3                      ret

0000000000401018 <main>:
  401018:       f3 0f 1e fa             endbr64
  40101c:       55                      push   %rbp
  40101d:       48 89 e5                mov    %rsp,%rbp
  401020:       8b 15 de 1f 00 00       mov    0x1fde(%rip),%edx        # 403004 <global2>
  401026:       8b 05 d4 1f 00 00       mov    0x1fd4(%rip),%eax        # 403000 <global1>
  40102c:       89 d6                   mov    %edx,%esi
  40102e:       89 c7                   mov    %eax,%edi
  401030:       e8 cb ff ff ff          call   401000 <sum>
  401035:       5d                      pop    %rbp
  401036:       c3                      ret

0000000000401037 <_start>:
  401037:       e8 dc ff ff ff          call   401018 <main>
  40103c:       89 c7                   mov    %eax,%edi
  40103e:       b8 3c 00 00 00          mov    $0x3c,%eax
  401043:       0f 05                   syscall
```

We notice that the first instruction is at `0x401000` which matches the second loadable segment in the program header. That segments tell the OS loader to map from offset `0x1000` in the file.
Let's confirm we have the expected data in the binary file using `hexdump`.
```console
$ hexdump -C sample_exe | grep "0001000" -A 1
00001000  f3 0f 1e fa 55 48 89 e5  89 7d fc 89 75 f8 8b 55  |....UH...}..u..U|
00001010  fc 8b 45 f8 01 d0 5d c3  f3 0f 1e fa 55 48 89 e5  |..E...].....UH..|
```
Great, we see the expected instruction bytes: `f3 0f 1e fa`....

The data section should start at offset `0x3000` according to the third entry program table:
```console
$ hexdump -C sample_exe | grep "0003000"
00003000  0a 00 00 00 14 00 00 00  47 43 43 3a 20 28 55 62  |........GCC: (Ub|
```
We see the values of our global variables: 10 and 20 (0x0a and 0x14).

Let's now see how a string table looks like in binary.
We know from the ELF header that:
- *Start of section headers: 12744 (bytes into file)*
- *Section header string table index: 8*

Each entry in the Section table has the following structure:
```c
typedef struct {
    uint32_t sh_name;      // Offset into .shstrtab (name of section)
    uint32_t sh_type;      // Section type (SHT_PROGBITS, SHT_SYMTAB, etc.)
    uint64_t sh_flags;     // Flags (writable, alloc, executable)
    uint64_t sh_addr;      // Virtual address in memory (if loaded)
    uint64_t sh_offset;    // File offset of section contents
    uint64_t sh_size;      // Size in bytes of section contents
    uint32_t sh_link;      // Section index link (depends on type)
    uint32_t sh_info;      // Extra info (depends on type)
    uint64_t sh_addralign; // Alignment requirement
    uint64_t sh_entsize;   // Entry size (for tables like .symtab)
} Elf64_Shdr;
```

Our entry should be at offset 12744 + 8 * sizeof(Elf64_Shdr) = 13256 (0x33c8).
```console
$ hexdump -C sample_exe | grep "00033c0" -A 3
000033c0  00 00 00 00 00 00 00 00  11 00 00 00 03 00 00 00  |................|
000033d0  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
000033e0  86 31 00 00 00 00 00 00  3f 00 00 00 00 00 00 00  |.1......?.......|
000033f0  00 00 00 00 00 00 00 00  01 00 00 00 00 00 00 00  |................|
```
The address found in the binary is: 0x3186. Let's look at it:
```console
$ hexdump -C sample_exe | grep "0003180" -A 4
00003180  61 72 72 61 79 00 00 2e  73 79 6d 74 61 62 00 2e  |array...symtab..|
00003190  73 74 72 74 61 62 00 2e  73 68 73 74 72 74 61 62  |strtab..shstrtab|
000031a0  00 2e 74 65 78 74 00 2e  65 68 5f 66 72 61 6d 65  |..text..eh_frame|
000031b0  00 2e 64 61 74 61 00 2e  62 73 73 00 2e 63 6f 6d  |..data..bss..com|
000031c0  6d 65 6e 74 00 00 00 00  00 00 00 00 00 00 00 00  |ment............|
```
As expected, we find the null-terminated strings for all the 8 section names.


Ok, we went the hard way just for fun, to do some more binary dissection. The section's info reported by `readelf -S` also gives us this offset in the last column:
```console
  [ 8] .shstrtab         STRTAB           0000000000000000  00003186
       000000000000003f  0000000000000000           0     0     1
```


In the end of this post, let's create a diagram with everything we discovered so far:


![ELF binary to RAM mapping](/assets/ELF_binary_ram_mapping.png)
