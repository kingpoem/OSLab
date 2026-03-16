CC = x86_64-elf-gcc
LD = x86_64-elf-ld
# Automatically generate lists of sources using wildcard.
C_SOURCES = $(wildcard *.c)
HEADERS = $(wildcard *.h)
CFLAGS = -m32 -g -ffreestanding -fno-pic -nostdlib -nodefaultlibs -Wall -O0

# Convert *.c filenames to *.o as a list of object files to build.
OBJ = ${C_SOURCES:.c=.o}

# Clean, build and run.
all: clean os-image run

# Removes all built files.
clean:
	rm -f *.bin
	rm -f *.bin *.o
	rm -f os-image

# Kernel code --------------------------------------------------------------------------------------

# Create kernel binary by linking kernel entry code to kernel_main.
kernel.bin: enter_kernel.o stdio.o kernel.o
	$(LD) -melf_i386 -o $@ -Ttext 0x1000 --oformat binary --entry kernel_main $^

# Create executable image by prepending kernel binary with boot sector.
os-image: boot.bin kernel.bin
	cat $^ > $@

# Compile assembly sources into binary.
%.bin: %.asm
	nasm $< -f bin -o $@

# Assemble the kernel entry to an object file.
%.o: %.asm
	nasm $< -f elf -o $@

# Compile C sources into object files.
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# --------------------------------------------------------------------------------------------------

# Run operating system on emulated x86.
run: os-image
	qemu-system-x86_64 -drive file=os-image,format=raw -net none
