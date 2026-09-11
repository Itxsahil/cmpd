CC       ?= gcc
CXX      ?= g++

# Override OPT for a release build, e.g. make OPT="-O2 -DNDEBUG"
OPT      ?= -Og -g
WARNINGS := -Wall -Wextra -pedantic

PKGS     := libavformat libavcodec libavutil libswresample \
            portaudio-2.0 ncursesw panelw taglib

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

.PHONY: all clean run asan tsan test deps

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

# Address + undefined behaviour build.
asan:
	$(MAKE) OBJDIR=obj-asan TARGET=cmpd-asan \
		EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
		EXTRA_LDFLAGS="-fsanitize=address,undefined" $(ASAN_GOAL)

# Data race build. The decoder feeds the output ring from a second thread, so
# this is the one that matters for playback changes.
tsan:
	$(MAKE) OBJDIR=obj-tsan TARGET=cmpd-tsan \
		EXTRA_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
		EXTRA_LDFLAGS="-fsanitize=thread" $(TSAN_GOAL)

# Print the development packages this build needs.
deps:
	@pkg-config --exists $(PKGS) && echo "all dependencies present" || \
		(echo "missing packages; on Debian/Ubuntu:"; \
		 echo "  apt install libavformat-dev libavcodec-dev libavutil-dev \\"; \
		 echo "              libswresample-dev portaudio19-dev \\"; \
		 echo "              libncursesw5-dev libtag1-dev"; exit 1)

clean:
	rm -rf obj obj-asan obj-tsan cmpd cmpd-asan cmpd-tsan

-include $(DEPS)
