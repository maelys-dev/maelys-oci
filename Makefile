SHELL := /bin/sh
CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
# The integration suites use Python assertions as executable checks.
override export PYTHONOPTIMIZE :=
BUILD_PLATFORM := $(shell scripts/release-target.sh)
BUILD_ROOT := $(abspath build)
BUILD ?= $(BUILD_ROOT)/$(BUILD_PLATFORM)/release
# Resolve symlinks and refuse the source tree, build/ itself and paths outside
# it before any recipe can write or clean. Custom profiles remain supported.
build_directory = $(or $(shell $(PYTHON) scripts/build-directory.py "$(1)"),$(error Invalid build directory: $(1)))
override BUILD := $(call build_directory,$(BUILD))
PREFIX ?= /usr/local
DESTDIR ?=
VERSION := $(shell cat VERSION)

# ---- pinned sibling dependencies ---------------------------------------------
# dependencies/<name>.pin: line 1 the release tag, line 2 the immutable commit.
# scripts/checkout-dependency.sh fetches them in CI; the build verifies them here.
MAELYS_SYSTEM_PIN := $(shell sed -n '2p' dependencies/maelys-system.pin)
MAELYS_JSON_PIN := $(shell sed -n '2p' dependencies/maelys-json.pin)
MAELYS_HTTP_PIN := $(shell sed -n '2p' dependencies/maelys-http.pin)
MAELYS_CLI_PIN := $(shell sed -n '2p' dependencies/maelys-cli.pin)
MAELYS_RELEASE_DIR ?= ../maelys-release

# Each Maelys library is either built from its pinned checkout MAELYS_<X>_DIR
# (the default, what every gate runs) or taken already installed under
# MAELYS_<X>_PREFIX (packaging: the Homebrew formulas depend on libmaelys-sys,
# libmaelys-json and libmaelys-http). An installed library must carry the
# ABI this tree was written against and at least the pinned version; the
# checkout must be the pinned commit and carry that ABI. maelys-cli is always
# built from its checkout: the framework is linked into the terminal.
MAELYS_SYSTEM_ABI := 1u
MAELYS_JSON_ABI := 2u
MAELYS_HTTP_ABI := 1u
MAELYS_SYSTEM_VERSION := $(patsubst v%,%,$(shell sed -n '1p' dependencies/maelys-system.pin))
MAELYS_JSON_VERSION := $(patsubst v%,%,$(shell sed -n '1p' dependencies/maelys-json.pin))
MAELYS_HTTP_VERSION := $(patsubst v%,%,$(shell sed -n '1p' dependencies/maelys-http.pin))

MAELYS_SYSTEM_DIR ?= ../maelys-system
MAELYS_SYSTEM_PREFIX ?=
MAELYS_SYSTEM_BUILD ?= $(abspath $(BUILD)/deps/maelys-system)
override MAELYS_SYSTEM_BUILD := $(call build_directory,$(MAELYS_SYSTEM_BUILD))
ifeq ($(MAELYS_SYSTEM_PREFIX),)
MAELYS_SYSTEM_INCLUDE := $(MAELYS_SYSTEM_DIR)/include
MAELYS_SYSTEM_LIB := $(MAELYS_SYSTEM_BUILD)/lib/libmaelys_sys.a
else
MAELYS_SYSTEM_INCLUDE := $(MAELYS_SYSTEM_PREFIX)/include
MAELYS_SYSTEM_LIB := $(MAELYS_SYSTEM_PREFIX)/lib/libmaelys_sys.a
endif
MAELYS_JSON_DIR ?= ../maelys-json
MAELYS_JSON_PREFIX ?=
MAELYS_JSON_BUILD ?= $(abspath $(BUILD)/deps/maelys-json)
override MAELYS_JSON_BUILD := $(call build_directory,$(MAELYS_JSON_BUILD))
ifeq ($(MAELYS_JSON_PREFIX),)
MAELYS_JSON_INCLUDE := $(MAELYS_JSON_DIR)/include
MAELYS_JSON_LIB := $(MAELYS_JSON_BUILD)/lib/libmaelys-json.a
else
MAELYS_JSON_INCLUDE := $(MAELYS_JSON_PREFIX)/include
MAELYS_JSON_LIB := $(MAELYS_JSON_PREFIX)/lib/libmaelys-json.a
endif
MAELYS_HTTP_DIR ?= ../maelys-http
MAELYS_HTTP_PREFIX ?=
MAELYS_HTTP_BUILD ?= $(abspath $(BUILD)/deps/maelys-http)
override MAELYS_HTTP_BUILD := $(call build_directory,$(MAELYS_HTTP_BUILD))
ifeq ($(MAELYS_HTTP_PREFIX),)
MAELYS_HTTP_INCLUDE := $(MAELYS_HTTP_DIR)/include
MAELYS_HTTP_LIBDIR := $(MAELYS_HTTP_BUILD)
else
MAELYS_HTTP_INCLUDE := $(MAELYS_HTTP_PREFIX)/include
MAELYS_HTTP_LIBDIR := $(MAELYS_HTTP_PREFIX)/lib
endif
MAELYS_HTTP_CORE_LIB := $(MAELYS_HTTP_LIBDIR)/libmaelys_http.a
MAELYS_HTTP_CLIENT_LIB := $(MAELYS_HTTP_LIBDIR)/libmaelys_http_client.a
MAELYS_HTTP_TLS_LIB := $(MAELYS_HTTP_LIBDIR)/libmaelys_http_tls_mbedtls.a
MAELYS_HTTP_STAMP := $(BUILD)/deps/maelys-http.stamp
MAELYS_CLI_DIR ?= ../maelys-cli
MAELYS_CLI_BUILD ?= $(abspath $(BUILD)/deps/maelys-cli)
override MAELYS_CLI_BUILD := $(call build_directory,$(MAELYS_CLI_BUILD))
MAELYS_CLI_LIB := $(MAELYS_CLI_BUILD)/lib/libmaelys_cli.a
MAELYS_DISPATCHER := $(MAELYS_CLI_BUILD)/bin/maelys
MAELYS_CLI_STAMP := $(BUILD)/deps/maelys-cli.stamp
EMBED := $(MAELYS_CLI_DIR)/tools/maelys-cli-embed

# ---- system libraries ---------------------------------------------------------
PKG_CONFIG_PATH ?= /opt/homebrew/opt/libarchive/lib/pkgconfig:/opt/homebrew/opt/e2fsprogs/lib/pkgconfig
# External headers are system headers: our warning set is not theirs.
OCI_CFLAGS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --cflags libarchive ext2fs com_err 2>/dev/null | sed 's/-I/-isystem /g')
EXT2FS_VERSION ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --modversion ext2fs 2>/dev/null)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
OCI_LIBS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs libarchive 2>/dev/null) /opt/homebrew/opt/e2fsprogs/lib/libext2fs.2.1.dylib /opt/homebrew/opt/e2fsprogs/lib/libcom_err.1.1.dylib
# Homebrew's libarchive.pc lists private libraries such as zstd and lz4
# without their search directory. Keep that directory in our static-link
# metadata so installed consumers do not need ambient LDFLAGS. Inside a
# formula build, Homebrew's sandbox carries HOMEBREW_PREFIX and no `brew`.
HOMEBREW_PREFIX ?= $(shell brew --prefix 2>/dev/null)
PLATFORM_PRIVATE_LIBS := $(if $(HOMEBREW_PREFIX),-L$(HOMEBREW_PREFIX)/lib,)
POST_LINK = codesign --force --sign - --timestamp=none $@
PLATFORM_CPPFLAGS := -D_DARWIN_C_SOURCE
else
OCI_LIBS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs libarchive ext2fs com_err 2>/dev/null)
POST_LINK = :
PLATFORM_CPPFLAGS := -D_GNU_SOURCE
endif

# ---- Mbed TLS ----------------------------------------------------------------
# maelys-http refuses at compile time a Mbed TLS below its security floor
# (3.6.7 as of maelys-http 0.1.6). Linux distributions ship below it, so on
# Linux the upstream commit that dependencies/mbedtls.pin names is built from
# source, as maelys-http's own CI does; macOS takes Homebrew's, above the
# floor. MBEDTLS_SOURCE=pinned|system overrides the choice; pinned on macOS
# links against Homebrew's Mbed TLS whenever one is installed, because
# PLATFORM_PRIVATE_LIBS puts /opt/homebrew/lib ahead of the pinned prefix.
# The pinned build is static and private to this tree: the installed
# pkg-config file requires a consumer's Mbed TLS above the same floor
# (Requires.private) and names no search path of ours.
# An installed maelys-http was built against the Mbed TLS of its host, so a
# prefix build takes that one too.
ifneq ($(MAELYS_HTTP_PREFIX),)
MBEDTLS_SOURCE ?= system
else ifeq ($(UNAME_S),Darwin)
MBEDTLS_SOURCE ?= system
else
MBEDTLS_SOURCE ?= pinned
endif
MBEDTLS_DIR ?= ../mbedtls
MBEDTLS_PIN := $(shell sed -n '2p' dependencies/mbedtls.pin)
MBEDTLS_BUILD ?= $(abspath $(BUILD)/deps/mbedtls)
override MBEDTLS_BUILD := $(call build_directory,$(MBEDTLS_BUILD))
MBEDTLS_PREFIX := $(MBEDTLS_BUILD)/prefix
MBEDTLS_PC := $(MBEDTLS_PREFIX)/lib/pkgconfig/mbedtls.pc
# The floor maelys-http enforces, so the installed pkg-config file requires
# of a consumer's Mbed TLS exactly what the build required of ours: read
# from the pinned checkout's Makefile, or from the pkg-config file an
# installed maelys-http carries for its Mbed TLS provider. A build that
# cannot find it stops here, not at the rendering of the metadata.
ifeq ($(MAELYS_HTTP_PREFIX),)
MBEDTLS_MIN_VERSION := $(shell sed -n 's/^MBEDTLS_PKGCONFIG_MIN_VERSION ?= //p' $(MAELYS_HTTP_DIR)/Makefile 2>/dev/null)
else
MBEDTLS_MIN_VERSION := $(shell PKG_CONFIG_PATH=$(MAELYS_HTTP_PREFIX)/lib/pkgconfig $(PKG_CONFIG) \
	--print-requires-private maelys-http-tls-mbedtls 2>/dev/null | sed -n 's/^mbedtls *>= *//p' | head -n 1)
endif
ifeq ($(MBEDTLS_MIN_VERSION),)
$(error maelys-http declares no Mbed TLS floor: MAELYS_HTTP_DIR must name its pinned checkout or MAELYS_HTTP_PREFIX an installed maelys-http)
endif
ifeq ($(MBEDTLS_SOURCE),pinned)
MBEDTLS_DEP := $(MBEDTLS_PC)
MBEDTLS_PKG_CONFIG_PATH := $(MBEDTLS_PREFIX)/lib/pkgconfig
MBEDTLS_ENV := PKG_CONFIG_PATH=$(MBEDTLS_PKG_CONFIG_PATH)
HTTP_CFLAGS ?= -isystem $(MBEDTLS_PREFIX)/include
HTTP_LIBS ?= -L$(MBEDTLS_PREFIX)/lib -lmbedtls -lmbedx509 -lmbedcrypto
else
MBEDTLS_DEP :=
MBEDTLS_PKG_CONFIG_PATH :=
MBEDTLS_ENV :=
# Mbed TLS 3/4 ship pkg-config files; Debian's 2.28 does not and installs
# its libraries in the default search path.
HTTP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags mbedtls mbedx509 mbedcrypto 2>/dev/null | sed 's/-I/-isystem /g')
HTTP_LIBS ?= $(shell $(PKG_CONFIG) --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || echo "-lmbedtls -lmbedx509 -lmbedcrypto")
endif

# ---- flags -------------------------------------------------------------------------
# CFLAGS and LDFLAGS belong to the user (optimization, sanitizers); the
# language level, warnings and feature macros are fixed here.
CFLAGS ?= -O2 -g
LDFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
COMMON_CFLAGS := -std=c11 $(WARNINGS) -pthread
COMMON_CPPFLAGS := -Iinclude -I. -isystem $(MAELYS_SYSTEM_INCLUDE) \
	-isystem $(MAELYS_JSON_INCLUDE) -isystem $(MAELYS_HTTP_INCLUDE) \
	-isystem $(MAELYS_CLI_DIR)/include \
	-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE \
	$(PLATFORM_CPPFLAGS) \
	-DMAELYS_EXT2FS_VERSION='"$(EXT2FS_VERSION)"' \
	-DMAELYS_OCI_BUILD_VERSION='"$(VERSION)"'

# ---- sources -----------------------------------------------------------------------
OBJ := $(BUILD)/obj
BIN := $(BUILD)/bin
LIB := $(BUILD)/lib
GENERATED := $(BUILD)/generated
PC := $(LIB)/pkgconfig/maelys-oci.pc
MANIFEST := $(BUILD)/share/maelys/commands/oci.json
COMPLETIONS := $(BUILD)/share/completions/maelys-oci.bash \
	$(BUILD)/share/completions/_maelys-oci \
	$(BUILD)/share/completions/maelys-oci.fish

COMMON_SOURCES := src/common/error.c src/common/io.c src/common/sha256.c \
	src/common/descriptor.c src/common/config.c src/common/document.c
STORE_SOURCES := src/store/core.c src/store/seal.c src/store/artifact.c \
	src/store/blobs.c src/store/acquisition.c
MATERIALIZER_SOURCES := src/materializer/source.c src/materializer/graph.c \
	src/materializer/layer.c src/materializer/ext4.c \
	src/materializer/inspection.c src/materializer/migration.c \
	src/materializer/verification.c src/materializer/leases.c \
	src/materializer/gc.c src/materializer/import.c src/materializer/closure.c \
	src/materializer/api.c
PULLER_SOURCES := src/puller/reference.c src/puller/json.c \
	src/puller/registry.c src/puller/auth.c src/puller/http.c \
	src/puller/download.c src/puller/resolve.c src/puller/pull.c \
	src/puller/tls_version.c src/puller/api.c
CLI_SOURCES := cli/main.c
LIB_SOURCES := $(COMMON_SOURCES) $(STORE_SOURCES) $(MATERIALIZER_SOURCES) $(PULLER_SOURCES)
LIB_OBJECTS := $(LIB_SOURCES:%.c=$(OBJ)/%.o)
CLI_OBJECTS := $(CLI_SOURCES:%.c=$(OBJ)/%.o) \
	$(OBJ)/generated/schemas.o
CLI_SCHEMAS := $(wildcard cli/schemas/*.json)
CLI_SCHEMA_SYMBOLS := $(foreach schema,$(CLI_SCHEMAS),oci_$(subst -,_,$(basename $(notdir $(schema))))_schema=$(schema))
OCI_LIB := $(LIB)/libmaelys-oci.a
OCI_BIN := $(BIN)/maelys-oci
MBEDTLS_SECURITY_TEST := $(BUILD)/tests/check_mbedtls_version

.PHONY: all check dependencies check-dependencies clean dist install analyze \
	asan-ubsan sanitizers cli-check \
	socle-check agents-install agents-status check-mbedtls-security security-check

all: $(OCI_LIB) $(OCI_BIN) $(PC) $(MANIFEST) $(COMPLETIONS)

# ---- dependencies ------------------------------------------------------------------
dependencies: check-dependencies $(MAELYS_SYSTEM_LIB) $(MAELYS_JSON_LIB) \
	$(MAELYS_HTTP_TLS_LIB) $(MAELYS_CLI_LIB)

.PHONY: check-system check-json check-http check-cli
check-dependencies: check-system check-json check-http check-cli

# A checkout is the pinned commit, clean, and carries the ABI this tree was
# written against; an installed library carries that ABI and at least the
# pinned version. A pin bump rebuilds the dependency it names.
ifeq ($(MAELYS_SYSTEM_PREFIX),)
check-system:
	@test "$$(git -C $(MAELYS_SYSTEM_DIR) rev-parse HEAD)" = \
		"$(MAELYS_SYSTEM_PIN)"
	@git -C $(MAELYS_SYSTEM_DIR) diff --quiet $(MAELYS_SYSTEM_PIN) -- .
	@test -z "$$(git -C $(MAELYS_SYSTEM_DIR) ls-files --others --exclude-standard)"
	@grep -Fq '#define MAELYS_SYS_ABI_VERSION $(MAELYS_SYSTEM_ABI)' \
		$(MAELYS_SYSTEM_INCLUDE)/maelys/sys/version.h

$(MAELYS_SYSTEM_LIB): dependencies/maelys-system.pin | check-dependencies
	$(MAKE) -C $(MAELYS_SYSTEM_DIR) BUILD=$(MAELYS_SYSTEM_BUILD) all
else
check-system:
	@test -f $(MAELYS_SYSTEM_LIB) -a -f $(MAELYS_SYSTEM_INCLUDE)/maelys/sys/version.h || \
		{ echo "MAELYS_SYSTEM_PREFIX must hold an installed maelys-system" >&2; exit 1; }
	@grep -Fq '#define MAELYS_SYS_ABI_VERSION $(MAELYS_SYSTEM_ABI)' \
		$(MAELYS_SYSTEM_INCLUDE)/maelys/sys/version.h || \
		{ echo "installed maelys-system has another ABI than $(MAELYS_SYSTEM_ABI)" >&2; exit 1; }
	@installed=$$(sed -n 's/^#define MAELYS_SYS_VERSION "\(.*\)"$$/\1/p' \
		$(MAELYS_SYSTEM_INCLUDE)/maelys/sys/version.h); \
	oldest=$$(printf '%s\n%s\n' "$(MAELYS_SYSTEM_VERSION)" "$$installed" | \
		sort -t. -k1,1n -k2,2n -k3,3n | sed -n '1p'); \
	test "$$oldest" = "$(MAELYS_SYSTEM_VERSION)" || \
		{ echo "installed maelys-system $$installed is older than $(MAELYS_SYSTEM_VERSION)" >&2; exit 1; }
endif

ifeq ($(MAELYS_JSON_PREFIX),)
check-json:
	@test "$$(git -C $(MAELYS_JSON_DIR) rev-parse HEAD)" = "$(MAELYS_JSON_PIN)"
	@git -C $(MAELYS_JSON_DIR) diff --quiet $(MAELYS_JSON_PIN) -- .
	@test -z "$$(git -C $(MAELYS_JSON_DIR) ls-files --others --exclude-standard)"
	@grep -Fq '#define MAELYS_JSON_ABI_VERSION $(MAELYS_JSON_ABI)' \
		$(MAELYS_JSON_INCLUDE)/maelys/json.h

$(MAELYS_JSON_LIB): dependencies/maelys-json.pin | check-dependencies
	$(MAKE) -C $(MAELYS_JSON_DIR) BUILD=$(MAELYS_JSON_BUILD) all
else
check-json:
	@test -f $(MAELYS_JSON_LIB) -a -f $(MAELYS_JSON_INCLUDE)/maelys/json.h || \
		{ echo "MAELYS_JSON_PREFIX must hold an installed maelys-json" >&2; exit 1; }
	@grep -Fq '#define MAELYS_JSON_ABI_VERSION $(MAELYS_JSON_ABI)' \
		$(MAELYS_JSON_INCLUDE)/maelys/json.h || \
		{ echo "installed maelys-json has another ABI than $(MAELYS_JSON_ABI)" >&2; exit 1; }
	@installed=$$(PKG_CONFIG_PATH=$(MAELYS_JSON_PREFIX)/lib/pkgconfig \
		$(PKG_CONFIG) --modversion maelys-json); \
	oldest=$$(printf '%s\n%s\n' "$(MAELYS_JSON_VERSION)" "$$installed" | \
		sort -t. -k1,1n -k2,2n -k3,3n | sed -n '1p'); \
	test "$$oldest" = "$(MAELYS_JSON_VERSION)" || \
		{ echo "installed maelys-json $$installed is older than $(MAELYS_JSON_VERSION)" >&2; exit 1; }
endif

ifeq ($(MAELYS_HTTP_PREFIX),)
check-http:
	@test "$$(git -C $(MAELYS_HTTP_DIR) rev-parse HEAD)" = "$(MAELYS_HTTP_PIN)"
	@git -C $(MAELYS_HTTP_DIR) diff --quiet $(MAELYS_HTTP_PIN) -- .
	@test -z "$$(git -C $(MAELYS_HTTP_DIR) ls-files --others --exclude-standard)"
	@grep -Fq '#define MAELYS_HTTP_ABI_VERSION $(MAELYS_HTTP_ABI)' \
		$(MAELYS_HTTP_INCLUDE)/maelys/http.h
else
check-http:
	@test -f $(MAELYS_HTTP_CORE_LIB) -a -f $(MAELYS_HTTP_CLIENT_LIB) \
		-a -f $(MAELYS_HTTP_TLS_LIB) -a -f $(MAELYS_HTTP_INCLUDE)/maelys/http.h || \
		{ echo "MAELYS_HTTP_PREFIX must hold an installed maelys-http with its Mbed TLS provider" >&2; exit 1; }
	@grep -Fq '#define MAELYS_HTTP_ABI_VERSION $(MAELYS_HTTP_ABI)' \
		$(MAELYS_HTTP_INCLUDE)/maelys/http.h || \
		{ echo "installed maelys-http has another ABI than $(MAELYS_HTTP_ABI)" >&2; exit 1; }
	@installed=$$(PKG_CONFIG_PATH=$(MAELYS_HTTP_PREFIX)/lib/pkgconfig \
		$(PKG_CONFIG) --modversion maelys-http); \
	oldest=$$(printf '%s\n%s\n' "$(MAELYS_HTTP_VERSION)" "$$installed" | \
		sort -t. -k1,1n -k2,2n -k3,3n | sed -n '1p'); \
	test "$$oldest" = "$(MAELYS_HTTP_VERSION)" || \
		{ echo "installed maelys-http $$installed is older than $(MAELYS_HTTP_VERSION)" >&2; exit 1; }
endif

check-cli:
	@test "$$(git -C $(MAELYS_CLI_DIR) rev-parse HEAD)" = "$(MAELYS_CLI_PIN)"
	@git -C $(MAELYS_CLI_DIR) diff --quiet $(MAELYS_CLI_PIN) -- .
	@test -z "$$(git -C $(MAELYS_CLI_DIR) ls-files --others --exclude-standard)"

# The pinned Mbed TLS: a static build installed under this tree, verified
# against its pin like every other dependency, rebuilt when the pin moves.
$(MBEDTLS_PC): dependencies/mbedtls.pin | check-dependencies
	@test "$$(git -C $(MBEDTLS_DIR) rev-parse HEAD)" = "$(MBEDTLS_PIN)"
	rm -rf $(MBEDTLS_BUILD)
	cmake -S $(MBEDTLS_DIR) -B $(MBEDTLS_BUILD)/cmake \
		-DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(MBEDTLS_PREFIX)
	cmake --build $(MBEDTLS_BUILD)/cmake --parallel
	cmake --install $(MBEDTLS_BUILD)/cmake
	@test -f $@

HTTP_OUTPUTS := $(MAELYS_HTTP_TLS_LIB) $(MAELYS_HTTP_CLIENT_LIB) $(MAELYS_HTTP_CORE_LIB)
ifeq ($(MAELYS_HTTP_PREFIX),)
$(MAELYS_HTTP_STAMP): dependencies/maelys-http.pin $(MAELYS_SYSTEM_LIB) $(MBEDTLS_DEP) \
        $(if $(filter-out $(wildcard $(HTTP_OUTPUTS)),$(HTTP_OUTPUTS)),FORCE) | check-dependencies
	$(MBEDTLS_ENV) $(MAKE) -C $(MAELYS_HTTP_DIR) BUILD=$(MAELYS_HTTP_BUILD) \
		SYSTEM_DIR=$(abspath $(MAELYS_SYSTEM_DIR)) \
		SYSTEM_LIB=$(MAELYS_SYSTEM_LIB) all check-mbedtls
	@mkdir -p $(@D)
	@touch $@

$(HTTP_OUTPUTS): $(MAELYS_HTTP_STAMP)
	@test -f $@
	@touch $@
endif

# The framework core has no dependency; its dispatcher reads manifests
# through the maelys-json build of this tree.
CLI_OUTPUTS := $(MAELYS_CLI_LIB) $(MAELYS_DISPATCHER)
# One sub-make owns all outputs. Ordinary multi-target recipes run once per
# target under -j; stamps keep this compatible with macOS's GNU make 3.81.
# Rebuild the group if any output was removed, then make outputs newer than
# the stamp so an unchanged subsequent build does not relink the terminal.
$(MAELYS_CLI_STAMP): dependencies/maelys-cli.pin $(MAELYS_JSON_LIB) \
        $(if $(filter-out $(wildcard $(CLI_OUTPUTS)),$(CLI_OUTPUTS)),FORCE) | check-dependencies
	$(MAKE) -C $(MAELYS_CLI_DIR) BUILD=$(MAELYS_CLI_BUILD) \
		MAELYS_JSON_DIR=$(abspath $(MAELYS_JSON_DIR)) \
		MAELYS_JSON_CFLAGS=-I$(abspath $(MAELYS_JSON_INCLUDE)) \
		MAELYS_JSON_LIB=$(MAELYS_JSON_LIB) MAELYS_JSON_LIBS=$(MAELYS_JSON_LIB) all
	@mkdir -p $(@D)
	@touch $@

$(CLI_OUTPUTS): $(MAELYS_CLI_STAMP)
	@test -f $@
	@touch $@

.PHONY: FORCE
FORCE:

# ---- generated schemas -----------------------------------------------------------------
$(GENERATED)/schemas.c: $(CLI_SCHEMAS) $(EMBED) Makefile
	@mkdir -p $(@D)
	$(EMBED) $(CLI_SCHEMA_SYMBOLS) > $@.tmp
	mv $@.tmp $@

$(GENERATED)/schemas.h: $(CLI_SCHEMAS) $(EMBED) Makefile
	@mkdir -p $(@D)
	$(EMBED) --header $(CLI_SCHEMA_SYMBOLS) > $@.tmp
	mv $@.tmp $@

$(OBJ)/generated/schemas.o: $(GENERATED)/schemas.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(COMMON_CFLAGS) -c $< -o $@

# ---- objects ---------------------------------------------------------------------------
$(OBJ)/src/materializer/%.o: src/materializer/%.c VERSION | $(MAELYS_SYSTEM_LIB) $(MAELYS_JSON_LIB)
	@mkdir -p $(@D)
	@test -n "$(OCI_CFLAGS)" -a -n "$(OCI_LIBS)" || \
		{ echo "libarchive and e2fsprogs development files are required" >&2; exit 1; }
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(OCI_CFLAGS) $(CFLAGS) $(COMMON_CFLAGS) \
		-MMD -MP -c $< -o $@

$(OBJ)/src/puller/%.o: src/puller/%.c VERSION | $(MAELYS_HTTP_TLS_LIB) $(MAELYS_JSON_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(OCI_CFLAGS) $(HTTP_CFLAGS) $(CFLAGS) \
		$(COMMON_CFLAGS) -MMD -MP -c $< -o $@

# The terminal is a consumer of the public API: it compiles without -I., so
# an include of src/ fails here, not at review (docs/policies/layout.md of
# maelys-platform), and without the system libraries the library wraps.
CLI_CPPFLAGS := $(filter-out -I.,$(COMMON_CPPFLAGS))
$(OBJ)/cli/%.o: cli/%.c $(GENERATED)/schemas.h VERSION | $(MAELYS_CLI_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) -I$(GENERATED) \
		$(CFLAGS) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

$(OBJ)/%.o: %.c VERSION | $(MAELYS_SYSTEM_LIB) $(MAELYS_JSON_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

$(OCI_LIB): $(LIB_OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
	ZERO_AR_DATE=1 $(AR) rcs $@ $^

check-mbedtls-security: $(MBEDTLS_DEP)
	@mkdir -p $(BUILD)/tests
	$(CC) $(HTTP_CFLAGS) $(CFLAGS) $(COMMON_CFLAGS) -I. \
		src/puller/tls_version.c tests/security/check_mbedtls_version.c \
		$(HTTP_LIBS) $(LDFLAGS) \
		-o $(MBEDTLS_SECURITY_TEST)
	$(MBEDTLS_SECURITY_TEST)

$(OCI_BIN): $(CLI_OBJECTS) $(OCI_LIB) $(MAELYS_CLI_LIB) \
		$(MAELYS_HTTP_TLS_LIB) $(MAELYS_HTTP_CLIENT_LIB) \
		$(MAELYS_HTTP_CORE_LIB) $(MAELYS_JSON_LIB) $(MAELYS_SYSTEM_LIB) | \
		check-mbedtls-security
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(COMMON_CFLAGS) $^ $(OCI_LIBS) $(HTTP_LIBS) \
		$(LDFLAGS) -o $@
	$(POST_LINK)

# The extension manifest binds the installed executable by absolute path and
# digest. Render on every invocation: PREFIX and linker flags can change
# without any source timestamp changing. Identical bytes keep their mtime.
.PHONY: install-metadata
install-metadata: $(OCI_BIN)
	$(PYTHON) scripts/render-install-metadata.py --prefix="$(PREFIX)" \
		--version="$(VERSION)" --binary="$(OCI_BIN)" --pkgconfig="$(PC)" \
		--manifest="$(MANIFEST)" --mbedtls-min-version="$(MBEDTLS_MIN_VERSION)" \
		--private-libs="$(PLATFORM_PRIVATE_LIBS)"

$(PC) $(MANIFEST): | install-metadata
	@test -f $@

# Shell completion is generated from the catalog by the binary itself.
$(BUILD)/share/completions/maelys-oci.bash: $(OCI_BIN)
	@mkdir -p $(@D)
	$(OCI_BIN) completion bash > $@.tmp && mv $@.tmp $@

$(BUILD)/share/completions/_maelys-oci: $(OCI_BIN)
	@mkdir -p $(@D)
	$(OCI_BIN) completion zsh > $@.tmp && mv $@.tmp $@

$(BUILD)/share/completions/maelys-oci.fish: $(OCI_BIN)
	@mkdir -p $(@D)
	$(OCI_BIN) completion fish > $@.tmp && mv $@.tmp $@

# ---- verification ------------------------------------------------------------------------
# The library-level lease protocol, exercised by the store lifecycle test
# against the public header only.
LEASE_TEST := $(BUILD)/tests/test_lease
check: all cli-check socle-check public-check parser-check source-check \
		build-layout-check security-check reproducible-check writers-check $(LEASE_TEST)
	tests/test_oci_materializer.sh $(abspath $(OCI_BIN)) $(abspath $(LEASE_TEST))
	$(PYTHON) tests/test_oci_materializer_adversarial.py $(abspath $(OCI_BIN))
	$(PYTHON) tests/test_oci_integrity.py $(abspath $(OCI_BIN))
	$(PYTHON) tests/test_oci_registry_pull.py $(abspath $(OCI_BIN)) $(abspath $(OCI_BIN))
	$(PYTHON) tests/test_oci_pull_recovery.py $(abspath $(OCI_BIN)) $(abspath $(PUBLIC_TEST))
	$(PYTHON) tests/test_install.py $(BUILD)

$(LEASE_TEST): tests/lease/test_lease.c $(OCI_LIB) $(MAELYS_JSON_LIB) \
		$(MAELYS_SYSTEM_LIB) VERSION
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(OCI_CFLAGS) $(CFLAGS) $(COMMON_CFLAGS) \
		$< $(OCI_LIB) $(MAELYS_JSON_LIB) $(MAELYS_SYSTEM_LIB) $(OCI_LIBS) \
		$(LDFLAGS) -o $@
	$(POST_LINK)

cli-check: $(OCI_BIN) $(MAELYS_DISPATCHER)
	tests/test_oci_cli.sh $(abspath $(OCI_BIN)) $(MAELYS_DISPATCHER)

# The release socle (maelys-release) must not drift; checked whenever a
# checkout of it sits next to this repository.
socle-check:
	@if [ -x $(MAELYS_RELEASE_DIR)/bin/maelys-release ]; then \
		$(MAELYS_RELEASE_DIR)/bin/maelys-release check . --product maelys-oci; \
	else echo "socle-check: skipped ($(MAELYS_RELEASE_DIR) not found)"; fi

# docs/cli.md and docs/cli-contract.json belong to the release socle, which
# runs maelys-cli's generator at the pinned commit and reads docs/cli.reference
# for the build holding the programs: 'maelys-release adopt' writes them,
# 'check' compares them, and check-product.yml does it in this repository's CI.

# Public C consumer, C++ header, and parser mutation gate.
PUBLIC_TEST := $(BUILD)/tests/public_pull
PARSER_TEST := $(BUILD)/tests/parser_smoke
PULL_LINK_LIBS := $(OCI_LIB) $(MAELYS_HTTP_TLS_LIB) $(MAELYS_HTTP_CLIENT_LIB) \
    $(MAELYS_HTTP_CORE_LIB) $(MAELYS_JSON_LIB) $(MAELYS_SYSTEM_LIB)
$(PUBLIC_TEST): tests/public/pull.c $(PULL_LINK_LIBS)
	@mkdir -p $(@D)
	$(CC) $(COMMON_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $< $(PULL_LINK_LIBS) $(OCI_LIBS) $(HTTP_LIBS) $(LDFLAGS) -o $@
	$(POST_LINK)
# The store operations through the public header alone, as the terminal
# and any other consumer see them.
OPERATIONS_TEST := $(BUILD)/tests/public_operations
$(OPERATIONS_TEST): tests/public/operations.c $(PULL_LINK_LIBS)
	@mkdir -p $(@D)
	$(CC) $(CLI_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $< $(PULL_LINK_LIBS) $(OCI_LIBS) $(HTTP_LIBS) $(LDFLAGS) -o $@
	$(POST_LINK)
$(PARSER_TEST): tests/fuzz/parsers.c $(PULL_LINK_LIBS)
	@mkdir -p $(@D)
	$(CC) $(COMMON_CPPFLAGS) $(OCI_CFLAGS) $(HTTP_CFLAGS) $(CFLAGS) $(COMMON_CFLAGS) -DOCI_FUZZ_SMOKE $< $(PULL_LINK_LIBS) $(OCI_LIBS) $(HTTP_LIBS) $(LDFLAGS) -o $@
	$(POST_LINK)
.PHONY: public-check parser-check fuzz
public-check: $(PUBLIC_TEST) $(OPERATIONS_TEST) pkgconfig-check
	$(CXX) -std=c++11 -Wall -Wextra -Werror -Iinclude -fsyntax-only tests/public/header.cpp
	$(PUBLIC_TEST)
	@rm -rf $(BUILD)/tests/operations-scratch && mkdir -p $(BUILD)/tests/operations-scratch
	$(OPERATIONS_TEST) $(abspath $(BUILD)/tests/operations-scratch)
.PHONY: pkgconfig-check
pkgconfig-check: $(PC) $(PULL_LINK_LIBS)
	@mkdir -p $(BUILD)/public-stage/lib/pkgconfig $(BUILD)/public-stage/include/maelys
	install -m 0644 include/maelys/oci.h $(BUILD)/public-stage/include/maelys/oci.h
	sed 's|^prefix=.*|prefix=$(abspath $(BUILD))/public-stage|' $(PC) >$(BUILD)/public-stage/lib/pkgconfig/maelys-oci.pc
	@for archive in $(abspath $(PULL_LINK_LIBS)); do ln -sf "$$archive" $(BUILD)/public-stage/lib/; done
	$(CC) $(CFLAGS) $(COMMON_CFLAGS) tests/public/pull.c \
        $$(PKG_CONFIG_PATH=$(abspath $(BUILD))/public-stage/lib/pkgconfig:$(MBEDTLS_PKG_CONFIG_PATH):$(PKG_CONFIG_PATH) $(PKG_CONFIG) --static --cflags --libs maelys-oci) \
        $(LDFLAGS) -o $(BUILD)/public-stage/pull
	$(BUILD)/public-stage/pull
parser-check: $(PARSER_TEST)
	$(PARSER_TEST)

WRITERS_TEST := $(BUILD)/tests/writers
$(WRITERS_TEST): tests/materializer/test_writers.c $(OCI_LIB) $(MAELYS_JSON_LIB) $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(COMMON_CPPFLAGS) $(OCI_CFLAGS) $(CFLAGS) $(COMMON_CFLAGS) \
		$< $(OCI_LIB) $(MAELYS_JSON_LIB) $(MAELYS_SYSTEM_LIB) $(OCI_LIBS) $(LDFLAGS) -o $@
	$(POST_LINK)
.PHONY: writers-check
writers-check: $(WRITERS_TEST)
	$(WRITERS_TEST) $(BUILD)/tests
fuzz: all
	mkdir -p $(BUILD)/tests $(BUILD)/fuzz-corpus
	cp -R tests/fuzz/corpus/. $(BUILD)/fuzz-corpus/
	clang $(COMMON_CPPFLAGS) $(OCI_CFLAGS) $(HTTP_CFLAGS) -g -O1 \
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
        tests/fuzz/parsers.c $(LIB_SOURCES) \
        $(filter-out $(OCI_LIB),$(PULL_LINK_LIBS)) $(OCI_LIBS) $(HTTP_LIBS) -pthread -o $(BUILD)/tests/fuzz_parsers
	$(BUILD)/tests/fuzz_parsers -max_total_time=30 -max_len=65536 $(BUILD)/fuzz-corpus

.PHONY: source-check build-layout-check reproducible-check
source-check:
	$(PYTHON) tests/check_source.py
build-layout-check:
	$(PYTHON) tests/test_build_layout.py
	$(PYTHON) tests/test_build_dependencies.py
security-check: check-mbedtls-security
	$(PYTHON) tests/test_mbedtls_security.py
reproducible-check: $(OCI_LIB)
	@mkdir -p $(BUILD)/reproducible
	@rm -f $(BUILD)/reproducible/libmaelys-oci.a
	ZERO_AR_DATE=1 $(AR) rcs $(BUILD)/reproducible/libmaelys-oci.a $(LIB_OBJECTS)
	cmp $(OCI_LIB) $(BUILD)/reproducible/libmaelys-oci.a
	@echo "reproducible-check: identical archive from the same objects"

# Static analysis of every source; any analyzer diagnostic fails the target.
analyze: dependencies $(GENERATED)/schemas.h
	@mkdir -p $(BUILD)/analyze
	@set -e; for source in $(LIB_SOURCES) $(CLI_SOURCES) \
			tests/lease/test_lease.c; do \
		if ! clang $(CPPFLAGS) $(COMMON_CPPFLAGS) -I$(GENERATED) $(OCI_CFLAGS) \
			$(HTTP_CFLAGS) -std=c11 --analyze -Xanalyzer -analyzer-output=text \
			"$$source" >$(BUILD)/analyze/$$(basename "$$source").log 2>&1; then \
			cat $(BUILD)/analyze/$$(basename "$$source").log; exit 1; fi; \
		cat $(BUILD)/analyze/$$(basename "$$source").log; \
		! grep -q "warning:" $(BUILD)/analyze/$$(basename "$$source").log; \
	done
	@echo "analyze: ok"

# `asan-ubsan` is the name the Maelys family and the release socle use;
# `sanitizers` stays as an alias.
asan-ubsan:
	$(MAKE) BUILD=$(BUILD_ROOT)/$(BUILD_PLATFORM)/sanitizers CC=clang \
		CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined' \
		LDFLAGS='$(LDFLAGS) -fsanitize=address,undefined' check

sanitizers: asan-ubsan

# Agent instructions installed by the framework (managed block in AGENTS.md
# and CLAUDE.md, docs/maelys-cli-guide.md and the Claude skill).
agents-install: $(MAELYS_DISPATCHER)
	$(MAELYS_DISPATCHER) agents install . --apply

agents-status: $(MAELYS_DISPATCHER)
	$(MAELYS_DISPATCHER) agents status .

# ---- distribution --------------------------------------------------------------------------
# Packages this host's target the way the release socle does.
dist:
	BUILD="$(BUILD)" PYTHON="$(PYTHON)" scripts/package-release.sh $(BUILD_PLATFORM)

# The library (archive, header, pkg-config file) and the command (terminal,
# manifest, completions) install separately: the Homebrew formulas
# libmaelys-oci and maelys-oci take one each. `install` takes both.
.PHONY: install-library install-command
install: install-library install-command

install-library: all
	install -d $(DESTDIR)$(PREFIX)/include/maelys $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 0644 include/maelys/oci.h $(DESTDIR)$(PREFIX)/include/maelys/oci.h
	install -m 0644 $(OCI_LIB) $(DESTDIR)$(PREFIX)/lib/libmaelys-oci.a
	install -m 0644 $(PC) $(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-oci.pc

install-command: all
	install -d $(DESTDIR)$(PREFIX)/bin \
		$(DESTDIR)$(PREFIX)/share/maelys/commands \
		$(DESTDIR)$(PREFIX)/share/bash-completion/completions \
		$(DESTDIR)$(PREFIX)/share/zsh/site-functions \
		$(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d
	install -m 0755 $(OCI_BIN) $(DESTDIR)$(PREFIX)/bin/maelys-oci
	install -m 0644 $(MANIFEST) $(DESTDIR)$(PREFIX)/share/maelys/commands/oci.json
	install -m 0644 $(BUILD)/share/completions/maelys-oci.bash \
		$(DESTDIR)$(PREFIX)/share/bash-completion/completions/maelys-oci
	install -m 0644 $(BUILD)/share/completions/_maelys-oci \
		$(DESTDIR)$(PREFIX)/share/zsh/site-functions/_maelys-oci
	install -m 0644 $(BUILD)/share/completions/maelys-oci.fish \
		$(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/maelys-oci.fish

clean:
	rm -rf -- "$(BUILD)"

-include $(LIB_OBJECTS:.o=.d) $(CLI_SOURCES:%.c=$(OBJ)/%.d)
