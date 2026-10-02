CC ?= cc
CLANG_FORMAT ?= clang-format
BLACK ?= black
VERSION := $(strip $(shell cat VERSION))
CPPFLAGS ?=
CFLAGS ?= -O2 -g
DRF_CPPFLAGS = -Iinclude -DDRF_VERSION=\"$(VERSION)\"
DRF_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-Wformat=2 -Werror=format-security -fstack-protector-strong \
	-fstack-clash-protection -fPIE
TARGET_MACHINE := $(shell $(CC) -dumpmachine)
ifneq ($(filter x86_64% i386% i486% i586% i686%,$(TARGET_MACHINE)),)
DRF_CFLAGS += -fcf-protection=full
endif
ifeq ($(findstring _FORTIFY_SOURCE,$(CPPFLAGS) $(CFLAGS)),)
DRF_CPPFLAGS += -D_FORTIFY_SOURCE=3
endif
LDFLAGS ?=
DRF_LDFLAGS = -Wl,-z,relro,-z,now,-z,noexecstack -pie
PREFIX ?= /usr/local

OBJ = build/main.o build/device.o build/protocol.o build/receiver.o \
      build/actions.o
TEST_OBJ = $(patsubst build/%.o,build/test/%.o,$(OBJ))
# Linker wrappers in the hardware tests require calls between object files.
TEST_CFLAGS = $(filter-out -flto%,$(CFLAGS)) $(DRF_CFLAGS) -fno-lto
TEST_LDFLAGS = $(filter-out -flto%,$(LDFLAGS)) $(DRF_LDFLAGS)
FORMAT_C = $(wildcard src/*.c include/*.h tests/*.c tests/fixtures/*.h)
FORMAT_PY = $(wildcard tests/*.py)

all: build/dell-rf

build:
	mkdir -p build

build/test:
	mkdir -p build/test

build/%.o: src/%.c VERSION | build
	$(CC) $(CPPFLAGS) $(DRF_CPPFLAGS) $(CFLAGS) $(DRF_CFLAGS) \
		-MMD -MP -c $< -o $@

build/test/%.o: src/%.c VERSION | build/test
	$(CC) $(CPPFLAGS) $(DRF_CPPFLAGS) $(TEST_CFLAGS) \
		-MMD -MP -c $< -o $@

build/dell-rf: $(OBJ)
	$(CC) $(CFLAGS) $(DRF_CFLAGS) $(OBJ) $(LDFLAGS) $(DRF_LDFLAGS) \
		-o $@

build/test_protocol: tests/test_protocol.c tests/fixtures/4503_queries.h \
                    build/test/protocol.o include/dell_rf_protocol.h | build
	$(CC) $(CPPFLAGS) $(DRF_CPPFLAGS) $(TEST_CFLAGS) \
		tests/test_protocol.c build/test/protocol.o $(TEST_LDFLAGS) -o $@

build/test_actions: tests/test_actions.c tests/fixtures/4503_actions.h \
                    tests/fixtures/4503_queries.h build/test/actions.o \
                    build/test/protocol.o include/dell_rf_actions.h | build
	$(CC) $(CPPFLAGS) $(DRF_CPPFLAGS) $(TEST_CFLAGS) \
		tests/test_actions.c build/test/actions.o build/test/protocol.o \
		$(TEST_LDFLAGS) -o $@

build/test_pair: tests/test_pair.c tests/fixtures/4503_actions.h \
                 tests/fixtures/4503_queries.h build/test/actions.o \
                 build/test/protocol.o include/dell_rf_actions.h | build
	$(CC) $(CPPFLAGS) $(DRF_CPPFLAGS) $(TEST_CFLAGS) \
		tests/test_pair.c build/test/actions.o build/test/protocol.o \
		$(TEST_LDFLAGS) -o $@

build/dell-rf-cli-test: tests/cli_mock.c tests/fixtures/4503_actions.h \
                       tests/fixtures/4503_queries.h $(TEST_OBJ) | build
	$(CC) $(CPPFLAGS) $(DRF_CPPFLAGS) $(TEST_CFLAGS) \
		tests/cli_mock.c $(TEST_OBJ) $(TEST_LDFLAGS) \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=poll \
		-Wl,--wrap=drf_probe_fd,--wrap=drf_read_descriptor_fd \
		-Wl,--wrap=drf_find_query_receiver,--wrap=drf_list_query_receivers \
		-Wl,--wrap=drf_receiver_pair \
		-Wl,--wrap=drf_receiver_unpair,--wrap=drf_receiver_batteries -o $@

build/test_receiver: tests/test_receiver.c tests/fixtures/4503_queries.h \
                     tests/fixtures/4503_actions.h \
                     build/test/receiver.o build/test/protocol.o build/test/actions.o \
                     include/dell_rf_protocol.h | build
	$(CC) $(CPPFLAGS) $(DRF_CPPFLAGS) $(TEST_CFLAGS) \
		tests/test_receiver.c \
		build/test/receiver.o build/test/protocol.o build/test/actions.o $(TEST_LDFLAGS) \
		-Wl,--wrap=open,--wrap=close,--wrap=flock,--wrap=ioctl \
		-Wl,--wrap=glob,--wrap=globfree,--wrap=drf_probe_fd \
		-Wl,--wrap=drf_read_descriptor_fd,--wrap=poll,--wrap=read \
		-Wl,--wrap=clock_gettime,--wrap=drf_unpair,--wrap=drf_pair \
		-Wl,--wrap=drf_read_snapshot -o $@

check: build/dell-rf build/test_protocol build/test_receiver \
       build/test_actions build/test_pair build/dell-rf-cli-test
	./build/test_protocol
	./build/test_receiver
	./build/test_actions
	./build/test_pair
	python3 -m unittest discover -s tests -p 'test_*.py'

format:
	$(CLANG_FORMAT) --style=file -i $(FORMAT_C)
	$(BLACK) --line-length 80 $(FORMAT_PY)

format-check:
	$(CLANG_FORMAT) --style=file --dry-run --Werror $(FORMAT_C)
	$(BLACK) --line-length 80 --check $(FORMAT_PY)

dist:
	bash packaging/source.sh

pkg-arch: dist
	bash packaging/build-arch.sh

pkg-deb: dist
	bash packaging/build-deb.sh

install: install-bin install-udev

install-bin: build/dell-rf
	install -Dm755 build/dell-rf $(DESTDIR)$(PREFIX)/bin/dell-rf

install-udev:
	install -Dm644 udev/70-dell-rf.rules \
		$(DESTDIR)/usr/lib/udev/rules.d/70-dell-rf.rules

clean:
	rm -rf build

.PHONY: all check format format-check dist pkg-arch pkg-deb \
        install install-bin install-udev clean

-include $(OBJ:.o=.d)
-include $(TEST_OBJ:.o=.d)
