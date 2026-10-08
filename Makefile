CXX ?= c++
CPPFLAGS += -Iinclude
CXXFLAGS ?= -O2 -DNDEBUG
# -ffp-contract=off: no fused multiply-add, so iteration and node counts are
# the same on x86 and ARM.
CXXFLAGS += -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic -Wconversion -Wshadow
BUILD ?= build
HEADERS := $(wildcard include/zerosum/*.hpp)

.PHONY: all test unit cli sanitize report bench clean

all: $(BUILD)/zerosum $(BUILD)/core-tests

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/zerosum: src/main.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD)/core-tests: tests/test_core.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Game rules, best responses, all five solvers, and the exploitation analysis.
unit: all
	$(BUILD)/core-tests

# The command line, plus the linear-program cross-check (needs NumPy and SciPy;
# skipped with a message without them).
cli: all
	python3 tests/test_cli.py $(BUILD)

test: unit cli

sanitize:
	$(MAKE) BUILD=build-sanitize CXXFLAGS='-std=c++20 -ffp-contract=off -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' unit

# Rerun every experiment and rebuild results/, the figures, and the memo.
report: all
	python3 scripts/build_report.py $(BUILD)

bench: all
	$(BUILD)/zerosum bench leduc --target 1.0 --max-nodes 4e9

clean:
	rm -rf build build-sanitize build-cmake
