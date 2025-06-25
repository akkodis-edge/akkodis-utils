BUILD ?= build
DESTDIR ?=
prefix ?= /usr/local
exec_prefix ?= $(prefix)
libdir ?= $(exec_prefix)/lib
bindir ?= $(exec_prefix)/bin
sysconfdir ?= $(prefix)/etc
systemd_system_unitdir ?= $(libdir)/systemd/system

CFLAGS += -Wall -Wextra -Werror -std=gnu17 -pedantic -O3 -D_GNU_SOURCE
CXXFLAGS += -Wall -Wextra -Werror -std=gnu++20 -pedantic -O3 -I$(BUILD)

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

# Always build targets without additional dependencies
ALL_TARGETS_BIN = atcli

.PHONY: all $(ALL_TARGETS_BIN)
all: $(ALL_TARGETS_BIN)

$(ALL_TARGETS_BIN): %: $(BUILD)/%

# Disable implicit shells script rule
%: %.sh

$(BUILD)/atcli: atcli.py
	mkdir -p $(BUILD)
	install -m 0755 $< $@

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

.PHONY: clean
clean:
	rm -rf $(BUILD)

# Create prefixed phony targets to allow generic rules for installation
ALL_TARGETS_BIN_INSTALL = $(patsubst %, %.bin.install, $(ALL_TARGETS_BIN))

.PHONY: install
install: $(ALL_TARGETS_BIN_INSTALL)

.PHONY:
%.bin.install: $(BUILD)/%
	install -d $(DESTDIR)$(bindir)
	install -m 0755 $< $(DESTDIR)$(bindir)
