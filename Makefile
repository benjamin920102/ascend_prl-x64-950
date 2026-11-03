# Requires the Ascend CANN toolchain; all artifacts are emitted to build/.
# RANK selects compatible K, NINNER, and NFOLD combinations.

RANK ?= 128
# The current kernels and optimized preparation layout are rank-128 only.
ifneq ($(RANK),128)
  $(error This kernel implementation supports RANK=128 only)
endif
HOST_ARCH ?= $(shell uname -m)
NPU_ARCH ?= dav-3510
GRID ?= 20
VEC2X ?= 2
K := 4096
NINNER := 1
NFOLD := 32
MDIM ?= 16384
# Host strip slots (separate from kernel BUFS). Use 2 for an A/B baseline.
PIPE_DEPTH ?= 4

OUT  := build
# Locate BLAKE3 C sources from the Cargo registry when building the standalone miner.
BLK ?= $(CURDIR)/vendor/BLAKE3/c
BLK_SRC := $(BLK)/blake3.c $(BLK)/blake3_dispatch.c $(BLK)/blake3_portable.c
CC = gcc
# The pthread split hook is implemented locally in src/blake3_join.c.
CFLAGS := -std=gnu11 -O3 -fopenmp -DBLAKE3_USE_TBB -DK=$(K) -DRANK=$(RANK) -DMDIM=$(MDIM) -DPEARL_PIPE_DEPTH=$(PIPE_DEPTH) -DPEARL_GRID=$(GRID) -I$(BLK)
ifeq ($(HOST_ARCH),aarch64)
  BLK_SRC += $(BLK)/blake3_neon.c
  CFLAGS += -march=armv8-a+crypto -DBLAKE3_USE_NEON=1
else ifeq ($(HOST_ARCH),x86_64)
  CFLAGS += -DBLAKE3_USE_NEON=0
  ifeq ($(PORTABLE),1)
    CFLAGS += -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512
  else
    BLK_SRC += $(BLK)/blake3_sse2_x86-64_unix.S $(BLK)/blake3_sse41_x86-64_unix.S $(BLK)/blake3_avx2_x86-64_unix.S $(BLK)/blake3_avx512_x86-64_unix.S
  endif
else
  $(error Unsupported HOST_ARCH=$(HOST_ARCH))
endif
RPATH  := -Wl,-rpath,'$$ORIGIN'

.PHONY: all miner kernel proof guard clean
all: miner

# Safe ordering also for make -j.
miner: kernel proof

$(OUT):
	mkdir -p $(OUT)

# Link exactly one frontend per binary; build-time rank/shape is encoded in the proof.
ENGINE_SRC := src/miner.c src/proofgz.c src/pools/stratum.c src/prep.c src/scan_strip_async.c src/blake3_join.c $(BLK_SRC)
# Use libz's soname: some runtime images lack the libz.so development symlink.
LINK := -L$(OUT) -lpearl_hlc2 -lpearl_proof -lpthread -l:libz.so.1 $(RPATH)
miner: $(OUT)
	$(CC) $(CFLAGS) -o $(OUT)/ascend_prl_kryptex src/pools/kryptex.c $(ENGINE_SRC) $(LINK)
	$(CC) $(CFLAGS) -o $(OUT)/ascend_prl_k1       src/pools/k1.c      $(ENGINE_SRC) $(LINK)

# Optional guard requires the DCMI driver library and sufficient device privileges.
DCMI_DIR ?= /usr/local/dcmi
guard: $(OUT)
	$(CC) -O2 -I$(DCMI_DIR) -o $(OUT)/coexist_guard scripts/coexist_guard.c \
	    -L$(DCMI_DIR) -ldcmi -Wl,-rpath,$(DCMI_DIR)

kernel: $(OUT)
	cd kernel && rm -rf build && mkdir build && cd build && \
	  cmake -DCMAKE_ASC_RUN_MODE=npu -DCMAKE_ASC_ARCHITECTURES=$(NPU_ARCH) \
	        -DPEARL_PIPE_DEPTH=$(PIPE_DEPTH) -DNINNER=$(NINNER) -DNFOLD=$(NFOLD) -DBUFS=4 -DVEC2X=$(VEC2X) -DPEARL_GRID=$(GRID) \
	        -DCMAKE_MODULE_PATH="$$ASC_MODULES" .. && make -j
	cp kernel/build/libpearl_hlc2.so $(OUT)/

proof: $(OUT)
	cd proof-ffi && unset MAKEFLAGS && cargo build --release
	cp proof-ffi/target/release/libpearl_proof.so $(OUT)/

clean:
	rm -rf $(OUT) kernel/build proof-ffi/target
