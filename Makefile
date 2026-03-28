# Minecraft PSP Port — Makefile
# Target: PSP (MIPS-allegrex, >=PSP-2000 cu 64MB RAM)
# Compilator: psp-gcc via PSPSDK

TARGET = MinecraftPSP
.DEFAULT_GOAL := EBOOT.PBP
PSPSDK = $(shell psp-config --pspsdk-path)

# Metadata EBOOT
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE  = Minecraft PSP
# PSP_EBOOT_ICON   = res/ICON0.PNG

# Surse
SRCS = src/main.cpp \
       src/world/Random.cpp \
       src/world/Mth.cpp \
       src/world/Vec3.cpp \
       src/world/AABB.cpp \
       src/world/Blocks.cpp \
       src/world/NoiseGen.cpp \
       src/world/TreeFeature.cpp \
       src/world/WorldGen.cpp \
       src/world/Chunk.cpp \
       src/world/Level.cpp \
       src/world/Raycast.cpp \
       src/render/PSPRenderer.cpp \
       src/render/ChunkRenderer.cpp \
       src/render/TextureAtlas.cpp \
       src/render/Tesselator.cpp \
       src/render/TileRenderer.cpp \
       src/render/SkyRenderer.cpp \
       src/render/CloudRenderer.cpp \
       src/render/PigMob.cpp \
       src/render/BlockHighlight.cpp \
       src/math/Frustum.cpp \
       src/input/PSPInput.cpp

OBJS = $(SRCS:.cpp=.o)

# Flags
CXXFLAGS = -O2 -G0 -Wall \
           -I$(PSPSDK)/include \
           -Isrc \
           -std=c++11 \
           -fno-exceptions \
           -fno-rtti \
           -DPSP \
           -Wno-misleading-indentation \
           -Wno-unused-function

PSP_FW_VERSION = 600

# Librarii
LIBS = -lstdc++ \
       -lpspgum -lpspgu \
       -lpspaudiolib -lpspaudio \
       -lpsprtc \
       -lpsppower \
       -lm

# Build
include $(PSPSDK)/lib/build.mak



PACKAGE_ZIP = $(TARGET)-package.zip
BUILD_INFO_FILE = BUILD_INFO.txt

package: EBOOT.PBP
	@echo "commit: $$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" > $(BUILD_INFO_FILE)
	@echo "time_utc: $$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> $(BUILD_INFO_FILE)
	@rm -f $(PACKAGE_ZIP)
	@if command -v python3 >/dev/null 2>&1; then \
		python3 -c "import os,zipfile; z=zipfile.ZipFile('$(PACKAGE_ZIP)','w',zipfile.ZIP_DEFLATED); z.write('EBOOT.PBP','EBOOT.PBP'); z.write('$(BUILD_INFO_FILE)','$(BUILD_INFO_FILE)'); [z.write(os.path.join(r,f), os.path.join(r,f)) for r,_,fs in os.walk('res') for f in fs]; z.close()"; \
	elif command -v python >/dev/null 2>&1; then \
		python -c "import os,zipfile; z=zipfile.ZipFile('$(PACKAGE_ZIP)','w',zipfile.ZIP_DEFLATED); z.write('EBOOT.PBP','EBOOT.PBP'); z.write('$(BUILD_INFO_FILE)','$(BUILD_INFO_FILE)'); [z.write(os.path.join(r,f), os.path.join(r,f)) for r,_,fs in os.walk('res') for f in fs]; z.close()"; \
	elif command -v zip >/dev/null 2>&1; then \
		zip -r "$(PACKAGE_ZIP)" EBOOT.PBP res "$(BUILD_INFO_FILE)"; \
	elif command -v busybox >/dev/null 2>&1; then \
		busybox zip -r "$(PACKAGE_ZIP)" EBOOT.PBP res "$(BUILD_INFO_FILE)"; \
	else \
		echo "Error: no zip-capable tool found (python3/python/zip/busybox)." >&2; \
		exit 127; \
	fi

.PHONY: package
