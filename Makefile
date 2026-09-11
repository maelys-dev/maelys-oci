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

MAELYS_SYSTEM_DIR ?= ../maelys-system
MAELYS_SYSTEM_BUILD ?= $(abspath $(BUILD)/deps/maelys-system)
override MAELYS_SYSTEM_BUILD := $(call build_directory,$(MAELYS_SYSTEM_BUILD))
MAELYS_SYSTEM_LIB := $(MAELYS_SYSTEM_BUILD)/lib/libmaelys_sys.a
MAELYS_JSON_DIR ?= ../maelys-json
MAELYS_JSON_BUILD ?= $(abspath $(BUILD)/deps/maelys-json)
override MAELYS_JSON_BUILD := $(call build_directory,$(MAELYS_JSON_BUILD))
MAELYS_JSON_LIB := $(MAELYS_JSON_BUILD)/lib/libmaelys-json.a
MAELYS_HTTP_DIR ?= ../maelys-http
MAELYS_HTTP_BUILD ?= $(abspath $(BUILD)/deps/maelys-http)
override MAELYS_HTTP_BUILD := $(call build_directory,$(MAELYS_HTTP_BUILD))
MAELYS_HTTP_CORE_LIB := $(MAELYS_HTTP_BUILD)/libmaelys_http.a
MAELYS_HTTP_CLIENT_LIB := $(MAELYS_HTTP_BUILD)/libmaelys_http_client.a
MAELYS_HTTP_TLS_LIB := $(MAELYS_HTTP_BUILD)/libmaelys_http_tls_mbedtls.a
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
# Mbed TLS 3/4 ship pkg-config files; Debian's 2.28 does not and installs
# its libraries in the default search path.
HTTP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags mbedtls mbedx509 mbedcrypto 2>/dev/null | sed 's/-I/-isystem /g')
HTTP_LIBS ?= $(shell $(PKG_CONFIG) --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || echo "-lmbedtls -lmbedx509 -lmbedcrypto")
EXT2FS_VERSION ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --modversion ext2fs 2>/dev/null)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
OCI_LIBS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs libarchive 2>/dev/null) /opt/homebrew/opt/e2fsprogs/lib/libext2fs.2.1.dylib /opt/homebrew/opt/e2fsprogs/lib/libcom_err.1.1.dylib
# Homebrew's libarchive.pc lists private libraries such as zstd and lz4
# without their search directory. Keep that directory in our static-link
# metadata so installed consumers do not need ambient LDFLAGS.
PLATFORM_PRIVATE_LIBS := -L$(shell brew --prefix)/lib
POST_LINK = codesign --force --sign - --timestamp=none $@
PLATFORM_CPPFLAGS := -D_DARWIN_C_SOURCE
else
OCI_LIBS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs libarchive ext2fs com_err 2>/dev/null)
POST_LINK = :
PLATFORM_CPPFLAGS := -D_GNU_SOURCE
endif

# ---- flags -------------------------------------------------------------------------
# CFLAGS and LDFLAGS belong to the user (optimization, sanitizers); the
# language level, warnings and feature macros are fixed here.
CFLAGS ?= -O2 -g
LDFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
COMMON_CFLAGS := -std=c11 $(WARNINGS) -pthread
COMMON_CPPFLAGS := -Iinclude -I. -isystem $(MAELYS_SYSTEM_DIR)/include \
	-isystem $(MAELYS_JSON_DIR)/include -isystem $(MAELYS_HTTP_DIR)/include \
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

check-dependencies:
	@test "$$(git -C $(MAELYS_SYSTEM_DIR) rev-parse HEAD)" = \
		"$(MAELYS_SYSTEM_PIN)"
	@git -C $(MAELYS_SYSTEM_DIR) diff --quiet $(MAELYS_SYSTEM_PIN) -- .
	@test -z "$$(git -C $(MAELYS_SYSTEM_DIR) ls-files --others --exclude-standard)"
	@test "$$(git -C $(MAELYS_JSON_DIR) rev-parse HEAD)" = "$(MAELYS_JSON_PIN)"
	@git -C $(MAELYS_JSON_DIR) diff --quiet $(MAELYS_JSON_PIN) -- .
	@test -z "$$(git -C $(MAELYS_JSON_DIR) ls-files --others --exclude-standard)"
	@test "$$(git -C $(MAELYS_HTTP_DIR) rev-parse HEAD)" = "$(MAELYS_HTTP_PIN)"
	@git -C $(MAELYS_HTTP_DIR) diff --quiet $(MAELYS_HTTP_PIN) -- .
	@test -z "$$(git -C $(MAELYS_HTTP_DIR) ls-files --others --exclude-standard)"
	@test "$$(git -C $(MAELYS_CLI_DIR) rev-parse HEAD)" = "$(MAELYS_CLI_PIN)"
	@git -C $(MAELYS_CLI_DIR) diff --quiet $(MAELYS_CLI_PIN) -- .
	@test -z "$$(git -C $(MAELYS_CLI_DIR) ls-files --others --exclude-standard)"

# A pin bump rebuilds the dependency it names.
$(MAELYS_SYSTEM_LIB): dependencies/maelys-system.pin | check-dependencies
	$(MAKE) -C $(MAELYS_SYSTEM_DIR) BUILD=$(MAELYS_SYSTEM_BUILD) all

$(MAELYS_JSON_LIB): dependencies/maelys-json.pin | check-dependencies
	$(MAKE) -C $(MAELYS_JSON_DIR) BUILD=$(MAELYS_JSON_BUILD) all

HTTP_OUTPUTS := $(MAELYS_HTTP_TLS_LIB) $(MAELYS_HTTP_CLIENT_LIB) $(MAELYS_HTTP_CORE_LIB)
$(MAELYS_HTTP_STAMP): dependencies/maelys-http.pin $(MAELYS_SYSTEM_LIB) \
        $(if $(filter-out $(wildcard $(HTTP_OUTPUTS)),$(HTTP_OUTPUTS)),FORCE) | check-dependencies
	$(MAKE) -C $(MAELYS_HTTP_DIR) BUILD=$(MAELYS_HTTP_BUILD) \
		SYSTEM_DIR=$(abspath $(MAELYS_SYSTEM_DIR)) \
		SYSTEM_LIB=$(MAELYS_SYSTEM_LIB) all check-mbedtls
	@mkdir -p $(@D)
	@touch $@

$(HTTP_OUTPUTS): $(MAELYS_HTTP_STAMP)
	@test -f $@
	@touch $@

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
		MAELYS_JSON_LIB=$(MAELYS_JSON_LIB) all
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

check-mbedtls-security:
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
		--manifest="$(MANIFEST)" --http-libs="$(HTTP_LIBS)" \
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
        $$(PKG_CONFIG_PATH=$(abspath $(BUILD))/public-stage/lib/pkgconfig:$(PKG_CONFIG_PATH) $(PKG_CONFIG) --static --cflags --libs maelys-oci) \
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

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/include/maelys \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig \
		$(DESTDIR)$(PREFIX)/share/maelys/commands \
		$(DESTDIR)$(PREFIX)/share/bash-completion/completions \
		$(DESTDIR)$(PREFIX)/share/zsh/site-functions \
		$(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d
	install -m 0755 $(OCI_BIN) $(DESTDIR)$(PREFIX)/bin/maelys-oci
	install -m 0644 include/maelys/oci.h $(DESTDIR)$(PREFIX)/include/maelys/oci.h
	install -m 0644 $(OCI_LIB) $(DESTDIR)$(PREFIX)/lib/libmaelys-oci.a
	install -m 0644 $(PC) $(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-oci.pc
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
