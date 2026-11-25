# ==============================================================================
# SRR: Synchronous Send / Receive / Reply Message Passing Library
# ==============================================================================

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude
AR      ?= ar

WIN_CC  ?= x86_64-w64-mingw32-gcc
WIN_AR  ?= x86_64-w64-mingw32-ar

SRC     := src/srr_pthread.c src/srr_ipc.c
OBJ     := $(SRC:.c=.o)
LIB     := libsrr.a

TEST_SRCS := tests/test_srr_basic.c tests/test_srr_races.c tests/test_srr_stress.c tests/test_srr_ipc.c
TEST_BINS := $(TEST_SRCS:.c=)

# Default target displays help
.DEFAULT_GOAL := help
.PHONY: help all lib examples beam win32 test test-valgrind test-adapters test-all bench clean

# ------------------------------------------------------------------------------
# Help Target
# ------------------------------------------------------------------------------

help:
	@echo "================================================================================"
	@echo " SRR Build System - Available Targets"
	@echo "================================================================================"
	@echo ""
	@echo "  Build Targets:"
	@echo "    make all             Build core library (libsrr.a), examples, and BEAM port driver"
	@echo "    make lib             Build static library (libsrr.a)"
	@echo "    make examples        Build synthesizer engine server & CLI (examples/synth_engine/)"
	@echo "    make beam            Build BEAM port driver (adapters/beam/srr_port_driver)"
	@echo "    make win32           Cross-compile Windows library (libsrr_win32.a) and tests (MinGW)"
	@echo ""
	@echo "  Testing & Quality Assurance:"
	@echo "    make test            Build and execute all native test suites"
	@echo "    make test-valgrind   Run all test suites under Valgrind (zero leak/error check)"
	@echo "    make test-adapters   Run BEAM and WebAudio browser gateway integration tests"
	@echo "    make test-all        Execute complete verification (test-valgrind + test-adapters)"
	@echo ""
	@echo "  Performance:"
	@echo "    make bench           Build with -O3 and execute nanosecond microbenchmark suite"
	@echo ""
	@echo "  Maintenance:"
	@echo "    make clean           Remove all object files, libraries, and binaries"
	@echo "    make help            Show this help reference"
	@echo "================================================================================"

# ------------------------------------------------------------------------------
# Core Build Recipes
# ------------------------------------------------------------------------------

all: lib examples beam

lib: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------------------
# Examples & Adapters
# ------------------------------------------------------------------------------

examples: $(LIB)
	$(CC) $(CFLAGS) examples/synth_engine/synth_server.c -L. -lsrr -o examples/synth_engine/synth_server
	$(CC) $(CFLAGS) examples/synth_engine/synth_cli.c -L. -lsrr -o examples/synth_engine/synth_cli

beam: $(LIB)
	$(CC) $(CFLAGS) adapters/beam/src/srr_port_driver.c -L. -lsrr -o adapters/beam/srr_port_driver

# ------------------------------------------------------------------------------
# Windows MinGW Cross-Compilation
# ------------------------------------------------------------------------------

win32:
	@echo "--- Cross-compiling for Windows (x86_64-w64-mingw32) ---"
	$(WIN_CC) $(CFLAGS) -c src/srr_pthread.c -o src/srr_pthread.win.o
	$(WIN_CC) $(CFLAGS) -c src/srr_win32.c -o src/srr_win32.win.o
	$(WIN_AR) rcs libsrr_win32.a src/srr_pthread.win.o src/srr_win32.win.o
	$(WIN_CC) $(CFLAGS) tests/test_srr_basic.c -L. -lsrr_win32 -o tests/test_srr_basic.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_races.c -L. -lsrr_win32 -o tests/test_srr_races.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_stress.c -L. -lsrr_win32 -o tests/test_srr_stress.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_win32_pipe.c -L. -lsrr_win32 -o tests/test_srr_win32_pipe.exe
	@echo "Windows build completed successfully: libsrr_win32.a"

# ------------------------------------------------------------------------------
# Testing & Verification
# ------------------------------------------------------------------------------

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lsrr -o $@

test: $(LIB) $(TEST_BINS)
	@echo "--- Running Native Test Suites ---"
	./tests/test_srr_basic
	./tests/test_srr_races
	./tests/test_srr_stress
	./tests/test_srr_ipc
	@echo "All native tests passed!"

test-valgrind: $(LIB) $(TEST_BINS)
	@echo "--- Running Valgrind Leak & Memory Checks ---"
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_basic
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_races
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_stress
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_ipc
	@echo "All Valgrind checks passed with 0 errors and 0 leaks!"

test-adapters: examples beam
	@echo "--- Running BEAM & WebAudio Gateway Integration Tests ---"
	python3 adapters/beam/test/test_beam_adapter.py
	python3 gateway/test_gateway.py
	@echo "All adapter integration tests passed!"

test-all: test-valgrind test-adapters
	@echo "================================================================================"
	@echo " ALL VERIFICATION SUITES (NATIVE, VALGRIND, ADAPTERS) PASSED WITH ZERO ERRORS!"
	@echo "================================================================================"

# ------------------------------------------------------------------------------
# Benchmarks
# ------------------------------------------------------------------------------

bench: clean
	@echo "--- Building Benchmark Suite with -O3 ---"
	$(MAKE) CFLAGS="$(CFLAGS) -O3" benchmarks/bench_srr
	./benchmarks/bench_srr

benchmarks/%: benchmarks/%.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lsrr -o $@

# ------------------------------------------------------------------------------
# Clean
# ------------------------------------------------------------------------------

clean:
	rm -f src/*.o src/*.win.o tests/test_srr_basic tests/test_srr_races tests/test_srr_stress tests/test_srr_ipc tests/*.exe
	rm -f benchmarks/bench_srr benchmarks/*.exe
	rm -f examples/synth_engine/synth_server examples/synth_engine/synth_cli
	rm -f adapters/beam/srr_port_driver
	rm -f $(LIB) libsrr_win32.a vgcore.* core.* *.log
	@echo "Clean completed."
