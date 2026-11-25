# anown build. macOS / Apple Silicon, Metal via sokol.
#   make                 library + examples + tests
#   make run EX=00_hello build and run one example
#   make test            build and run headless tests
#   make deps            fetch pinned sokol headers and sokol-shdc
#   make clean

CC      ?= clang
# Parallel by default: sokol_impl.m alone takes ~2.5 s, everything else overlaps it.
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu)
BUILD   := build
SHDC    := tools/sokol-shdc

WARN     := -Wall -Wextra -Wpedantic -Werror
CFLAGS   := -std=c11 -O1 -g $(WARN) -Iinclude -isystem vendor/sokol -isystem vendor/stb -I$(BUILD)/shaders \
            -DSOKOL_METAL -arch arm64 -MMD -MP
# Objective-C (Metal glue). sokol_impl.m is vendor code, so it alone skips -Werror.
OBJCFLAGS := -x objective-c -fobjc-arc -O1 -g -Wall -Wextra -Iinclude -isystem vendor/sokol \
             -DSOKOL_METAL -arch arm64 -MMD -MP
# Tests and spikes may use internal headers (src/*_internal.h); examples may not.
INTERNAL := -Isrc
LDLIBS   := -framework Metal -framework AppKit -framework QuartzCore

LIB_SRC  := $(wildcard src/*.c)
LIB_MSRC := $(wildcard src/*.m)
LIB_SSRC := $(wildcard src/*.S)
LIB_OBJ  := $(LIB_SRC:src/%.c=$(BUILD)/obj/%.o) $(LIB_MSRC:src/%.m=$(BUILD)/obj/%.o) $(LIB_SSRC:src/%.S=$(BUILD)/obj/%.o)
FONTS    := $(wildcard assets/fonts/*.ttf)
LIB      := $(BUILD)/libanown.a

SHADERS  := $(wildcard shaders/*.glsl)
SHADER_H := $(SHADERS:shaders/%.glsl=$(BUILD)/shaders/%.glsl.h)

EXAMPLES := $(patsubst examples/%/main.c,%,$(wildcard examples/*/main.c))
EX_BIN   := $(EXAMPLES:%=$(BUILD)/examples/%)

# Spikes: throwaway single-file experiments, examples/spike_*.c.
SPIKES   := $(patsubst examples/%.c,%,$(wildcard examples/spike_*.c))
EX_BIN   += $(SPIKES:%=$(BUILD)/examples/%)

TESTS    := $(patsubst tests/%.c,%,$(wildcard tests/test_*.c))
TEST_BIN := $(TESTS:%=$(BUILD)/tests/%)

.PHONY: all run test deps clean clean-cache
# Keep generated shader headers; make would otherwise delete them as intermediates.
.SECONDARY: $(SHADER_H)
all: $(LIB) $(EX_BIN) $(TEST_BIN)

# Shaders must exist before any C file that might include them.
$(BUILD)/shaders/%.glsl.h: shaders/%.glsl $(SHDC)
	@mkdir -p $(dir $@)
	$(SHDC) -i $< -o $@ -l metal_macos

$(BUILD)/obj/%.o: src/%.c $(SHADER_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: src/%.m
	@mkdir -p $(dir $@)
	$(CC) $(OBJCFLAGS) -Werror -c $< -o $@

# Vendor objects (sokol, stb) are the slowest compiles (2.4 s and 1.0 s) and
# change only when the vendored headers do. They're cached in .vendor-cache/
# (gitignored, outside build/, so make clean keeps it) under a key hashing the
# impl file, the vendor headers and the flags: a clean build copies them; any
# change to those inputs compiles (and caches) a new one.
VENDOR_CACHE := .vendor-cache
SOKOL_KEY := $(shell (cat src/sokol_impl.m vendor/sokol/*.h; echo '$(CC) $(OBJCFLAGS)') | shasum | cut -c1-16)
STB_KEY   := $(shell (cat src/stb_impl.c vendor/stb/*.h; echo '$(CC) $(CFLAGS)') | shasum | cut -c1-16)

$(BUILD)/obj/sokol_impl.o: src/sokol_impl.m $(wildcard vendor/sokol/*.h)
	@mkdir -p $(dir $@) $(VENDOR_CACHE)
	@if [ -f $(VENDOR_CACHE)/sokol_impl-$(SOKOL_KEY).o ]; then \
		echo "cp $(VENDOR_CACHE)/sokol_impl-$(SOKOL_KEY).o $@"; cp $(VENDOR_CACHE)/sokol_impl-$(SOKOL_KEY).o $@; \
	else \
		echo "$(CC) $(OBJCFLAGS) -c $< -o $@"; $(CC) $(OBJCFLAGS) -c $< -o $@ && cp $@ $(VENDOR_CACHE)/sokol_impl-$(SOKOL_KEY).o; \
	fi

# the built-in fonts, embedded with .incbin (src/fonts.S)
$(BUILD)/obj/fonts.o: src/fonts.S $(FONTS)
	@mkdir -p $(dir $@)
	$(CC) -arch arm64 -c $< -o $@

# stb_image_write and stb_truetype: vendor code, so no -Werror (and no -Wpedantic)
$(BUILD)/obj/stb_impl.o: src/stb_impl.c $(wildcard vendor/stb/*.h)
	@mkdir -p $(dir $@) $(VENDOR_CACHE)
	@if [ -f $(VENDOR_CACHE)/stb_impl-$(STB_KEY).o ]; then \
		echo "cp $(VENDOR_CACHE)/stb_impl-$(STB_KEY).o $@"; cp $(VENDOR_CACHE)/stb_impl-$(STB_KEY).o $@; \
	else \
		echo "$(CC) $(filter-out -Werror -Wpedantic,$(CFLAGS)) -c $< -o $@"; \
		$(CC) $(filter-out -Werror -Wpedantic,$(CFLAGS)) -c $< -o $@ && cp $@ $(VENDOR_CACHE)/stb_impl-$(STB_KEY).o; \
	fi

$(LIB): $(LIB_OBJ)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/examples/%: examples/%/main.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/examples/spike_%: examples/spike_%.c $(SHADER_H) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INTERNAL) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/tests/%: tests/%.c $(SHADER_H) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INTERNAL) $< $(LIB) $(LDLIBS) -o $@

run: $(BUILD)/examples/$(EX)
	./$(BUILD)/examples/$(EX)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do ./$$t || exit 1; done

$(SHDC):
	./scripts/fetch-deps.sh

deps:
	./scripts/fetch-deps.sh

clean:
	rm -rf $(BUILD)

# Also drop the cached vendor objects (they rebuild on the next make).
clean-cache:
	rm -rf $(VENDOR_CACHE)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
