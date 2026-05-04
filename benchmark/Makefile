CC = gcc
CFLAGS = -L../ -lmy_vm -m32

all : test
test: ../my_vm.h
	$(CC) test.c $(CFLAGS) -o test

multi:
	$(CC) multi_test.c $(CFLAGS) -pthread -o test

example:
	$(CC) example.c $(CFLAGS) -o test

clean:
	rm -rf test
