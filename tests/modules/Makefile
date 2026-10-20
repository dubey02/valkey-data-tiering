
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

warning_cflags = -W -Wall -Wno-missing-field-initializers
ifeq ($(uname_S),Darwin)
	SHOBJ_CFLAGS ?= $(warning_cflags) -dynamic -fno-common -g -ggdb -std=gnu11 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
else	# Linux, others
	SHOBJ_CFLAGS ?= $(warning_cflags) -fno-common -g -ggdb -std=gnu11 -O2
	SHOBJ_LDFLAGS ?= -shared
endif

ifeq ($(uname_S),Linux)
    LD = gcc
    CC = gcc
endif

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
LIBS = -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

TEST_MODULES = \
    commandfilter.so \
    commandresult.so \
    basics.so \
    testrdb.so \
    fork.so \
    infotest.so \
    propagate.so \
    misc.so \
    hooks.so \
    blockonkeys.so \
    blockonbackground.so \
    scan.so \
    datatype.so \
    datatype2.so \
    auth.so \
    keyspace_events.so \
    block_keyspace_notification.so \
    blockedclient.so \
    getkeys.so \
    getchannels.so \
    test_lazyfree.so \
    timer.so \
    defragtest.so \
    keyspecs.so \
    hash.so \
    hash_stringref.so \
    zset.so \
    stream.so \
    mallocsize.so \
    aclcheck.so \
    list.so \
    subcommands.so \
    reply.so \
    cmdintrospection.so \
    eventloop.so \
    moduleconfigs.so \
    moduleconfigstwo.so \
    moduleparameter.so \
    publish.so \
    usercall.so \
    postnotifications.so \
    moduleauthtwo.so \
    rdbloadsave.so \
    crash.so \
    cluster.so \
    hellovalkey.so \
    helloscripting.so \
    unsupported_features.so \
    deferred_reply.so

# storage_mock_module registers a storage engine, so it needs data tiering.
# When BUILD_EXT_STORAGE is not passed in, fall back to how the server was
# built, recorded in src/.make-settings.
ifeq ($(BUILD_EXT_STORAGE),)
BUILD_EXT_STORAGE := $(shell sed -n 's/^BUILD_EXT_STORAGE=//p' ../../src/.make-settings 2>/dev/null)
endif
ifeq ($(BUILD_EXT_STORAGE),yes)
TEST_MODULES += storage_mock_module.so
endif

.PHONY: all

all: $(TEST_MODULES)

32bit:
	$(MAKE) CFLAGS="-m32" LDFLAGS="-m32"

%.xo: %.c ../../src/valkeymodule.h
	$(CC) -I../../src $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

%.so: %.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS)

# hellovalkey is the only test module that exercises the legacy redismodule.h
# include path (regression test for #547). The generic %.xo rule above only
# tracks valkeymodule.h, so declare the redismodule.h dependency explicitly.
hellovalkey.xo: ../../src/redismodule.h

hellovalkey.so: hellovalkey.xo hellovalkey.version
ifeq ($(uname_S),Linux)
	$(LD) -o $@ hellovalkey.xo $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS) -Wl,--version-script=hellovalkey.version
else
	$(LD) -o $@ hellovalkey.xo $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS)
endif

# storage_mock_module links the shared mock engine from src/storage. Both
# objects need the data tiering flag so the engine code compiles in.
EXT_STORAGE_CFLAGS = -DUSE_EXT_STORAGE=1 -I../../src/storage

storage_mock_module.xo: storage_mock_module.c ../../src/valkeymodule.h ../../src/storage/storage.h
	$(CC) -I../../src $(EXT_STORAGE_CFLAGS) $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

storage_mock.xo: ../../src/storage/storage_mock.c ../../src/storage/storage.h
	$(CC) -I../../src $(EXT_STORAGE_CFLAGS) $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

storage_mock_module.so: storage_mock_module.xo storage_mock.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(TEST_MODULES) $(TEST_MODULES:.so=.xo) storage_mock.xo
