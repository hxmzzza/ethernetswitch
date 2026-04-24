# MinGW-w64 Makefile for ethernetswitch.
#
# Usage (from a MSYS2 / MinGW-w64 shell on Windows, or a Linux host with
# mingw-w64 cross-compilers):
#
#   1. powershell -ExecutionPolicy Bypass -File scripts/fetch_windivert.ps1
#        (or manually extract the WinDivert 2.2 release into
#         third_party/windivert/ with include/, x64/, x86/)
#   2. make                # 64-bit build
#   3. make ARCH=x86       # 32-bit build

ARCH ?= x64

ifeq ($(ARCH),x64)
  TOOLCHAIN_PREFIX ?= x86_64-w64-mingw32-
  SYS              := WinDivert64.sys
else
  TOOLCHAIN_PREFIX ?= i686-w64-mingw32-
  SYS              := WinDivert32.sys
endif

CC      := $(TOOLCHAIN_PREFIX)gcc
WINDRES := $(TOOLCHAIN_PREFIX)windres

ROOT    := $(CURDIR)
OUT     := $(ROOT)/build
WD      := $(ROOT)/third_party/windivert

CFLAGS  := -O2 -Wall -Wextra -I"$(WD)/include"
LDFLAGS := -mwindows -L"$(WD)/$(ARCH)" -lWinDivert -lcomctl32 -luser32 -lgdi32 -ladvapi32

SRCS    := src/main.c
RC      := src/ethernetswitch.rc
RES     := $(OUT)/ethernetswitch.res.o
EXE     := $(OUT)/ethernetswitch.exe

.PHONY: all clean deps

all: $(EXE)

$(OUT):
	mkdir -p $(OUT)

$(RES): $(RC) src/ethernetswitch.manifest | $(OUT)
	$(WINDRES) -I src -O coff -i $(RC) -o $(RES)

$(EXE): $(SRCS) $(RES) | $(OUT)
	@test -f "$(WD)/include/windivert.h" || (echo "!! Run scripts/fetch_windivert.ps1 first"; exit 1)
	$(CC) $(CFLAGS) $(SRCS) $(RES) -o $(EXE) $(LDFLAGS)
	cp "$(WD)/$(ARCH)/WinDivert.dll" $(OUT)/
	cp "$(WD)/$(ARCH)/$(SYS)"        $(OUT)/
	@echo ""
	@echo "Built $(EXE) (alongside WinDivert.dll and $(SYS))"

clean:
	rm -rf $(OUT)
