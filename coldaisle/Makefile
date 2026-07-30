# coldaisle build. `make` builds the CPU simulator, `make test` runs the gates,
# `make cuda` builds the batched GPU driver (needs nvcc).
CC      ?= gcc
CFLAGS  ?= -O2 -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -I.
LDLIBS  ?= -lm
NVCC    ?= nvcc
# sm_86 is the 3090, sm_120 is Blackwell. Override for another card:
#   make cuda NVARCH=sm_90
NVARCH  ?= sm_86
NVFLAGS ?= -O2 -I. -arch=$(NVARCH)

HOST_SRC = host/dc_build.c host/dc_run.c host/dc_report.c
HOST_OBJ = $(HOST_SRC:.c=.o)
CORE_HDR = core/dc_defs.h core/dc_state.h core/dc_tick.h
TESTS    = tests/test_units tests/test_model tests/test_determinism

.PHONY: all test cuda clean
all: coldaisle

coldaisle: app/coldaisle.c $(HOST_OBJ) $(CORE_HDR) host/dc_host.h
	$(CC) $(CFLAGS) app/coldaisle.c $(HOST_OBJ) -o $@ $(LDLIBS)

%.o: %.c $(CORE_HDR) host/dc_host.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/%: tests/%.c $(HOST_OBJ) $(CORE_HDR) host/dc_host.h
	$(CC) $(CFLAGS) $< $(HOST_OBJ) -o $@ $(LDLIBS)

# The determinism contract starts with "no floating point in core/", so the
# gate greps the preprocessed core rather than trusting review.
test: coldaisle $(TESTS)
	@bash tests/no_float.sh
	@tests/test_units
	@tests/test_model
	@tests/test_determinism
	@for bp in blueprints/*.dcb; do \
	  ./coldaisle run $$bp --ticks 200 --quiet || exit 1; \
	done
	@echo "ok: every blueprint builds, runs and conserves requests"
	@if command -v $(NVCC) >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory tests/test_parity && tests/test_parity; \
	else \
	  echo "skip: test_parity (no nvcc)"; \
	fi
	@echo "coldaisle: all gates PASS"

cuda: coldaisle_cuda tests/test_parity

cuda/dc_cuda.o: cuda/dc_cuda.cu cuda/dc_cuda.h $(CORE_HDR)
	$(NVCC) $(NVFLAGS) -c cuda/dc_cuda.cu -o $@

coldaisle_cuda: app/coldaisle_cuda.c cuda/dc_cuda.o $(HOST_OBJ) host/dc_host.h
	$(CC) $(CFLAGS) app/coldaisle_cuda.c $(HOST_OBJ) cuda/dc_cuda.o -o $@ \
		-L$(dir $(shell which $(NVCC)))../lib64 -lcudart -lstdc++ $(LDLIBS)

tests/test_parity: tests/test_parity.c cuda/dc_cuda.o $(HOST_OBJ) $(CORE_HDR)
	$(CC) $(CFLAGS) $< $(HOST_OBJ) cuda/dc_cuda.o -o $@ \
		-L$(dir $(shell which $(NVCC)))../lib64 -lcudart -lstdc++ $(LDLIBS)

clean:
	rm -f coldaisle coldaisle_cuda $(HOST_OBJ) cuda/dc_cuda.o $(TESTS) tests/test_parity
