CXX ?= g++
CPPFLAGS += $(shell pkg-config --cflags jsoncpp) $(foreach module,storage algebra query optimizer,-Ilib/$(module)/include)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs jsoncpp)

LIB_SOURCES := $(wildcard lib/*/src/*.cc lib/algebra/src/operators/*.cc)
LIB_OBJECTS := $(patsubst %.cc,build/%.o,$(LIB_SOURCES))
COMMON := build/tools/common.o
TOOLS := storage exec sql opt

.PHONY: all clean test $(TOOLS)
all: $(TOOLS) build/vql
build/vql: build/src/main.o build/src/shell.o $(LIB_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@
$(TOOLS): %: build/%
build/storage: build/tools/storage.o $(COMMON) $(LIB_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@
build/exec: build/tools/exec.o $(COMMON) $(LIB_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@
build/sql: build/tools/sql.o $(COMMON) $(LIB_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@
build/opt: build/tools/opt.o $(COMMON) $(LIB_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@
build/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
test: all
	python3 tests/cli_test.py
	python3 tests/shell_test.py $(CURDIR)/build/vql
clean:
	rm -rf build
-include build/src/main.d build/src/shell.d $(LIB_OBJECTS:.o=.d) $(COMMON:.o=.d) $(foreach tool,$(TOOLS),build/tools/$(tool).d)
