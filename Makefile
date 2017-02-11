CC=cc
CV=-std=c11 -D_POSIX_C_SOURCE=201112L
CFLAGS=-g -Os -W -Wall

.PHONY: clean test

shi: shi.c vendor/*.c
	$(CC) $(CFLAGS) shi.c vendor/linenoise.c vendor/pcg_basic.c -o shi

clean:
	rm -f shi *~

test: shi
	@./test.sh

format:
	clang-format shi.c >shi.c.new
	mv shi.c.new shi.c
