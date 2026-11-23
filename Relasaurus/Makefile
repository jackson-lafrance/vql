CXX := c++

CPPFLAGS := -Iinclude
OPTFLAGS ?= -O3 -DNDEBUG
CXXFLAGS := -std=c++20 $(OPTFLAGS) -Wall -Wextra -Wpedantic -MMD -MP

SOURCES := src/algebra.cc src/relation.cc src/schema.cc src/lexer.cc src/parser.cc src/interpreter.cc src/repl.cc
OBJECTS := $(SOURCES:src/%.cc=build/%.o)

TEST_SOURCE := test/required_tests.cc
TEST_OBJECT := build/test/required_tests.o
TEST_TARGET := build/required_tests
APP_TARGET := build/relasaurus

.PHONY: all test run clean

all: $(APP_TARGET) $(TEST_TARGET)

$(APP_TARGET): main.cc $(OBJECTS)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cc $(OBJECTS) \
		$(LDFLAGS) $(LDLIBS) -o $@

$(TEST_TARGET): build/interpreter.o build/parser.o build/lexer.o \
	build/algebra.o build/relation.o build/schema.o build/repl.o $(TEST_OBJECT)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cc
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/test/%.o: test/%.cc
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	@./$(TEST_TARGET)

run: $(APP_TARGET)
	./$(APP_TARGET)

clean:
	rm -rf build

-include $(OBJECTS:.o=.d) $(TEST_OBJECT:.o=.d)
