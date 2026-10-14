# Builds the portable modules and their regression tests natively on Linux,
# against the Win32 stand-in in include/ (not used by the real Windows build).
#
#   make -C tests/posix check LUA_DIR=<lua-5.4.7> GIFLIB_DIR=<giflib-5.2.2>
#
# Same pinned sources as CMakeLists.txt. Checks:
#   - UBSan in trap mode (needs no runtime library);
#   - SAN="-fsanitize=address,undefined" instead, where an ASan runtime exists;
#   - the GIF tests also run under guardmalloc.so (guard pages, no reuse), which
#     catches heap overruns and use-after-free without ASan.

LUA_DIR ?= ../../_deps/lua-5.4.7
GIFLIB_DIR ?= ../../_deps/giflib-5.2.2
BUILD ?= build
SRC ?= ../..
LANG_DEFINE ?= YAGP_LANG_EN
PYTHON ?= python3

CC ?= cc
SAN ?= -fsanitize=undefined -fsanitize-undefined-trap-on-error
OPT ?= -O1 -g
CFLAGS_COMMON := -std=c11 $(OPT) $(SAN) -D_GNU_SOURCE -DUNICODE -D_UNICODE -I include -I $(SRC) \
                 -include windows.h
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare

LUA_CORE := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes \
            lparser lstate lstring ltable ltm lundump lvm lzio \
            lauxlib lbaselib lcorolib lmathlib ltablib lutf8lib
GIF_CORE := dgif_lib gif_err gifalloc gif_hash openbsd-reallocarray

LUA_OBJ := $(LUA_CORE:%=$(BUILD)/lua/%.o) $(BUILD)/lua/lstrlib.o
GIF_OBJ := $(GIF_CORE:%=$(BUILD)/gif/%.o)
SHIM_OBJ := $(BUILD)/win32_shim.o

all: $(BUILD)/gif_dump $(BUILD)/script_test $(BUILD)/guardmalloc.so

$(BUILD)/lua/%.o: $(LUA_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu99 $(OPT) $(SAN) -w -c $< -o $@

# lstrlib with the pattern-matching step limit, generated exactly like CMake does.
$(BUILD)/lua/lstrlib_yagp.c: $(LUA_DIR)/src/lstrlib.c $(SRC)/cmake/patch_lstrlib.cmake
	@mkdir -p $(dir $@)
	cmake -DIN=$(abspath $<) -DOUT=$(abspath $@) -P $(SRC)/cmake/patch_lstrlib.cmake

$(BUILD)/lua/lstrlib.o: $(BUILD)/lua/lstrlib_yagp.c
	$(CC) -std=gnu99 $(OPT) $(SAN) -w -I $(LUA_DIR)/src -c $< -o $@

$(BUILD)/gif/%.o: $(GIFLIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu99 $(OPT) $(SAN) -w -c $< -o $@

$(SHIM_OBJ): win32_shim.c include/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS_COMMON) $(WARN) -c $< -o $@

$(BUILD)/guardmalloc.so: guardmalloc.c
	@mkdir -p $(BUILD)
	$(CC) -shared -fPIC -O1 -g $< -o $@

$(BUILD)/gif_dump: $(SRC)/tests/gif_dump.c $(SRC)/gif_stream.c $(SRC)/frame_cache.c wmain_bridge.c \
                   $(wildcard $(SRC)/*.h) $(SHIM_OBJ) $(GIF_OBJ)
	$(CC) $(CFLAGS_COMMON) $(WARN) -I $(GIFLIB_DIR) $(filter %.c,$^) $(SHIM_OBJ) $(GIF_OBJ) -o $@

$(BUILD)/script_test: $(SRC)/tests/script_test.c $(SRC)/script.c $(SRC)/monitors.c $(SRC)/contacts.c \
                      $(wildcard $(SRC)/*.h) $(SHIM_OBJ) $(LUA_OBJ)
	$(CC) $(CFLAGS_COMMON) $(WARN) -I $(LUA_DIR)/src -D$(LANG_DEFINE) \
	    -DYAGP_SCRIPT_DIR='"$(abspath $(SRC)/scripts)"' \
	    $(filter %.c,$^) $(SHIM_OBJ) $(LUA_OBJ) -lm -o $@

# Per-tick script cost (build with SAN= OPT=-O2 for meaningful numbers).
$(BUILD)/script_bench: script_bench.c $(SRC)/script.c $(SRC)/monitors.c $(SRC)/contacts.c \
                       $(wildcard $(SRC)/*.h) $(SHIM_OBJ) $(LUA_OBJ)
	$(CC) $(CFLAGS_COMMON) $(WARN) -I $(LUA_DIR)/src -D$(LANG_DEFINE) \
	    -DYAGP_SCRIPT_DIR='"$(abspath $(SRC)/scripts)"' \
	    $(filter %.c,$^) $(SHIM_OBJ) $(LUA_OBJ) -lm -o $@

check: all
	$(BUILD)/script_test
	$(PYTHON) gifgen.py check $(BUILD)/gif_dump
	LD_PRELOAD=$(abspath $(BUILD)/guardmalloc.so) $(PYTHON) gifgen.py check --quick $(BUILD)/gif_dump

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
