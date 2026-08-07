# zm_emu —— 使用系统库的快速构建（与 xmake.lua 等价，方便 CI / 无网络环境）
#
#   make            构建 build/linux/x86_64/release/zm_emu
#   make debug      带 -g -O0 的调试构建
#   make clean      清理
#
# 依赖（Debian/Ubuntu）：
#   libsdl2-dev libsdl2-ttf-dev libsdl2-mixer-dev libcapstone-dev libunicorn-dev

CC      ?= cc
MODE    ?= release

SRCDIR  := src
OUTDIR  := build/linux/x86_64/$(MODE)
OBJDIR  := $(OUTDIR)/.objs
TARGET  := $(OUTDIR)/zm_emu

SRCS    := $(shell find $(SRCDIR) -name '*.c' | sort)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

PKGS    := sdl2 SDL2_ttf SDL2_mixer capstone libpng libjpeg
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)

CFLAGS  := -std=gnu11 -Wall -Wno-unused-parameter -Wno-unused-function \
           -DLOG_USE_COLOR -DHAVE_PNG -DHAVE_JPEG $(PKG_CFLAGS)
LDFLAGS := $(PKG_LIBS) -lunicorn -lm -lpthread

ifeq ($(MODE),debug)
CFLAGS  += -g -O0 -DDEBUG
else
CFLAGS  += -Os -DNDEBUG
endif

.PHONY: all debug clean run test

all: $(TARGET)

debug:
	@$(MAKE) MODE=debug

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "==> 构建完成: $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build/linux

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./scripts/run_all_tests.sh

-include $(DEPS)
