CC      ?= gcc
CFLAGS  ?= -Og -g -Wall -Wextra -pedantic
CFLAGS  += $(shell pkg-config --cflags libavformat libavcodec libavutil portaudio-2.0 ncursesw taglib)
CFLAGS  += -Ithird_party -Isrc
LDLIBS  := $(shell pkg-config --libs libavformat libavcodec libavutil portaudio-2.0 ncursesw taglib)
LDLIBS  += -lm -lpthread

SRCDIR  := src
OBJDIR  := obj
BINDIR  := .
TARGET  := $(BINDIR)/cmpd

SRCS    := $(shell find $(SRCDIR) -name '*.c')
OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

$(BINDIR) $(OBJDIR):
	@mkdir -p $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)

-include $(DEPS)
