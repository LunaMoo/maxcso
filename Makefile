SRCDIR := $(abspath $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST))))))

USE_EXTERNAL_LIBDEFLATE ?= 0

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man

CC ?= gcc
CXX ?= g++
AR ?= ar

CFLAGS ?= -O2
CXXFLAGS ?= $(CFLAGS)

PKG_CONFIG ?= pkg-config

CFLAGS_UV = $(shell $(PKG_CONFIG) --cflags libuv)
LIBS_UV = $(shell $(PKG_CONFIG) --libs libuv)

CFLAGS_LZ4 = $(shell $(PKG_CONFIG) --cflags liblz4)
LIBS_LZ4 = $(shell $(PKG_CONFIG) --libs liblz4)

CFLAGS_ZLIB = $(shell $(PKG_CONFIG) --cflags zlib)
LIBS_ZLIB = $(shell $(PKG_CONFIG) --libs zlib)

DEP_FLAGS := $(CFLAGS_UV) $(CFLAGS_LZ4) $(CFLAGS_ZLIB)
LIBS := $(LIBS_UV) $(LIBS_LZ4) $(LIBS_ZLIB)

OBJDIR := obj
MKDIRS := $(OBJDIR)/src $(OBJDIR)/cli $(OBJDIR)/capi $(OBJDIR)/zopfli/src/zopfli

# Everything is built position independent, so the same objects can go into mCSO.so.
ifeq ($(OS),Windows_NT)
	PIC_FLAGS :=
else
	PIC_FLAGS := -fPIC
endif

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
	SHARED_LIB := mCSO.dylib
	SHARED_LIB_FLAGS = -dynamiclib -install_name @rpath/$(SHARED_LIB) \
		-Wl,-exported_symbols_list,$(SRCDIR)/capi/exports.txt
else
	SHARED_LIB := mCSO.so
	SHARED_LIB_FLAGS = -shared -Wl,-soname,$(SHARED_LIB) \
		-Wl,--version-script=$(SRCDIR)/capi/exports.map -Wl,--no-undefined
endif
STATIC_LIB := mCSO.a

SRC_CFLAGS += -W -Wall -Wextra -Wno-implicit-function-declaration -DNDEBUG=1 $(PIC_FLAGS)
SRC_CXXFLAGS += -W -Wall -Wextra -std=c++11 -I$(SRCDIR)/zopfli/src -I$(SRCDIR)/7zip \
	-DNDEBUG=1 -I$(SRCDIR)/libdeflate -Wno-unused-parameter -Wno-unused-variable \
	-pthread $(PIC_FLAGS) $(DEP_FLAGS)

SRC_CXX_SRC := $(wildcard $(SRCDIR)/src/*.cpp)
SRC_CXX_TMP := $(SRC_CXX_SRC:.cpp=.o)
SRC_CXX_OBJ := $(patsubst $(SRCDIR)/%,$(OBJDIR)/%,$(SRC_CXX_TMP))

CAPI_CXX_SRC := $(wildcard $(SRCDIR)/capi/*.cpp)
CAPI_CXX_TMP := $(CAPI_CXX_SRC:.cpp=.o)
CAPI_CXX_OBJ := $(patsubst $(SRCDIR)/%,$(OBJDIR)/%,$(CAPI_CXX_TMP))

CLI_CXX_SRC := $(wildcard $(SRCDIR)/cli/*.cpp)
CLI_CXX_TMP := $(CLI_CXX_SRC:.cpp=.o)
CLI_CXX_OBJ := $(patsubst $(SRCDIR)/%,$(OBJDIR)/%,$(CLI_CXX_TMP))

ZOPFLI_C_DIR := $(SRCDIR)/zopfli/src/zopfli
ZOPFLI_C_SRC := $(ZOPFLI_C_DIR)/blocksplitter.c $(ZOPFLI_C_DIR)/cache.c \
               $(ZOPFLI_C_DIR)/deflate.c $(ZOPFLI_C_DIR)/gzip_container.c \
               $(ZOPFLI_C_DIR)/hash.c $(ZOPFLI_C_DIR)/katajainen.c \
               $(ZOPFLI_C_DIR)/lz77.c $(ZOPFLI_C_DIR)/squeeze.c \
               $(ZOPFLI_C_DIR)/tree.c $(ZOPFLI_C_DIR)/util.c \
               $(ZOPFLI_C_DIR)/zlib_container.c $(ZOPFLI_C_DIR)/zopfli_lib.c
ZOPFLI_C_TMP := $(ZOPFLI_C_SRC:.c=.o)
ZOPFLI_C_OBJ := $(patsubst $(SRCDIR)/%,$(OBJDIR)/%,$(ZOPFLI_C_TMP))

ifeq ($(OS),Windows_NT)
	LIBDEFLATE := libdeflatestatic.lib
	LIBS += -luuid
else
	LIBDEFLATE := libdeflate.a
endif

SRC_7ZIP := $(OBJDIR)/7zip/7zip.a
SRC_LIBDEFLATE := $(SRCDIR)/libdeflate/$(LIBDEFLATE)

OBJS := $(SRC_CXX_OBJ) $(CLI_CXX_OBJ) $(ZOPFLI_C_OBJ) $(SRC_7ZIP)
LIB_OBJS := $(SRC_CXX_OBJ) $(CAPI_CXX_OBJ) $(ZOPFLI_C_OBJ)
LIB_ARCHIVES := $(SRC_7ZIP)

ifeq ($(USE_EXTERNAL_LIBDEFLATE), 0)
	OBJS += $(SRC_LIBDEFLATE)
	LIB_ARCHIVES += $(SRC_LIBDEFLATE)
else
	CFLAGS_LIBDEFLATE = $(shell $(PKG_CONFIG) --cflags libdeflate)
	LIBS_LIBDEFLATE = $(shell $(PKG_CONFIG) --libs libdeflate)
	SRC_CXXFLAGS += $(CFLAGS_LIBDEFLATE)
	LIBS += $(LIBS_LIBDEFLATE)
endif

.PHONY: all lib clean install uninstall

all: maxcso

# The library with the C API from capi/maxcso_capi.h.
lib: $(STATIC_LIB) $(SHARED_LIB)

$(OBJDIR)/capi/%.o: $(SRCDIR)/capi/%.cpp $(OBJDIR)/.done
	$(CXX) -c $(SRC_CXXFLAGS) -DMAXCSO_CAPI_BUILD $(CXXFLAGS) -o $@ $<

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(OBJDIR)/.done
	$(CXX) -c $(SRC_CXXFLAGS) $(CXXFLAGS) -o $@ $<

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(OBJDIR)/.done
	$(CC) -c $(SRC_CFLAGS) $(CFLAGS) -o $@ $<

maxcso: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(SRC_CXXFLAGS) $(CXXFLAGS) $^ $(LIBS)

$(SHARED_LIB): $(LIB_OBJS) $(LIB_ARCHIVES)
	$(CXX) $(LDFLAGS) $(SHARED_LIB_FLAGS) -o $@ $(SRC_CXXFLAGS) $(CXXFLAGS) $^ $(LIBS)

# A static library that carries the bundled 7-zip and libdeflate objects too, so users only
# need to link the external dependencies (libuv, lz4, zlib).
$(STATIC_LIB): $(LIB_OBJS) $(LIB_ARCHIVES)
	rm -rf $(OBJDIR)/staticlib && mkdir -p $(OBJDIR)/staticlib
	for a in $(abspath $(LIB_ARCHIVES)); do \
		d=$(OBJDIR)/staticlib/$$(basename $$a .a); mkdir -p $$d && (cd $$d && $(AR) x $$a) || exit 1; \
	done
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJS) $(OBJDIR)/staticlib/*/*.o

$(SRC_7ZIP):
	$(MAKE) -f $(SRCDIR)/7zip/Makefile 7zip.a CFLAGS="$(CFLAGS) $(PIC_FLAGS)" CXXFLAGS="$(CXXFLAGS) $(PIC_FLAGS)"

$(SRC_LIBDEFLATE):
	$(MAKE) -C $(SRCDIR)/libdeflate $(LIBDEFLATE) CFLAGS="$(CFLAGS) $(PIC_FLAGS)"

$(OBJDIR)/.done:
	@mkdir -p $(MKDIRS)
	@touch $@

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp maxcso $(DESTDIR)$(BINDIR)
	cp $(SRCDIR)/maxcso.1 $(DESTDIR)$(MANDIR)/man1
	chmod 0755 $(DESTDIR)$(BINDIR)/maxcso
	chmod 0644 $(DESTDIR)$(MANDIR)/man1/maxcso.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/maxcso
	rm -f $(DESTDIR)$(MANDIR)/man1/maxcso.1

clean:
	rm -rf -- $(OBJDIR)
	rm -f maxcso $(STATIC_LIB) $(SHARED_LIB)
	$(MAKE) -C $(SRCDIR)/libdeflate clean
