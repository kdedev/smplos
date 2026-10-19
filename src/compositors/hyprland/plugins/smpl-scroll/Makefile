# smpl-scroll: a Hyprland plugin that lets a scrolling-layout column grow wider than
# the monitor. Build needs the Hyprland development headers (the `hyprland` package
# ships them) and a C++23 compiler.
#
#   make        build build/smpl-scroll.so against the installed Hyprland
#   make test   compositor-free unit tests (plain g++, no Hyprland headers)
#   make clean

CXX      ?= g++
BUILD    := build
PLUGIN   := $(BUILD)/smpl-scroll.so

ifeq ($(CXX),g++)
EXTRA_FLAGS := --no-gnu-unique
else
EXTRA_FLAGS :=
endif

# Bind every symbol when the plugin loads, so a function missing from this Hyprland
# fails the load instead of crashing the compositor on its first call.
EXTRA_FLAGS += -Wl,-z,now

LIB_SRC    := scroll_math.cpp
PLUGIN_SRC := $(LIB_SRC) main.cpp
TESTS      := test_scroll_math

CXXFLAGS ?= -O2
PLUGIN_CXXFLAGS := $(CXXFLAGS) -shared -fPIC -std=c++2b -Wall -Wextra
TEST_CXXFLAGS   := -O1 -g -std=c++20 -Wall -Wextra -Werror

HYPR_PKGS := pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon

.PHONY: all test clean
all: $(PLUGIN)

$(PLUGIN): $(PLUGIN_SRC) $(wildcard *.hpp)
	@mkdir -p $(BUILD)
	$(CXX) $(PLUGIN_CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(PLUGIN_SRC) -o $@ `pkg-config --cflags $(HYPR_PKGS)`

test: $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $(TESTS); do $(BUILD)/$$t; done

$(BUILD)/test_%: tests/test_%.cpp $(LIB_SRC) $(wildcard *.hpp) tests/check.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(TEST_CXXFLAGS) $< $(LIB_SRC) -o $@

clean:
	rm -rf $(BUILD)
