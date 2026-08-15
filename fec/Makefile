CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c89
CPPFLAGS ?= -Isrc
SRC = src/arena.c src/diag.c src/lexer.c src/ast.c src/parser.c src/types.c src/check.c src/emit_c.c src/driver.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean test dos-build
all: fec

fec: $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

test: fec
	@./tests/run-tests.sh

dos-build:
	@echo "Run build-dos.bat inside FreeDOS/Open Watcom."

clean:
	$(RM) $(OBJ) fec
