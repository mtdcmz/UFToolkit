CC      = g++
RC      = windres
STRIP   = strip
TARGET  = UPPEditor.exe
SRC     = UPPEditor.cpp
RESOBJ  = resource.o

CFLAGS  = -m32 -std=c++14 -O2 -Wall -Wextra \
          -DUNICODE -D_UNICODE \
          -DWINVER=0x0600 -D_WIN32_WINNT=0x0600

# -municode selects the wide-entry CRT startup (wWinMainCRTStartup),
# which is what calls our wWinMain instead of an ANSI WinMain.
LDFLAGS = -mwindows -municode -m32 -static

# advapi32: registry access (reading UFunPlayer's history)
# others: shell/dialogs/listview, same family as UFunPlayer's deps
LIBS    = -lshell32 -lshlwapi -lcomctl32 -lcomdlg32 -ladvapi32

all: $(TARGET)

$(TARGET): $(SRC) $(RESOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	$(STRIP) $@
	@echo "Build OK: $(TARGET)"

$(RESOBJ): UPPEditor.rc resource.h UPPEditor.exe.manifest
	$(RC) -F pe-i386 UPPEditor.rc -o $@

clean:
	rm -f $(TARGET) $(RESOBJ)

.PHONY: all clean
