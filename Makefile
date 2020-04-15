CC=cc -c
CFLAGS=-W -Wall -Wextra -std=gnu11 -pedantic
LD=cc
LDFLAGS=-lpthread

BINDIR=bin

SERVER_SOURCES=src/server.c src/libtun/libtun.c src/tunnel.c
SERVER_OBJECTS=$(SERVER_SOURCES:%.c=%.o)
SERVER_EXEC=$(BINDIR)/server

CLIENT_SOURCES=src/client.c src/libtun/libtun.c src/tunnel.c
CLIENT_OBJECTS=$(CLIENT_SOURCES:%.c=%.o)
CLIENT_EXEC=$(BINDIR)/client

EXEC=$(CLIENT_EXEC) $(SERVER_EXEC)

ifeq ($(MODE),)
    MODE = release
endif

ifeq ($(MODE), debug)
	CFLAGS += -DDEBUG -O0 -g
	LDFLAGS += -g
else
	CFLAGS += -O3 -march=native
	LDFLAGS += -s
endif

CFLAGS += -I`pwd`/src

DUMMY := $(shell echo $(SERVER_OBJECTS) $(CLIENT_OBJECTS))

all: client server

$(BINDIR):
	mkdir $(BINDIR)

client: $(CLIENT_EXEC)
server: $(SERVER_EXEC)

$(CLIENT_EXEC): $(CLIENT_OBJECTS) bin
	$(LD) $(CLIENT_OBJECTS) -o $@ $(LDFLAGS)

$(SERVER_EXEC): $(SERVER_OBJECTS) bin
	$(LD) $(SERVER_OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(CLIENT_OBJECTS) $(SERVER_OBJECTS) $(BINDIR)

.PHONY: clean server client all
