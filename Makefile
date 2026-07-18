CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -Og -g -Wall -Wextra -pedantic
CXXFLAGS?= -Og -g -Wall -Wextra -pedantic
CFLAGS  += $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample portaudio-2.0 ncursesw taglib)
CFLAGS  += -Ithird_party -Isrc
CXXFLAGS+= $(CFLAGS)
LDLIBS  := $(shell pkg-config --libs libavformat libavcodec libavutil libswresample portaudio-2.0 ncursesw taglib)
LDLIBS  += -lm -lpthread -lstdc++

SRCDIR  := src
OBJDIR  := obj
BINDIR  := .
TARGET  := $(BINDIR)/cmpd

CSRCS   := $(shell find $(SRCDIR) -name '*.c')
CXXSRCS := $(shell find $(SRCDIR) -name '*.cpp')
COBJS   := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CSRCS))
CXXOBJS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(CXXSRCS))
OBJS    := $(COBJS) $(CXXOBJS)
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

$(BINDIR) $(OBJDIR):
	@mkdir -p $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)

-include $(DEPS)
