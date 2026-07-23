CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
TARGET   := wordlist
TEST     := tests/test_wordlist

LIB_SRC  := convert.cpp
CLI_SRC  := main.cpp
TEST_SRC := tests/test_wordlist.cpp

ZSTD_DIR := third_party/zstd
ZSTD_LIB := $(ZSTD_DIR)/libzstd.a
CPPFLAGS += -I. -I$(ZSTD_DIR)
LDFLAGS  += $(ZSTD_LIB)

.PHONY: all clean test zstd

all: $(TARGET)

install : all
	install -m +x ./wordlist /usr/bin/

zstd: $(ZSTD_LIB)

$(ZSTD_LIB):
	$(MAKE) -C $(ZSTD_DIR) libzstd.a ZSTD_LIB_MINIFY=1 ZSTD_LEGACY_SUPPORT=0

$(TARGET): $(CLI_SRC) $(LIB_SRC) convert.hpp wordlist.hpp $(ZSTD_LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(CLI_SRC) $(LIB_SRC) $(LDFLAGS)

$(TEST): $(TEST_SRC) $(LIB_SRC) convert.hpp wordlist.hpp $(ZSTD_LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(TEST_SRC) $(LIB_SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TEST)

test: $(TARGET) $(TEST)
	./$(TEST) ./$(TARGET)
