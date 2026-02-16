# ===== User settings =====
# IgH EtherCAT Master installed here:
ETHERCAT_HOME ?= /opt/etherlab

# EtherCAT include/lib directories (override if yours differ)
ETHERCAT_INCLUDEDIR ?= $(ETHERCAT_HOME)/include
ETHERCAT_LIBDIR     ?= $(ETHERCAT_HOME)/lib

# Use RTDM variant of the EtherCAT userspace lib? (0 = no, 1 = yes)
USE_RTDM ?= 0

# Path to ethercat CLI (used by 'make validate' script). Adjust if needed.
ETHERCAT_CLI ?= $(ETHERCAT_HOME)/bin/ethercat

# ===== Toolchain =====
CC      := gcc
PKGCFG  := pkg-config

# Jansson flags via pkg-config (falls back to -ljansson if pkg-config missing)
JANSSON_CFLAGS := $(shell $(PKGCFG) --cflags jansson 2>/dev/null)
JANSSON_LIBS   := $(shell $(PKGCFG) --libs   jansson 2>/dev/null)
ifeq ($(JANSSON_LIBS),)
  JANSSON_LIBS := -ljansson
endif

CFLAGS  := -O2 -g -Wall -Wextra -fPIC -I$(ETHERCAT_INCLUDEDIR) $(JANSSON_CFLAGS)
LDFLAGS := -L$(ETHERCAT_LIBDIR) -Wl,-rpath,$(ETHERCAT_LIBDIR)
ifeq ($(USE_RTDM),1)
  LDLIBS  := -lethercat_rtdm $(JANSSON_LIBS)
else
  LDLIBS  := -lethercat $(JANSSON_LIBS)
endif

# ===== Targets =====
BINARIES := ecat_configurator ecat_diag

all: $(BINARIES)

ecat_configurator: ecat_configurator.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

ecat_diag: ecat_diag.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# Optional: quick validation recipe (runs diag with your JSON and ethercat CLI)
validate: ecat_diag
	@echo ">>> Running ecat_diag (configure+validate) ..."
	sudo ./ecat_diag ./ecat_pdo_config.json --with-cli "$(ETHERCAT_CLI)"

clean:
	rm -f *.o $(BINARIES)

.PHONY: all clean validate
