# qemu-guest-agent for IRIX 6.5. Build inside the guest with smake: stock make has no #if.
#
#   smake            /tmp/qemu-guest-agent.obj/qemu-guest-agent
#   smake test       the self-test and the stdin/stdout framing test
#   smake install    as root: the binary, an rc script and a chkconfig flag
#
# /share may be NFS and noexec, so objects go to local disk. -mips3 n32 runs on every machine that
# runs 6.5. Source is C89 plus `long long`; keep it so.

#if exists(/usr/local/mipspro/bin/cc)
CC = /usr/local/mipspro/bin/cc
#else
CC = cc
#endif

OBJDIR = /tmp/qemu-guest-agent.obj
BIN    = $(OBJDIR)/qemu-guest-agent
CFLAGS = -n32 -mips3 -O2 -fullwarn -woff 1209
HDRS   = install.h selftest.h core/util.h core/json.h core/base64.h core/dispatch.h transport/tty.h transport/serve.h transport/logstream.h
OBJS   = $(OBJDIR)/main.o $(OBJDIR)/selftest.o $(OBJDIR)/install.o $(OBJDIR)/util.o $(OBJDIR)/json.o $(OBJDIR)/base64.o $(OBJDIR)/dispatch.o $(OBJDIR)/protocol.o $(OBJDIR)/file.o $(OBJDIR)/exec.o $(OBJDIR)/system.o $(OBJDIR)/hw.o $(OBJDIR)/freeze.o $(OBJDIR)/ssh.o $(OBJDIR)/user.o $(OBJDIR)/stat.o $(OBJDIR)/tty.o $(OBJDIR)/serve.o $(OBJDIR)/logstream.o

all: $(BIN)

$(BIN): $(OBJDIR)/.dir $(OBJS)
	$(CC) $(CFLAGS) -o $(BIN) $(OBJS)

$(OBJDIR)/.dir:
	mkdir -p $(OBJDIR)
	touch $(OBJDIR)/.dir

$(OBJS): $(OBJDIR)/.dir $(HDRS)

$(OBJDIR)/main.o: main.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/main.o main.c

$(OBJDIR)/selftest.o: selftest.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/selftest.o selftest.c

$(OBJDIR)/install.o: install.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/install.o install.c

$(OBJDIR)/util.o: core/util.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/util.o core/util.c

$(OBJDIR)/json.o: core/json.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/json.o core/json.c

$(OBJDIR)/base64.o: core/base64.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/base64.o core/base64.c

$(OBJDIR)/dispatch.o: core/dispatch.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/dispatch.o core/dispatch.c

$(OBJDIR)/protocol.o: commands/protocol.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/protocol.o commands/protocol.c

$(OBJDIR)/file.o: commands/file.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/file.o commands/file.c

$(OBJDIR)/exec.o: commands/exec.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/exec.o commands/exec.c

$(OBJDIR)/system.o: commands/system.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/system.o commands/system.c

$(OBJDIR)/hw.o: commands/hw.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/hw.o commands/hw.c

$(OBJDIR)/freeze.o: commands/freeze.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/freeze.o commands/freeze.c

$(OBJDIR)/ssh.o: commands/ssh.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/ssh.o commands/ssh.c

$(OBJDIR)/user.o: commands/user.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/user.o commands/user.c

$(OBJDIR)/stat.o: commands/stat.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/stat.o commands/stat.c

$(OBJDIR)/tty.o: transport/tty.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/tty.o transport/tty.c

$(OBJDIR)/serve.o: transport/serve.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/serve.o transport/serve.c

$(OBJDIR)/logstream.o: transport/logstream.c
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/logstream.o transport/logstream.c

test: $(BIN)
	$(BIN) --self-test
	sh tests/stdio.sh $(BIN)

install: $(BIN)
	$(BIN) --install

clean:
	rm -rf $(OBJDIR)

.PHONY: all test install clean
