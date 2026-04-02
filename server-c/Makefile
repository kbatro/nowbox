CC ?= cc
CFLAGS = -Wall -Wextra -O2
LDFLAGS =
NAME = nowbox-server

UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
  LDFLAGS += -lutil
  CFLAGS += -I/opt/homebrew/include
  LDFLAGS += -L/opt/homebrew/lib
endif
ifeq ($(UNAME), Linux)
  LDFLAGS += -lutil
endif

LDFLAGS += -lsecp256k1 -lssl -lcrypto -ldatachannel
CFLAGS += -DMG_TLS=MG_TLS_OPENSSL -DMG_ENABLE_LOG=0

SRCS = main.c mongoose.c nostr.c webrtc.c

all: static_files.h $(NAME)

static_files.h: static/index.html static/style.css static/app.js
	bash embed.sh

$(NAME): $(SRCS) mongoose.h nostr.h static_files.h
	$(CC) $(CFLAGS) -o $(NAME) $(SRCS) $(LDFLAGS)
	@echo "built: ./$(NAME) ($$(du -h $(NAME) | cut -f1))"

clean:
	rm -f $(NAME) static_files.h

.PHONY: all clean
