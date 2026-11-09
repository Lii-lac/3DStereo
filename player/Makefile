DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB   := $(DEVKITPRO)/libctru
PORTLIBS  := $(DEVKITPRO)/portlibs/3ds
TOOLS     := $(DEVKITPRO)/tools/bin
CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS    := -O2 -Wall -mword-relocations -ffunction-sections $(ARCH) -D__3DS__ \
             -I$(CTRULIB)/include -I$(PORTLIBS)/include -I$(PORTLIBS)/include/opus
OBJS      := source/main.o source/audio.o source/library.o source/tags.o source/art.o \
             source/decoder.o source/crumb.o source/flac.o source/vorbis.o source/stb.o source/stats.o \
             source/spotify.o source/spotmatch.o source/cacert.o source/net.o source/meta.o \
             source/connect.o source/cpb.o source/stream.o source/splash.o

BUILD      ?= 94
CFLAGS     += -DBUILD=$(BUILD) -DCC_RELAUNCH
BANNERTOOL ?= bannertool
MAKEROM    ?= makerom

all: player.3dsx player.cia

player.3dsx: player.elf player.smdh
	$(TOOLS)/3dsxtool $< $@ --smdh=player.smdh

player.elf: $(OBJS)
	$(CC) $(ARCH) $^ -specs=3dsx.specs -L$(PORTLIBS)/lib -L$(CTRULIB)/lib \
		-lopusfile -lopus -logg -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljansson -lz -lcitro2d -lcitro3d -lctru -lm -o $@

# the single-header libraries build without our warnings
source/flac.o: source/flac.c source/dr_flac.h source/dr_mp3.h source/dr_wav.h
	$(CC) $(CFLAGS) -w -c $< -o $@
source/vorbis.o: source/vorbis.c source/stb_vorbis.c
	$(CC) $(CFLAGS) -w -c $< -o $@
source/stb.o: source/stb.c source/stb_image.h
	$(CC) $(CFLAGS) -w -c $< -o $@

# the splash, inside the app
source/splash.o: source/splash.s cia/splash.png
	$(CC) $(ARCH) -c $< -o $@

# the CA bundle, inside the app
source/cacert.o: source/cacert.s source/cacert.pem
	$(CC) $(ARCH) -c $< -o $@

source/%.o: source/%.c source/*.h
	$(CC) $(CFLAGS) -c $< -o $@

player.smdh: icon.png
	$(TOOLS)/smdhtool --create "3DStereo" "Your music, at 47.6 kHz" "3ds-music" icon.png $@

# the HOME menu install (version 1.<build> to 1.63, then 2.0 = build 64: the minor stops at 63): same elf, wrapped with a banner, an icon, our own launch logo and its permissions.
# The logo (cia/logo.bin) must be SIGNED: HOME checks an HMAC-SHA256 at the end of the logo's darc, and a logo edited without re-signing made HOME itself panic (Luma dumps 8-13, builds 64-69 tests). Sign with tools/cia/sign_logo.py (key from HOME's own code, kept outside the repo).
player.cia: player.elf cia/pocket.rsf cia/banner.bnr cia/icon.icn cia/logo.bin
	$(MAKEROM) -f cia -o $@ -elf player.elf -rsf cia/pocket.rsf -icon cia/icon.icn 		-banner cia/banner.bnr -logo cia/logo.bin -exefslogo -target t -major $(shell echo $$((1 + $(BUILD) / 64))) -minor $(shell echo $$(($(BUILD) % 64))) -micro 0
# the pictures are drawn as svg: a png older than its svg stops the build (render it on Windows with
# tools/cia/svg2png.sh, Chrome isn't in the build image), so a stale banner can't ship again
cia/banner.png: cia/banner.svg
	@echo "cia/banner.png is older than cia/banner.svg: run tools/cia/svg2png.sh player/cia/banner.svg 256 128"; false
cia/splash.png: cia/splash.svg
	@echo "cia/splash.png is older than cia/splash.svg: run tools/cia/svg2png.sh player/cia/splash.svg 400 240"; false
icon.png: cia/icon.svg
	@echo "icon.png is older than cia/icon.svg: run tools/cia/svg2png.sh player/cia/icon.svg 48 48 player/icon.png"; false
cia/banner.wav: cia/make_jingle.py
	@echo "cia/banner.wav is older than cia/make_jingle.py: run uv run --with numpy python player/cia/make_jingle.py"; false
cia/banner3d.cgfx: ../tools/cia/make_banner3d_gltf.py ../tools/cia/make_banner3d.py cia/banner.svg
	@echo "cia/banner3d.cgfx is older than its maker: run uv run --with numpy --with pillow --with gltflib python -I tools/cia/make_banner3d_gltf.py"; false
cia/banner.bnr: cia/banner3d.cgfx cia/banner.wav
	$(BANNERTOOL) makebanner -ci cia/banner3d.cgfx -a cia/banner.wav -o $@
cia/icon.icn: icon.png
	$(BANNERTOOL) makesmdh -s "3DStereo" -l "Your music, at 47.6 kHz" -p "3ds-music" -i icon.png -o $@

clean:
	rm -f player.elf player.smdh player.3dsx player.cia cia/banner.bnr cia/icon.icn source/*.o
