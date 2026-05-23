!include ..\common.mak

A2SDK=..\..\..\AviUtl2\aviutl2_sdk
AVIUTL2_DIR=..\..\..\AviUtl2\aviutl2beta46
INSTALL_DIR=$(AVIUTL2_DIR)\data\Plugin\AviUtl2LoopPlayback
TARGET=$(BIND)\aviutl2_loop_playback.aux2

CFLAGS=$(CFLAGS) /EHsc /std:c++17 /utf-8 /wd4828 /I$(A2SDK)
LIBS=$(LIBS) kernel32.lib user32.lib

all: dirs $(TARGET)

dirs:
	@if not exist $(BIND) mkdir $(BIND) && echo.   Created $(BIND)
	@if not exist $(OBJD) mkdir $(OBJD) && echo.   Created $(OBJD)

$(OBJD)\aviutl2_loop_playback.obj: aviutl2_loop_playback.cpp

$(TARGET): $(OBJD)\aviutl2_loop_playback.obj $(DEPS)
	cl /LD $(CFLAGS) /Fe$@ /Fd$(BIND)\aviutl2_loop_playback.pdb \
		$(OBJD)\aviutl2_loop_playback.obj \
		/link $(LINKFLAGS) /subsystem:windows /dll /out:$@ $(LIBS)

install: all
	@if not exist "$(INSTALL_DIR)" mkdir "$(INSTALL_DIR)"
	copy /y "$(TARGET)" "$(INSTALL_DIR)\aviutl2_loop_playback.aux2"
	copy /y "$(BIND)\aviutl2_loop_playback.pdb" "$(INSTALL_DIR)\aviutl2_loop_playback.pdb"

clean:
	-del "$(TARGET)" 2>nul
	-del "$(BIND)\aviutl2_loop_playback.*" 2>nul
	-rmdir /q /s "$(OBJD)" 2>nul

##############################################################################
