CC ?= cc
AR ?= ar
BUILD ?= build
CPPFLAGS += -Iinclude
CFLAGS ?= -O3
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow

.PHONY: all archive clean
all: $(BUILD)/kivrim $(BUILD)/kivrim-archive $(BUILD)/libkivrim.a
archive: $(BUILD)/kivrim-archive

$(BUILD):
	mkdir -p $@

$(BUILD)/kivrim.o: src/kivrim.c include/kivrim.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libkivrim.a: $(BUILD)/kivrim.o
	$(AR) rcs $@ $<

$(BUILD)/kivrim: src/cli.c $(BUILD)/kivrim.o include/kivrim.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/cli.c $(BUILD)/kivrim.o $(LDFLAGS) -o $@

$(BUILD)/kivrim-archive: src/archive.c $(BUILD)/kivrim.o include/kivrim.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/archive.c $(BUILD)/kivrim.o $(LDFLAGS) -o $@

clean:
	rm -rf -- "$(BUILD)"
