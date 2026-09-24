BUILD ?= build
DESTDIR ?=
prefix ?= /usr/local
exec_prefix ?= $(prefix)
libdir ?= $(exec_prefix)/lib
includedir ?= $(prefix)/include
bindir ?= $(exec_prefix)/bin
sysconfdir ?= $(prefix)/etc
systemd_system_unitdir ?= $(libdir)/systemd/system

CFLAGS += -Wall -Wextra -Werror -std=gnu17 -pedantic -O3 -D_GNU_SOURCE -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 -fPIC
CXXFLAGS += -Wall -Wextra -Werror -std=gnu++20 -pedantic -O3 -I$(BUILD) -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 -fPIC

# Enable sanitizers by default
USE_SANITIZER ?= 1
ifeq ($(USE_SANITIZER), 1)
	CXXFLAGS += -fsanitize=address -fsanitize=undefined
	CFLAGS += -fsanitize=address -fsanitize=undefined
	LDFLAGS += -fsanitize=address -fsanitize=undefined
endif

# Enable clang-tidy checking by default
USE_CLANG_TIDY ?= 1
CLANG_TIDY ?= clang-tidy --config-file build-tools/clang-tidy.config

ifeq ($(abspath $(BUILD)),$(shell pwd))
$(error "ERROR: Build dir can't be equal to source dir")
endif

WITH_ATCLI ?=  1
ifeq ($(WITH_ATCLI), 1)
	ALL_TARGETS_BIN += atcli
endif

WITH_LIBOWL ?= 1
ifeq ($(WITH_LIBOWL), 1)
	ALL_TARGETS_LIB += libowl.so libowl.so.1
	ALL_TARGETS_INC += libowl.h
endif

WITH_OWLD ?= 1
ifeq ($(WITH_OWLD), 1)
ifneq ($(WITH_LIBOWL),1)
$(error "ERROR: owld requires WITH_LIBOWL=1")
endif
	ALL_TARGETS_BIN += owld
	ALL_TARGETS_SYSTEMD += owld.service
endif

WITH_OWL ?= 1
ifeq ($(WITH_OWL), 1)
ifneq ($(WITH_LIBOWL),1)
$(error "ERROR: owl requires WITH_LIBOWL=1")
endif
	ALL_TARGETS_BIN += owl
endif

.PHONY: all $(ALL_TARGETS_BIN)
all: $(ALL_TARGETS_BIN) $(ALL_TARGETS_SYSTEMD) $(ALL_TARGETS_LIB) $(ALL_TARGETS_INC)

$(ALL_TARGETS_BIN): %: $(BUILD)/%

$(ALL_TARGETS_SYSTEMD): %: $(BUILD)/%

$(ALL_TARGETS_LIB): %: $(BUILD)/%

# Disable implicit shells script rule
%: %.sh

$(BUILD)/atcli: atcli.py
	mkdir -p $(BUILD)
	install -m 0755 $< $@

$(BUILD)/libowl.so.1: $(BUILD)/libowl.o
	$(CC) -o $@ $^ -shared $(LDFLAGS) -lsqlite3

$(BUILD)/libowl.so: $(BUILD)/libowl.so.1
	ln -sf libowl.so.1 $@

$(BUILD)/owl: owl.py
	mkdir -p $(BUILD)
	install -m 0755 $< $@

$(BUILD)/owld: $(BUILD)/owld.o $(BUILD)/libowl.so
	$(CC) -o $@ $^ $(LDFLAGS) -L $(BUILD) -lcyaml -liio -lowl

$(BUILD)/%.o: %.c
ifeq ($(USE_CLANG_TIDY), 1)
	$(CLANG_TIDY) $< -- $(CFLAGS)
endif
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
ifeq ($(USE_CLANG_TIDY), 1)
	$(CLANG_TIDY) $< -- $(CXXFLAGS)
endif
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.service: %.service.in
	mkdir -p $(BUILD)
	sed \
		-e 's:@BINDIR@:${bindir}:g' \
		-e 's:@SYSCONFDIR@:${sysconfdir}:g' \
		$< > $@

.PHONY: clean
clean:
	rm -rf $(BUILD)

# Create prefixed phony targets to allow generic rules for installation
ALL_TARGETS_BIN_INSTALL = $(patsubst %, %.bin.install, $(ALL_TARGETS_BIN))
ALL_TARGETS_SYSTEMD_INSTALL = $(patsubst %, %.systemd.install, $(ALL_TARGETS_SYSTEMD))
ALL_TARGETS_LIB_INSTALL = $(patsubst %, %.lib.install, $(ALL_TARGETS_LIB))
ALL_TARGETS_INC_INSTALL = $(patsubst %, %.inc.install, $(ALL_TARGETS_INC))

.PHONY: install
install: $(ALL_TARGETS_BIN_INSTALL) $(ALL_TARGETS_SYSTEMD_INSTALL) $(ALL_TARGETS_LIB_INSTALL) $(ALL_TARGETS_INC_INSTALL)

.PHONY:
%.bin.install: $(BUILD)/%
	install -d $(DESTDIR)$(bindir)
	install -m 0755 $< $(DESTDIR)$(bindir)

.PHONY:
%.systemd.install: $(BUILD)/%
	install -d $(DESTDIR)$(systemd_system_unitdir)
	install -m 0644 $< $(DESTDIR)$(systemd_system_unitdir)

.PHONY:
%.lib.install: $(BUILD)/%
	install -d $(DESTDIR)$(libdir)
	# symlinks are dereferenced by install, use cp
	chmod 0644 $<
	cp -d $< $(DESTDIR)$(libdir)/

.PHONY:
%.inc.install: %
	install -d $(DESTDIR)$(includedir)
	install -m 0644 $< $(DESTDIR)$(includedir)

.PHONY: test
test: $(BUILD)/test-libowl.o $(BUILD)/libowl.so
	$(CXX) -o $(BUILD)/test-libowl $^ $(LDFLAGS) -L $(BUILD) -lCatch2Main -lCatch2 -lowl
	LD_LIBRARY_PATH=$(BUILD) $(BUILD)/test-libowl
