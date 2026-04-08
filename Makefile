CXX      = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -pthread -DCPPHTTPLIB_OPENSSL_SUPPORT
LDFLAGS  = -lsqlite3 -lssl -lcrypto -lpthread

SRC_DIR  = src
BUILD_DIR = build
TARGET   = bourse

SOURCES  = $(SRC_DIR)/main.cpp \
           $(SRC_DIR)/db.cpp \
           $(SRC_DIR)/market.cpp \
           $(SRC_DIR)/auth.cpp \
           $(SRC_DIR)/routes_game.cpp \
           $(SRC_DIR)/routes_admin.cpp

OBJECTS  = $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

.PHONY: all clean

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)