# FactorIA - Linux build

CXX ?= g++
PKG_CONFIG ?= pkg-config
BUILD ?= debug
VCPKG_TRIPLET ?= x64-linux
VCPKG_ROOT := $(CURDIR)/vcpkg
VCPKG_INSTALLED_DIR ?= $(CURDIR)/vcpkg_installed
VCPKG := $(VCPKG_ROOT)/vcpkg
VCPKG_REPOSITORY := https://github.com/microsoft/vcpkg.git

APP_DIR := src/FactorIA.App
SRC_DIR := $(APP_DIR)/src
INCLUDE_DIR := $(APP_DIR)/include
BUILD_DIR := build/$(BUILD)
BIN_DIR := bin
TARGET := $(BIN_DIR)/factoria-$(BUILD)

# Keep this source list in sync with FactorIA.App.vcxproj.
SOURCES := \
    $(SRC_DIR)/AgentController.cpp \
    $(SRC_DIR)/AgentHistoryDatabase.cpp \
    $(SRC_DIR)/AppSettings.cpp \
    $(SRC_DIR)/FactorioTools.cpp \
    $(SRC_DIR)/LlamaClient.cpp \
    $(SRC_DIR)/Main.cpp \
    $(SRC_DIR)/MainFrame.cpp \
    $(SRC_DIR)/RconClient.cpp \
    $(SRC_DIR)/WebControlServer.cpp

OBJECTS := $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJECTS:.o=.d)
VCPKG_PREFIX := $(VCPKG_INSTALLED_DIR)/$(VCPKG_TRIPLET)

WARNINGS := -Wall -Wextra -Wformat=2 -Wunused-function
CPPFLAGS += -I$(INCLUDE_DIR) -I$(VCPKG_PREFIX)/include \
    -DASIO_STANDALONE -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -DCPPHTTPLIB_WEBSOCKET_READ_TIMEOUT_SECOND=5
CXXFLAGS += -std=c++20 $(WARNINGS) -MMD -MP -pthread

ifeq ($(BUILD),release)
CPPFLAGS += -DNDEBUG
CXXFLAGS += -O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
    -fstack-clash-protection -fcf-protection=full -fPIE
VCPKG_LIB_DIR := $(VCPKG_PREFIX)/lib
WX_CONFIG := $(VCPKG_PREFIX)/tools/wxwidgets/wx-config
LDFLAGS += -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
else ifeq ($(BUILD),debug)
CPPFLAGS += -D_DEBUG
CXXFLAGS += -O0 -g
VCPKG_LIB_DIR := $(VCPKG_PREFIX)/debug/lib
WX_CONFIG := $(VCPKG_PREFIX)/tools/wxwidgets/debug/wx-config
else
$(error Unsupported BUILD '$(BUILD)'; expected debug or release)
endif

LDFLAGS += -L$(VCPKG_LIB_DIR) -Wl,-rpath,$(VCPKG_LIB_DIR)
LDLIBS += -pthread
VCPKG_PKG_CONFIG_PATH := $(VCPKG_LIB_DIR)/pkgconfig:$(VCPKG_PREFIX)/share/pkgconfig

# Expand these in the recipe after the order-only deps prerequisite has run.
WX_CXXFLAGS = $$("$(WX_CONFIG)" --cxxflags)
WX_LIBS = $$("$(WX_CONFIG)" --libs)
VCPKG_LIBS = $$(PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$(VCPKG_PKG_CONFIG_PATH)" \
    "$(PKG_CONFIG)" --static --libs openssl sqlite3)

ifeq ($(STRICT),1)
CXXFLAGS += -Werror
endif

.PHONY: all analyze clean debug deps release

all: debug

debug:
	$(MAKE) BUILD=debug $(BIN_DIR)/factoria-debug

release:
	$(MAKE) BUILD=release $(BIN_DIR)/factoria-release

deps: $(VCPKG)
	$(VCPKG) install --triplet="$(VCPKG_TRIPLET)" --x-install-root="$(VCPKG_INSTALLED_DIR)"

# Bootstrap a private vcpkg checkout the first time dependencies are requested.
$(VCPKG):
	@if [ ! -d "$(VCPKG_ROOT)/.git" ]; then \
		git clone --depth 1 "$(VCPKG_REPOSITORY)" "$(VCPKG_ROOT)"; \
	fi
	"$(VCPKG_ROOT)/bootstrap-vcpkg.sh" -disableMetrics

analyze:
	@echo "Running cppcheck for unused functions..."
	@cppcheck --enable=unusedFunction --quiet $(APP_DIR)/ 2>&1 || true

$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) $(OBJECTS) -o $@ $(WX_LIBS) $(VCPKG_LIBS) $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | deps $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(WX_CXXFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf build $(BIN_DIR)

-include $(DEPS)
