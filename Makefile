# keyhunt build
#
#   make                 build ./keyhunt
#   make bsgsd           build ./bsgsd (BSGS server, Linux only)
#   make legacy          build ./keyhunt from keyhunt_legacy.cpp (needs libssl-dev, libgmp-dev;
#                        for CPUs/systems without SSE, e.g. ARM)
#   make test            build and run the test suite
#   make kangaroo        build ./kangaroo (novel/, Pollard kangaroo with negation map)
#   make clean
#
# Useful knobs:
#   make -j$(nproc)      parallel build
#   make ARCH=x86-64-v3  target a specific CPU instead of the build machine (default: native)
#   make LTO=1           link time optimisation
#   make DEBUG=1         -O0 -g, no -march=native
#   make SANITIZE=1      build with ASan + UBSan
#   make V=1             show the full compiler command lines

CC  ?= gcc
CXX ?= g++

ARCH  ?= native
BUILD ?= build

MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq (,$(filter x86_64% i%86%,$(MACHINE)))
  X86 := 1
endif

ifdef DEBUG
  OPTFLAGS := -O0 -g
else
  OPTFLAGS := -O3 -ftree-vectorize
  ifdef X86
    OPTFLAGS += -march=$(ARCH) -mtune=$(if $(filter native,$(ARCH)),native,generic)
  else
    OPTFLAGS += -mcpu=$(ARCH)
  endif
endif
ifdef X86
  # SSSE3 is the baseline of the 4-way SSE hash kernels. Only the main/bsgsd
  # objects get it: the legacy build is meant for CPUs without that baseline.
  SIMDFLAGS := -mssse3
endif
ifdef LTO
  OPTFLAGS += -flto
endif
ifdef SANITIZE
  OPTFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
endif

WARN     := -Wall -Wextra
CFLAGS   := $(OPTFLAGS) $(WARN) -Wno-unused-parameter -MMD -MP
CXXFLAGS := $(OPTFLAGS) $(WARN) -Wno-deprecated-copy -std=gnu++17 -MMD -MP
MAIN_CFLAGS   := $(CFLAGS) $(SIMDFLAGS)
MAIN_CXXFLAGS := $(CXXFLAGS) $(SIMDFLAGS)
LDLIBS   := -lm -lpthread

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# Plain C sources
C_SRCS   := base58/base58.c rmd160/rmd160.c xxhash/xxhash.c
# These ".c" files are written as C++ and always have been compiled as such
CXXC_SRCS := util.c sha3/sha3.c sha3/keccak.c
CXX_SRCS := oldbloom/bloom.cpp bloom/bloom.cpp \
            secp256k1/Int.cpp secp256k1/Point.cpp secp256k1/SECP256K1.cpp \
            secp256k1/IntMod.cpp secp256k1/Random.cpp secp256k1/IntGroup.cpp \
            hash/ripemd160.cpp hash/sha256.cpp hash/ripemd160_sse.cpp hash/sha256_sse.cpp \
            hash/hash160_simd.cpp

COMMON_OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o) $(CXXC_SRCS:.c=.o) $(CXX_SRCS:.cpp=.o))

# Sources of the legacy (libgmp / OpenSSL) variant
LEGACY_SRCS := oldbloom/bloom.cpp bloom/bloom.cpp \
               gmp256k1/Int.cpp gmp256k1/Point.cpp gmp256k1/GMP256K1.cpp \
               gmp256k1/IntMod.cpp gmp256k1/Random.cpp gmp256k1/IntGroup.cpp
LEGACY_CXXC := util.c sha3/sha3.c sha3/keccak.c hashing.c
LEGACY_OBJS := $(addprefix $(BUILD)/legacy/,base58/base58.o xxhash/xxhash.o \
               $(LEGACY_SRCS:.cpp=.o) $(LEGACY_CXXC:.c=.o))

# Make only compares timestamps, so record the compiler and flags in a stamp
# file that every object depends on; changing ARCH/LTO/DEBUG/SANITIZE/... then
# rebuilds instead of silently reusing incompatible objects.
STAMP := $(BUILD)/.config
CONFIG := $(CC) $(CXX) $(MAIN_CFLAGS) $(MAIN_CXXFLAGS) $(LDLIBS)
$(shell mkdir -p $(BUILD); echo '$(CONFIG)' | cmp -s - $(STAMP) || echo '$(CONFIG)' > $(STAMP))

.PHONY: default all clean legacy bsgsd keyhunt kangaroo lineage lcgfit test
default: all
all: keyhunt

# The variants are linked under build/ and copied to the top level, so building
# one after the other always refreshes ./keyhunt (it is the same output name).
keyhunt: $(BUILD)/keyhunt.bin
	@cp -f $< $@

bsgsd: $(BUILD)/bsgsd.bin
	@cp -f $< $@

legacy: $(BUILD)/keyhunt-legacy.bin
	@cp -f $< keyhunt

kangaroo: $(BUILD)/kangaroo.bin
	@cp -f $< $@

lineage: $(BUILD)/lineage.bin
	@cp -f $< $@

lcgfit: $(BUILD)/lcgfit.bin
	@cp -f $< $@

$(BUILD)/lcgfit.bin: $(BUILD)/novel/lcgfit.o
	@echo "  LD    lcgfit"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/lineage.bin: $(BUILD)/novel/lineage.o
	@echo "  LD    lineage"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/kangaroo.bin: $(BUILD)/novel/kangaroo.o $(COMMON_OBJS)
	@echo "  LD    kangaroo"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/keyhunt.bin: $(BUILD)/keyhunt.o $(COMMON_OBJS)
	@echo "  LD    keyhunt"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/bsgsd.bin: $(BUILD)/bsgsd.o $(COMMON_OBJS)
	@echo "  LD    bsgsd"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/keyhunt-legacy.bin: $(BUILD)/legacy/keyhunt_legacy.o $(LEGACY_OBJS)
	@echo "  LD    keyhunt (legacy)"
	$(Q)$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS) -lcrypto -lgmp

# ---- compile rules ----------------------------------------------------------

$(BUILD)/%.o: %.cpp $(STAMP)
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -c $< -o $@

$(addprefix $(BUILD)/,$(CXXC_SRCS:.c=.o)): $(BUILD)/%.o: %.c $(STAMP)
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -x c++ -c $< -o $@

$(addprefix $(BUILD)/,$(C_SRCS:.c=.o)): $(BUILD)/%.o: %.c $(STAMP)
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(Q)$(CC) $(MAIN_CFLAGS) -c $< -o $@

# Legacy objects (separate directory: same file names, different flags/sources, no SSSE3)
$(BUILD)/legacy/%.o: %.cpp $(STAMP)
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

$(addprefix $(BUILD)/legacy/,$(LEGACY_CXXC:.c=.o)): $(BUILD)/legacy/%.o: %.c $(STAMP)
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(Q)$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

$(BUILD)/legacy/base58/base58.o $(BUILD)/legacy/xxhash/xxhash.o: $(BUILD)/legacy/%.o: %.c $(STAMP)
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -Wno-unused-result -c $< -o $@

# ---- tests ------------------------------------------------------------------

TEST_BINS := $(BUILD)/test_hash160 $(BUILD)/test_int
TEST_HASH_OBJS := $(addprefix $(BUILD)/hash/,hash160_simd.o sha256.o ripemd160.o ripemd160_sse.o sha256_sse.o) \
                  $(addprefix $(BUILD)/secp256k1/,Int.o Point.o SECP256K1.o IntMod.o Random.o IntGroup.o) \
                  $(BUILD)/util.o

$(BUILD)/test_hash160: $(BUILD)/tests/test_hash160.o $(TEST_HASH_OBJS)
	@echo "  LD    $@"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_int: $(BUILD)/tests/test_int.o $(TEST_HASH_OBJS)
	@echo "  LD    $@"
	$(Q)$(CXX) $(MAIN_CXXFLAGS) -o $@ $^ $(LDLIBS)

test: keyhunt kangaroo lcgfit lineage $(TEST_BINS)
	@$(BUILD)/test_hash160
	@$(BUILD)/test_int
	@sh tests/run_tests.sh ./keyhunt
	@sh tests/test_kangaroo.sh ./kangaroo
	@./lcgfit s > /dev/null && echo "[ok]   lcgfit self test"
	@./lineage selftest 2 > /dev/null && echo "[ok]   lineage self test"
	@if command -v python3 > /dev/null; then python3 novel/nonce_forensics.py | grep "creator nonces deterministic (RFC 6979): True" > /dev/null && echo "[ok]   nonce forensics (RFC 6979 confirmed)"; else echo "[skip] nonce forensics (no python3)"; fi

clean:
	rm -rf $(BUILD) keyhunt bsgsd kangaroo lineage lcgfit

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
