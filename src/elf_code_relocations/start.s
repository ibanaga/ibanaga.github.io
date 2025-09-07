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

