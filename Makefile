# ==============================================================================
# SRR: Synchronous Send / Receive / Reply Message Passing Library
# ==============================================================================

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -pedantic -std=c99 -pthread -Iinclude/srr -Iinclude
AR      ?= ar

WIN_CC  ?= x86_64-w64-mingw32-gcc
WIN_AR  ?= x86_64-w64-mingw32-ar

SRC     := src/srr_pthread.c src/srr_ipc.c src/srr_wire.c src/srr_serial.c src/srr_embedded.c src/srr_tiny.c src/srr_esp32.c src/srr_p1.c src/srr_udp.c src/srr_resolver.c
OBJ     := $(SRC:.c=.o)
LIB     := libsrr.a

TEST_SRCS := tests/test_srr_basic.c tests/test_srr_races.c tests/test_srr_stress.c tests/test_srr_ipc.c tests/test_srr_wire.c tests/test_srr_embedded.c tests/test_srr_serial.c tests/test_srr_tiny.c tests/test_srr_esp32.c tests/test_srr_p1.c tests/test_srr_udp.c tests/test_srr_resolver.c tests/test_srr_audit_regressions.c tests/test_srr_scatter_gather.c tests/test_srr_sid.c
TEST_BINS := $(TEST_SRCS:.c=)

# Default target displays help
.DEFAULT_GOAL := help
.PHONY: help all lib shared examples tools beam test-beam win32 avr pico test test-asan test-tsan test-ubsan test-valgrind test-conformance test-adapters test-scripts test-all bench clean

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
	@echo "    make shared          Build dynamic shared library (libsrr.so) for scripting FFI"
	@echo "    make examples        Build synthesizer engine & Pico host/device examples"
	@echo "    make beam            Build BEAM port driver (adapters/beam/srr_port_driver)"
	@echo "    make win32           Cross-compile Windows library (libsrr_win32.a) and tests (MinGW)"
	@echo ""
	@echo "  Testing & Quality Assurance:"
	@echo "    make test            Build and execute all native test suites"
	@echo "    make test-valgrind   Run all test suites under Valgrind (zero leak/error check)"
	@echo "    make test-beam       Run BEAM (Erlang, Elixir, Gleam) integration tests"
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

all: lib examples tools beam

lib: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

shared: $(SRC)
	$(CC) $(CFLAGS) -fPIC -shared $(SRC) -o libsrr.so

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------------------
# Examples, Tools & Adapters
# ------------------------------------------------------------------------------

examples: $(LIB)
	$(CC) $(CFLAGS) examples/synth_engine/synth_server.c $(LIB) -o examples/synth_engine/synth_server
	$(CC) $(CFLAGS) examples/synth_engine/synth_cli.c $(LIB) -o examples/synth_engine/synth_cli
	$(CC) $(CFLAGS) examples/pico/pico_device_sim.c $(LIB) -o examples/pico/pico_device_sim
	$(CC) $(CFLAGS) examples/pico/pico_host_cli.c $(LIB) -o examples/pico/pico_host_cli
	$(CC) $(CFLAGS) examples/example_scatter_gather.c $(LIB) -o examples/example_scatter_gather
	$(CC) $(CFLAGS) examples/p1_sid/srr_sid_cli.c $(LIB) -o examples/p1_sid/srr_sid_cli
	$(CC) $(CFLAGS) examples/p1_sid/srr_p1_sid_sim.c $(LIB) -o examples/p1_sid/srr_p1_sid_sim

tools: $(LIB)
	$(CC) $(CFLAGS) tools/srr_cli.c $(LIB) -o tools/srr_cli
	$(CC) $(CFLAGS) tools/srr_resolver.c $(LIB) -o tools/srr_resolver

beam: $(LIB)
	$(CC) $(CFLAGS) adapters/beam/src/srr_port_driver.c $(LIB) -o adapters/beam/srr_port_driver
	@if command -v erlc >/dev/null 2>&1; then \
		echo "Compiling Erlang srr_client.erl..."; \
		erlc -o adapters/beam/src adapters/beam/src/srr_client.erl; \
	fi

# ------------------------------------------------------------------------------
# Windows MinGW Cross-Compilation
# ------------------------------------------------------------------------------

win32:
	@echo "--- Cross-compiling for Windows (x86_64-w64-mingw32) ---"
	$(WIN_CC) $(CFLAGS) -c src/srr_pthread.c -o src/srr_pthread.win.o
	$(WIN_CC) $(CFLAGS) -c src/srr_win32.c -o src/srr_win32.win.o
	$(WIN_CC) $(CFLAGS) -c src/srr_wire.c -o src/srr_wire.win.o
	$(WIN_CC) $(CFLAGS) -c src/srr_embedded.c -o src/srr_embedded.win.o
	$(WIN_AR) rcs libsrr_win32.a src/srr_pthread.win.o src/srr_win32.win.o src/srr_wire.win.o src/srr_embedded.win.o
	$(WIN_CC) $(CFLAGS) tests/test_srr_basic.c -L. -lsrr_win32 -o tests/test_srr_basic.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_races.c -L. -lsrr_win32 -o tests/test_srr_races.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_stress.c -L. -lsrr_win32 -o tests/test_srr_stress.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_win32_pipe.c -L. -lsrr_win32 -o tests/test_srr_win32_pipe.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_wire.c -L. -lsrr_win32 -o tests/test_srr_wire.exe
	$(WIN_CC) $(CFLAGS) tests/test_srr_embedded.c -L. -lsrr_win32 -o tests/test_srr_embedded.exe
	@echo "Windows build completed successfully: libsrr_win32.a"

# ------------------------------------------------------------------------------
# Embedded Cross-Compilation Targets
# ------------------------------------------------------------------------------

AVR_CC      ?= avr-gcc
AVR_SIZE    ?= avr-size
PICO_CC     ?= arm-none-eabi-gcc
PICO_SIZE   ?= arm-none-eabi-size

avr:
	@echo "--- Cross-compiling SRR Tiny for AVR ATmega328P ---"
	$(AVR_CC) -Wall -Wextra -Os -mmcu=atmega328p -DF_CPU=16000000UL -Iinclude/srr -Iinclude -c src/srr_wire.c -o src/srr_wire.avr.o
	$(AVR_CC) -Wall -Wextra -Os -mmcu=atmega328p -DF_CPU=16000000UL -Iinclude/srr -Iinclude -c src/srr_tiny.c -o src/srr_tiny.avr.o
	$(AVR_SIZE) --format=avr --mcu=atmega328p src/srr_wire.avr.o src/srr_tiny.avr.o
	@echo "AVR build completed successfully."

pico:
	@echo "--- Cross-compiling SRR for Raspberry Pi Pico (ARM Cortex-M0+) ---"
	$(PICO_CC) -Wall -Wextra -Os -mcpu=cortex-m0plus -mthumb -Iinclude/srr -Iinclude -c src/srr_wire.c -o src/srr_wire.pico.o
	$(PICO_CC) -Wall -Wextra -Os -mcpu=cortex-m0plus -mthumb -Iinclude/srr -Iinclude -c src/srr_embedded.c -o src/srr_embedded.pico.o
	$(PICO_SIZE) src/srr_wire.pico.o src/srr_embedded.pico.o
	@echo "Pico cross-build completed successfully."

# ------------------------------------------------------------------------------
# Testing & Verification
# ------------------------------------------------------------------------------

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) -o $@

test: $(LIB) $(TEST_BINS)
	@echo "--- Running Native Test Suites ---"
	./tests/test_srr_basic
	./tests/test_srr_races
	./tests/test_srr_stress
	./tests/test_srr_ipc
	./tests/test_srr_wire
	./tests/test_srr_embedded
	./tests/test_srr_serial
	./tests/test_srr_tiny
	./tests/test_srr_esp32
	./tests/test_srr_p1
	./tests/test_srr_udp
	./tests/test_srr_resolver
	./tests/test_srr_audit_regressions
	./tests/test_srr_scatter_gather
	./tests/test_srr_sid
	python3 tests/conformance/test_conformance.py
	@echo "All native tests passed!"

test-asan:
	@echo "--- Building and Running with AddressSanitizer & UndefinedBehaviorSanitizer ---"
	@$(MAKE) clean
	@set +e; \
	$(MAKE) CC="$(CC)" CFLAGS="$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g" test; \
	rc=$$?; \
	$(MAKE) clean; \
	clean_rc=$$?; \
	if [ $$rc -ne 0 ]; then exit $$rc; fi; \
	exit $$clean_rc

test-tsan:
	@echo "--- Building and Running with ThreadSanitizer ---"
	@$(MAKE) clean
	@set +e; \
	$(MAKE) CC="$(CC)" CFLAGS="$(CFLAGS) -fsanitize=thread -fno-omit-frame-pointer -g" test; \
	rc=$$?; \
	$(MAKE) clean; \
	clean_rc=$$?; \
	if [ $$rc -ne 0 ]; then exit $$rc; fi; \
	exit $$clean_rc

test-ubsan:
	@echo "--- Building and Running with UndefinedBehaviorSanitizer ---"
	@$(MAKE) clean
	@set +e; \
	$(MAKE) CC="$(CC)" CFLAGS="$(CFLAGS) -fsanitize=undefined -fno-omit-frame-pointer -g" test; \
	rc=$$?; \
	$(MAKE) clean; \
	clean_rc=$$?; \
	if [ $$rc -ne 0 ]; then exit $$rc; fi; \
	exit $$clean_rc

test-conformance: $(LIB)
	@echo "--- Running Protocol Conformance Test Vectors ---"
	python3 tests/conformance/test_conformance.py

test-valgrind: $(LIB) $(TEST_BINS)
	@echo "--- Running Valgrind Leak & Memory Checks ---"
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_basic
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_races
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_stress
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_ipc
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_wire
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_embedded
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_serial
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_tiny
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_esp32
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_p1
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_udp
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_resolver
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_audit_regressions
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_scatter_gather
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_srr_sid
	@echo "All Valgrind checks passed with 0 errors and 0 leaks!"

test-beam: examples beam
	@echo "--- Running BEAM (Erlang, Elixir, Gleam) Integration Tests ---"
	python3 adapters/beam/test/test_beam_adapter.py
	@if command -v mix >/dev/null 2>&1; then \
		echo "Compiling Elixir package..."; \
		(cd adapters/elixir && mix compile); \
	fi
	@if command -v gleam >/dev/null 2>&1; then \
		echo "Checking Gleam package..."; \
		(cd adapters/gleam && gleam check); \
	fi
	@echo "BEAM tests passed!"

test-adapters: examples beam
	@echo "--- Running BEAM & WebAudio Gateway Integration Tests ---"
	python3 adapters/beam/test/test_beam_adapter.py
	python3 gateway/test_gateway.py
	@echo "--- Running Native Language Adapters (Python, Perl, Go, Node, Ruby, PHP) ---"
	python3 adapters/test_native_adapters.py
	@echo "All adapter integration tests passed!"

test-scripts: tools examples
	@echo "--- Running Scripting & Automation Tests (Shell, Python, Perl, Tcl) ---"
	./examples/scripting/shell_automation.sh
	tclsh examples/scripting/srr_client.tcl
	@echo "Scripting tests passed!"

test-all: test-valgrind test-adapters test-scripts
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
	rm -f src/*.o src/*.win.o src/*.avr.o src/*.pico.o tests/test_srr_basic tests/test_srr_races tests/test_srr_stress tests/test_srr_ipc tests/test_srr_wire tests/test_srr_embedded tests/test_srr_serial tests/test_srr_tiny tests/test_srr_esp32 tests/test_srr_p1 tests/test_srr_udp tests/test_srr_resolver tests/test_srr_audit_regressions tests/test_srr_scatter_gather tests/test_srr_sid tests/conformance/test_c_vectors tests/conformance/generate_vectors tests/*.exe
	rm -f benchmarks/bench_srr benchmarks/*.exe
	rm -f examples/synth_engine/synth_server examples/synth_engine/synth_cli
	rm -f examples/pico/pico_device_sim examples/pico/pico_host_cli examples/example_scatter_gather
	rm -f examples/p1_sid/srr_sid_cli examples/p1_sid/srr_p1_sid_sim
	rm -f tools/srr_cli tools/srr_resolver
	rm -rf examples/go/bin
	rm -f adapters/beam/srr_port_driver
	rm -f $(LIB) libsrr.so libsrr_win32.a vgcore.* core.* *.log
	@echo "Clean completed."
