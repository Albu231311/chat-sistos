

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pthread -Igen/ -Isrc/
LDFLAGS  := -lprotobuf -lpthread

PROTO_DIR := protos
GEN_DIR   := gen
SRC_DIR   := src

UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo "/usr/local")

    # pkg-config para protobuf (incluye absl automáticamente)
    PROTO_CFLAGS := $(shell pkg-config --cflags protobuf 2>/dev/null)
    PROTO_LIBS   := $(shell pkg-config --libs   protobuf 2>/dev/null)

    # pkg-config para ncurses de Homebrew
    export PKG_CONFIG_PATH := $(BREW_PREFIX)/opt/ncurses/lib/pkgconfig:$(PKG_CONFIG_PATH)
    NCURSES_CFLAGS := $(shell pkg-config --cflags ncurses 2>/dev/null)
    NCURSES_LIBS   := $(shell pkg-config --libs   ncurses 2>/dev/null)

    CXXFLAGS += $(PROTO_CFLAGS)
    LDFLAGS   = $(PROTO_LIBS) -lpthread
    NCURSES_FLAGS := $(NCURSES_CFLAGS) $(NCURSES_LIBS)

    PROTOC := $(BREW_PREFIX)/bin/protoc
else
    # Linux / WSL
    NCURSES_FLAGS := -lncurses
    PROTOC        := protoc
endif

# ── Proto sources ─────────────────────────────────────────────
PROTO_FILES := $(wildcard $(PROTO_DIR)/*.proto)
PROTO_CC    := $(patsubst $(PROTO_DIR)/%.proto,$(GEN_DIR)/%.pb.cc,$(PROTO_FILES))
PROTO_OBJS  := $(patsubst %.cc,%.o,$(PROTO_CC))

.PHONY: all clean

all: server client

$(GEN_DIR):
	mkdir -p $(GEN_DIR)

$(PROTO_CC) &: $(PROTO_FILES) | $(GEN_DIR)
	$(PROTOC) -I$(PROTO_DIR) --cpp_out=$(GEN_DIR) $(PROTO_FILES)

$(GEN_DIR)/%.pb.o: $(GEN_DIR)/%.pb.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SRC_DIR)/server.o: $(SRC_DIR)/server.cpp $(SRC_DIR)/utils.h $(PROTO_CC)
	$(CXX) $(CXXFLAGS) -c $< -o $@

server: $(SRC_DIR)/server.o $(PROTO_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "✓ server built"

$(SRC_DIR)/client.o: $(SRC_DIR)/client.cpp $(SRC_DIR)/utils.h $(PROTO_CC)
	$(CXX) $(CXXFLAGS) $(NCURSES_CFLAGS) -c $< -o $@

client: $(SRC_DIR)/client.o $(PROTO_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(NCURSES_LIBS)
	@echo "✓ client built"

clean:
	rm -rf $(GEN_DIR) $(SRC_DIR)/*.o server client
	@echo "✓ cleaned"
