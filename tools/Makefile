# ===== User settings =====
ETHERCAT_HOME       ?= /opt/etherlab
ETHERCAT_INCLUDEDIR ?= $(ETHERCAT_HOME)/include
ETHERCAT_LIBDIR     ?= $(ETHERCAT_HOME)/lib
USE_RTDM            ?= 0

# ===== Toolchain =====
CC      := gcc
PKGCFG  := pkg-config

JANSSON_CFLAGS := $(shell $(PKGCFG) --cflags jansson 2>/dev/null)
JANSSON_LIBS   := $(shell $(PKGCFG) --libs   jansson 2>/dev/null)
ifeq ($(JANSSON_LIBS),)
  JANSSON_LIBS := -ljansson
endif

CFLAGS  := -O2 -g -Wall -Wextra -fPIC -I$(ETHERCAT_INCLUDEDIR) $(JANSSON_CFLAGS) -DDEBUG_SDO 
LDFLAGS := -L$(ETHERCAT_LIBDIR) -Wl,-rpath,$(ETHERCAT_LIBDIR)

ifeq ($(USE_RTDM),1)
  LDLIBS  := -lethercat_rtdm $(JANSSON_LIBS)
else
  LDLIBS  := -lethercat $(JANSSON_LIBS)
endif

BIN := ecat_cfgdiag

all: $(BIN)

ecat_cfgdiag: ecat_cfgdiag.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

ecat_liveviewer: ecat_liveviewer.o
	$(CC) $^ -o $@ -lncurses \
        $(LDFLAGS) $(LDLIBS)

ecat_dump_raw: ecat_dump_raw.o
	$(CC) $^ -o $@ -lncurses \
        $(LDFLAGS) $(LDLIBS)

clean:
	rm -f *.o $(BIN)

.PHONY: all clean
