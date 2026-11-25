CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude
AR ?= ar

SRC = src/srr_pthread.c src/srr_ipc.c
OBJ = $(SRC:.c=.o)
LIB = libsrr.a

TEST_SRCS = tests/test_srr_basic.c tests/test_srr_races.c tests/test_srr_stress.c tests/test_srr_ipc.c
TEST_BINS = $(TEST_SRCS:.c=)

.PHONY: all clean test test-valgrind bench examples beam

all: $(LIB) examples beam

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

benchmarks/%: benchmarks/%.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lsrr -o $@

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lsrr -o $@

examples/%: examples/synth_engine/%.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lsrr -o $@

examples: $(LIB)
	$(CC) $(CFLAGS) examples/synth_engine/synth_server.c -L. -lsrr -o examples/synth_engine/synth_server
	$(CC) $(CFLAGS) examples/synth_engine/synth_cli.c -L. -lsrr -o examples/synth_engine/synth_cli

beam: $(LIB)
	$(CC) $(CFLAGS) adapters/beam/src/srr_port_driver.c -L. -lsrr -o adapters/beam/srr_port_driver

win32:
	x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude -c src/srr_pthread.c -o src/srr_pthread.win.o
	x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude -c src/srr_win32.c -o src/srr_win32.win.o
	x86_64-w64-mingw32-ar rcs libsrr_win32.a src/srr_pthread.win.o src/srr_win32.win.o
	x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude tests/test_srr_basic.c -L. -lsrr_win32 -o tests/test_srr_basic.exe
	x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude tests/test_srr_races.c -L. -lsrr_win32 -o tests/test_srr_races.exe
	x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude tests/test_srr_stress.c -L. -lsrr_win32 -o tests/test_srr_stress.exe
	x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude tests/test_srr_win32_pipe.c -L. -lsrr_win32 -o tests/test_srr_win32_pipe.exe

bench: clean
	$(MAKE) CFLAGS="$(CFLAGS) -O3" benchmarks/bench_srr
	./benchmarks/bench_srr

test: $(LIB) $(TEST_BINS)
	@echo "--- Running All Tests ---"
	./tests/test_srr_basic
	./tests/test_srr_races
	./tests/test_srr_stress
	./tests/test_srr_ipc

test-valgrind: $(LIB) $(TEST_BINS)
	@echo "--- Running Valgrind Leak & Memory Checks ---"
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_basic
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_races
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_stress
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_ipc

clean:
	rm -f src/*.o tests/test_srr_basic tests/test_srr_races tests/test_srr_stress tests/test_srr_ipc benchmarks/bench_srr examples/synth_engine/synth_server examples/synth_engine/synth_cli adapters/beam/srr_port_driver $(LIB)
