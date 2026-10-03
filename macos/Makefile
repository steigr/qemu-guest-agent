# qemu-guest-agent: QEMU guest agent for PowerPC Mac OS X (10.0-10.5).
#
# Builds natively on the host for development (`make test`) and inside a guest
# with that release's own Developer Tools for deployment:
#
#   make ARCH_FLAGS="-arch ppc"       # on a PPC guest the default arch already is ppc
#
# Source is C89 + `long long` (GNU extension, accepted by GCC 2.95); keep it so.

CC        ?= cc
ARCH_FLAGS ?=
OPT       ?= -Os
# Flags are probed because GCC 2.95 (10.0/10.1) and 3.1 (10.2) reject newer ones
# (-Wextra, -Wdeclaration-after-statement); -W is the old spelling of -Wextra.
# (GCC 2.95 only warns about unknown options and exits 0, so judge by silence.)
cc_opt = $(shell echo 'int x;' > .probe.c; if [ -z "`$(CC) $(1) -c -o .probe.o .probe.c 2>&1`" ]; then echo $(1); fi; rm -f .probe.c .probe.o)
STD       := $(call cc_opt,-std=gnu89)
WARN      := -Wall -W $(call cc_opt,-Wdeclaration-after-statement)
CFLAGS    += $(STD) $(WARN) $(OPT) $(ARCH_FLAGS)
LDFLAGS   += $(ARCH_FLAGS) -framework IOKit -framework CoreFoundation

SRCS = main.c selftest.c \
       core/util.c core/json.c core/base64.c core/dispatch.c \
       commands/protocol.c commands/file.c commands/exec.c commands/system.c commands/hw.c commands/freeze.c commands/ssh.c commands/user.c commands/stat.c \
       install.c transport/tty.c transport/serve.c transport/vsock.c transport/logstream.c
OBJS = $(SRCS:.c=.o)

all: qemu-guest-agent

qemu-guest-agent: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: qemu-guest-agent
	./qemu-guest-agent --self-test
	sh tests/stdio.sh ./qemu-guest-agent

clean:
	rm -f qemu-guest-agent $(OBJS)

.PHONY: all test clean
