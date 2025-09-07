all:
	gcc -c -o main.o main.c
	as -o start.o start.s
	ld -z noexecstack -o sample_exe main.o start.o

clean:
	rm main.o start.o sample_exe
