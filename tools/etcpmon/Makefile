# ETCP Monitor Makefile
# Usage: make [target]
#
# Targets:
#   all      - Build the application (default)
#   clean    - Remove object files and executable
#   rebuild  - Clean and rebuild

CC = gcc
CFLAGS = -Wall -O2 -DWIN32_LEAN_AND_MEAN
CFLAGS += -I. -I../../src
LDFLAGS = -mwindows
LIBS = -lws2_32 -lcomctl32 -luser32 -lgdi32

TARGET = etcpmon.exe
SRCS = etcpmon_main.c etcpmon_gui.c etcpmon_client.c etcpmon_graph.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean rebuild check

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo Linking $@
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJS): $(wildcard *.h)

%.o: %.c
	@echo Compiling $<
	$(CC) $(CFLAGS) -c -o $@ $<

test_etcpmon.exe: test_etcpmon.c $(SRCS) $(wildcard *.h)
	$(CC) $(CFLAGS) -o $@ test_etcpmon.c etcpmon_graph.c $(LIBS)

check: test_etcpmon.exe
	$(TEST_RUNNER) ./test_etcpmon.exe

clean:
	@echo Cleaning...
	rm -f $(OBJS) $(TARGET) test_etcpmon.exe

rebuild: clean $(TARGET)
