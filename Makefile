CXX      ?= g++
CXXFLAGS := -std=gnu++23 -Os -s -fno-exceptions -fno-rtti -Wall -Wextra
DBGFLAGS := -std=gnu++23 -O1 -g -fsanitize=address,undefined -Wall -Wextra
BIN      := yrs-cli
INSTALL_DIR ?= $(HOME)/.cargo/bin

.PHONY: all test install clean

all: $(BIN)

$(BIN): src/main.cpp src/yrs.hpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o $(BIN)

test: $(BIN)
	$(CXX) $(DBGFLAGS) tests/test_all.cpp -o /tmp/yrs_test
	/tmp/yrs_test ./$(BIN)
	rm -f /tmp/yrs_test

install: $(BIN)
	install -m 0755 $(BIN) $(INSTALL_DIR)/$(BIN)

clean:
	rm -f $(BIN) /tmp/yrs_test
