CC       ?= gcc
CXX      ?= g++

# Override OPT for a release build, e.g. make OPT="-O2 -DNDEBUG"
OPT      ?= -Og -g
WARNINGS := -Wall -Wextra -pedantic

PKGS     := libavformat libavcodec libavutil libswresample \
            portaudio-2.0 ncursesw panelw taglib

# Debian/Ubuntu package names for $(PKGS), kept next to them so they stay in sync.
APT_PKGS := build-essential pkg-config libavformat-dev libavcodec-dev \
            libavutil-dev libswresample-dev portaudio19-dev \
            libncurses-dev libtag1-dev

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

# EXTRA_CFLAGS/EXTRA_LDFLAGS are the hook the sanitizer targets use.
BASE_CFLAGS := $(OPT) $(WARNINGS) $(PKG_CFLAGS) -Ithird_party -Isrc
CFLAGS   := $(BASE_CFLAGS) $(EXTRA_CFLAGS)
CXXFLAGS := $(BASE_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS  := $(EXTRA_LDFLAGS)
LDLIBS   := $(PKG_LIBS) -lm -lpthread -lstdc++

SRCDIR   := src
OBJDIR   ?= obj
TARGET   ?= cmpd

CSRCS    := $(shell find $(SRCDIR) -name '*.c')
CXXSRCS  := $(shell find $(SRCDIR) -name '*.cpp')
COBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CSRCS))
CXXOBJS  := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(CXXSRCS))
OBJS     := $(COBJS) $(CXXOBJS)
DEPS     := $(OBJS:.o=.d)

.PHONY: all clean run asan tsan test test-asan test-tsan valgrind check deps

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

# ── tests ──────────────────────────────────────────────────────────────
TESTSRCS := $(wildcard tests/test_*.c)
TESTBINS := $(patsubst tests/%.c,$(OBJDIR)/tests/%,$(TESTSRCS))
LIBOBJS  := $(filter-out $(OBJDIR)/main.o,$(OBJS))

$(OBJDIR)/tests/%: tests/%.c $(LIBOBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests $(LDFLAGS) -o $@ $< $(LIBOBJS) $(LDLIBS)

test: $(TESTBINS)
	@fail=0; for t in $(TESTBINS); do \
		"$$t" || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "all tests passed"; else echo "TESTS FAILED"; fi; \
	exit $$fail

# Sanitizer builds. ASAN_FLAGS/TSAN_FLAGS are shared by the binary and the
# test targets below so both are built the same way.
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer

asan:
	$(MAKE) OBJDIR=obj-asan TARGET=cmpd-asan \
		EXTRA_CFLAGS="$(ASAN_FLAGS)" EXTRA_LDFLAGS="$(ASAN_FLAGS)"

# The decoder feeds the output ring from a second thread, so this is the one
# that matters for playback changes.
tsan:
	$(MAKE) OBJDIR=obj-tsan TARGET=cmpd-tsan \
		EXTRA_CFLAGS="$(TSAN_FLAGS)" EXTRA_LDFLAGS="$(TSAN_FLAGS)"

test-asan:
	$(MAKE) OBJDIR=obj-asan TARGET=cmpd-asan \
		EXTRA_CFLAGS="$(ASAN_FLAGS)" EXTRA_LDFLAGS="$(ASAN_FLAGS)" test

test-tsan:
	$(MAKE) OBJDIR=obj-tsan TARGET=cmpd-tsan \
		EXTRA_CFLAGS="$(TSAN_FLAGS)" EXTRA_LDFLAGS="$(TSAN_FLAGS)" test

# Valgrind cannot say anything useful about test_output: ALSA hands PortAudio
# uninitialised stack data (confirmed with --track-origins: "created by a stack
# allocation ... in libasound"), which arrives in the audio callback as the
# frame count and taints every conditional downstream. That test's own memory
# safety is covered by test-asan and test-tsan instead, both clean.
VALGRIND_TESTS := $(filter-out $(OBJDIR)/tests/test_output,$(TESTBINS))
VALGRIND_FLAGS := --quiet --error-exitcode=99 \
                  --leak-check=full --show-leak-kinds=definite,indirect \
                  --errors-for-leak-kinds=definite,indirect \
                  --suppressions=tests/valgrind.supp

valgrind: $(VALGRIND_TESTS)
	@for t in $(VALGRIND_TESTS); do \
		printf '%-28s ' "$$(basename $$t)"; \
		valgrind $(VALGRIND_FLAGS) "$$t" >/dev/null || exit 1; \
		echo "clean"; \
	done

# Everything CI runs, in one target.
check: test test-asan test-tsan valgrind

# The decoder uses AVChannelLayout and swr_alloc_set_opts2, which arrived in
# FFmpeg 5.1. Checking the floor here beats a wall of compiler errors.
deps:
	@ok=1; \
	pkg-config --exists $(PKGS) || { \
		echo "missing development packages; on Debian/Ubuntu:"; \
		echo "  sudo apt install $(APT_PKGS)"; ok=0; }; \
	pkg-config --atleast-version=59.18 libavcodec || { \
		echo "libavcodec >= 59.18 required (FFmpeg 5.1); found $$(pkg-config --modversion libavcodec 2>/dev/null || echo none)"; ok=0; }; \
	pkg-config --atleast-version=4.5 libswresample || { \
		echo "libswresample >= 4.5 required (FFmpeg 5.1); found $$(pkg-config --modversion libswresample 2>/dev/null || echo none)"; ok=0; }; \
	[ $$ok -eq 1 ] && echo "all dependencies present" || exit 1

clean:
	rm -rf obj obj-asan obj-tsan cmpd cmpd-asan cmpd-tsan

-include $(DEPS)
