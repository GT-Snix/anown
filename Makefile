# anown build. macOS / Apple Silicon, Metal via sokol.
#   make                 library + examples + tests
#   make run EX=00_hello build and run one example
#   make test            build and run headless tests
#   make deps            fetch pinned sokol headers and sokol-shdc
#   make clean

CC      ?= clang
BUILD   := build
SHDC    := tools/sokol-shdc

WARN     := -Wall -Wextra -Wpedantic -Werror
CFLAGS   := -std=c11 -O1 -g $(WARN) -Iinclude -isystem vendor/sokol -I$(BUILD)/shaders \
            -DSOKOL_METAL -arch arm64 -MMD -MP
# sokol implementation: Objective-C, vendor code, so no -Werror.
OBJCFLAGS := -x objective-c -fobjc-arc -O1 -g -Wall -Wextra -isystem vendor/sokol \
             -arch arm64 -MMD -MP
LDLIBS   := -framework Metal -framework AppKit -framework QuartzCore

LIB_SRC  := $(wildcard src/*.c)
LIB_OBJ  := $(LIB_SRC:src/%.c=$(BUILD)/obj/%.o) $(BUILD)/obj/sokol_impl.o
LIB      := $(BUILD)/libanown.a

SHADERS  := $(wildcard shaders/*.glsl)
SHADER_H := $(SHADERS:shaders/%.glsl=$(BUILD)/shaders/%.glsl.h)

EXAMPLES := $(patsubst examples/%/main.c,%,$(wildcard examples/*/main.c))
EX_BIN   := $(EXAMPLES:%=$(BUILD)/examples/%)

# Spikes: throwaway single-file experiments, examples/spike_*.c (+ optional _mtl.m).
SPIKES   := $(patsubst examples/%.c,%,$(filter-out %_mtl.c,$(wildcard examples/spike_*.c)))
EX_BIN   += $(SPIKES:%=$(BUILD)/examples/%)

TESTS    :=$(patsubst tests/%.c,%,$(wildcard tests/test_*.c))
TEST_BIN := $(TESTS:%=$(BUILD)/tests/%)

.PHONY: all run test deps clean
all: $(LIB) $(EX_BIN) $(TEST_BIN)

# Shaders must exist before any C file that might include them.
$(BUILD)/shaders/%.glsl.h: shaders/%.glsl $(SHDC)
	@mkdir -p $(dir $@)
	$(SHDC) -i $< -o $@ -l metal_macos

$(BUILD)/obj/%.o: src/%.c $(SHADER_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/sokol_impl.o: src/sokol_impl.m
	@mkdir -p $(dir $@)
	$(CC) $(OBJCFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/examples/%: examples/%/main.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/obj/examples/%_mtl.o: examples/%_mtl.m
	@mkdir -p $(dir $@)
	$(CC) $(OBJCFLAGS) -Werror -c $< -o $@

$(BUILD)/examples/spike_export: examples/spike_export.c $(BUILD)/obj/examples/spike_export_mtl.o $(SHADER_H) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(BUILD)/obj/examples/spike_export_mtl.o $(LIB) $(LDLIBS) -o $@

$(BUILD)/tests/%: tests/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

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

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
