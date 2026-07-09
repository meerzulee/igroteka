# Conformance corpus — tiny 32-bit PEs, one seam each.
# Requires i686-w64-mingw32-gcc (brew install mingw-w64 / apt install gcc-mingw-w64-i686).

CC      := i686-w64-mingw32-gcc
CFLAGS  := -Os -ffreestanding -fno-asynchronous-unwind-tables -fno-ident
LDFLAGS := -nostdlib -Wl,--entry=_start -Wl,--subsystem,console \
           -Wl,--image-base=0x400000 -lkernel32 -luser32 -lgdi32 -ld3d9

BIN := bin
RUNGS := $(BIN)/hello.exe $(BIN)/window.exe $(BIN)/recurse.exe $(BIN)/seh.exe \
         $(BIN)/seh_chain.exe $(BIN)/seh_noncont.exe $(BIN)/fpu.exe \
         $(BIN)/paint.exe $(BIN)/d3dclear.exe $(BIN)/tri.exe $(BIN)/tri_evil.exe \
         $(BIN)/vbtri.exe $(BIN)/vb_evil.exe $(BIN)/fftri.exe

all: $(RUNGS)

.SECONDEXPANSION:
$(BIN)/%.exe: $$*/$$*.c | $(BIN)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

$(BIN):
	mkdir -p $(BIN)

clean:
	rm -rf $(BIN)

.PHONY: all clean
