CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
LDFLAGS ?= -framework IOKit -framework CoreFoundation

SRC := $(wildcard src/*.cpp)
HDR := $(wildcard src/*.hpp)

fan: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@

test: tests/test_curve.cpp src/curve.cpp $(HDR)
	$(CXX) $(CXXFLAGS) tests/test_curve.cpp src/curve.cpp -o /tmp/fan-test-curve
	/tmp/fan-test-curve

clean:
	rm -f fan /tmp/fan-test-curve

.PHONY: test clean
