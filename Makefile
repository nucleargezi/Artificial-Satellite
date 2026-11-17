CXX      ?= g++
CXXFLAGS := -std=gnu++23 -Os -flto=auto -s -fno-exceptions -fno-rtti -Wall -Wextra -Isrc
DBGFLAGS := -std=gnu++23 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Isrc
DEPFLAGS := -MMD -MP
BIN      := yrs-cli
INSTALL_DIR ?= $(HOME)/.cargo/bin

ifeq ($(filter -j%,$(MAKEFLAGS)),)
NPROC := $(shell nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROC)
endif

LIB_SRCS := \
	src/core/str_util.cpp \
	src/core/fs_util.cpp \
	src/codec/gzip.cpp \
	src/codec/json.cpp \
	src/sys/clipboard.cpp \
	src/config.cpp \
	src/bundle.cpp \
	src/archive.cpp \
	src/cph/oj.cpp \
	src/cph/meta.cpp \
	src/cph/workflow.cpp

MAIN_SRC := src/main.cpp
TEST_SRC := tests/test_all.cpp

REL_OBJS := $(patsubst src/%.cpp,build/rel/%.o,$(LIB_SRCS) $(MAIN_SRC))
SAN_LIB_OBJS := $(patsubst src/%.cpp,build/san/%.o,$(LIB_SRCS))
SAN_TEST_OBJ := build/san/tests/test_all.o
SAN_BIN := build/san/yrs_test
DEPS := $(REL_OBJS:.o=.d) $(SAN_LIB_OBJS:.o=.d) $(SAN_TEST_OBJ:.o=.d)

.PHONY: all test install clean

all: $(BIN)

$(BIN): $(REL_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

build/rel/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(SAN_BIN): $(SAN_LIB_OBJS) $(SAN_TEST_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(DBGFLAGS) $^ -o $@

build/san/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(DBGFLAGS) $(DEPFLAGS) -c $< -o $@

$(SAN_TEST_OBJ): $(TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(DBGFLAGS) $(DEPFLAGS) -c $< -o $@

test: $(BIN) $(SAN_BIN)
	./$(SAN_BIN) ./$(BIN)

install: $(BIN)
	install -m 0755 $(BIN) $(INSTALL_DIR)/$(BIN)

clean:
	rm -rf $(BIN) build /tmp/yrs_test

-include $(DEPS)
