
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / macOS
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -shared
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
endif

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
LIBS = -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

.SUFFIXES: .c .so .xo .o

MODULES = helloworld.so hellotype.so helloblock.so hellocluster.so hellotimer.so hellodict.so hellohook.so helloacl.so

# hellostorage registers a storage engine, so it builds only with data
# tiering. When BUILD_EXT_STORAGE is not passed in, fall back to how the server
# was built, recorded in src/.make-settings.
ifeq ($(BUILD_EXT_STORAGE),)
BUILD_EXT_STORAGE := $(shell sed -n 's/^BUILD_EXT_STORAGE=//p' ../.make-settings 2>/dev/null)
endif
ifeq ($(BUILD_EXT_STORAGE),yes)
MODULES += hellostorage.so
endif

all: $(MODULES)

.c.xo:
	$(CC) -I. $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

helloworld.xo: ../valkeymodule.h

helloworld.so: helloworld.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellotype.xo: ../valkeymodule.h

hellotype.so: hellotype.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

helloblock.xo: ../valkeymodule.h

helloblock.so: helloblock.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lpthread -lc

hellocluster.xo: ../valkeymodule.h

hellocluster.so: hellocluster.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellotimer.xo: ../valkeymodule.h

hellotimer.so: hellotimer.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellodict.xo: ../valkeymodule.h

hellodict.so: hellodict.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellohook.xo: ../valkeymodule.h

hellohook.so: hellohook.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

helloacl.xo: ../valkeymodule.h

helloacl.so: helloacl.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

# hellostorage needs the storage engine interface header.
hellostorage.xo: hellostorage.c ../valkeymodule.h ../storage/storage.h
	$(CC) -I. -I.. -I../storage $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

hellostorage.so: hellostorage.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

clean:
	rm -rf *.xo *.so
