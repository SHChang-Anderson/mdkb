# mdkb Makefile
# A vim-like TUI for managing and reading Markdown notes

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -I./include
UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
    # Prefer Homebrew ncursesw (proper UTF-8 wide char support)
    BREW_NCURSES := $(shell brew --prefix ncurses 2>/dev/null)
    ifneq ($(BREW_NCURSES),)
        CFLAGS  += -I$(BREW_NCURSES)/include
        LDFLAGS  = -L$(BREW_NCURSES)/lib -lncursesw -lm
    else
        LDFLAGS  = -lncurses -lm
    endif
    CC = clang
else
    LDFLAGS = -lncursesw -lm
endif

# Source files (exclude daemon — removed)
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:src/%.c=obj/%.o)

TARGET = mdkb

all: $(TARGET)

obj:
	@mkdir -p obj

obj/%.o: src/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

debug: CFLAGS = -Wall -Wextra -std=c99 -g -O0 -DDEBUG -I./include
debug: clean $(TARGET)

release: CFLAGS = -Wall -Wextra -std=c99 -O3 -I./include
release: clean all

test: debug
	@echo "Running tests..."
	@./tests/run_tests.sh

clean:
	@rm -rf obj $(TARGET)
	@echo "Cleaned build artifacts"

man: man/man1/mdkb.1
	@gzip -k -f man/man1/mdkb.1
	@echo "Built man/man1/mdkb.1.gz"

format:
	@find src include -name '*.c' -o -name '*.h' | xargs clang-format -i

help:
	@echo "mdkb build targets:"
	@echo "  make         - Build development version"
	@echo "  make debug   - Build with debug symbols"
	@echo "  make release - Build optimized binary"
	@echo "  make clean   - Remove build artifacts"
	@echo "  make man     - Build compressed man page"

.PHONY: all debug release test clean format man help
