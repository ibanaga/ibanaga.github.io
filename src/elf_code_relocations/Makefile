CFLAGS=-fno-asynchronous-unwind-tables -g
DYNAMIC_LINKER=--dynamic-linker /lib64/ld-linux-x86-64.so.2
all:
	gcc -c ${CFLAGS} -o main.o main.c
	gcc -c ${CFLAGS} -o sum.o sum.c
	gcc -c ${CFLAGS} -fPIC -o sum_shared.o sum.c
	as -o start.o start.s
	ld -shared -o libsum.so sum_shared.o
	ld -z noexecstack -o sample_exe main.o start.o sum.o
	ld -z noexecstack -rpath=. ${DYNAMIC_LINKER} -o sample_shared main.o start.o libsum.so

clean:
	rm -f main.o start.o sample_exe libsum.so sample_shared sum.o sum_shared.o
