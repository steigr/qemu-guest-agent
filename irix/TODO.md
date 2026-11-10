# TODO

- Find out why iris's TCP ports stay taken after it exits. They were still in `netstat` up to 72 s
  later, and an emulator started in that time carries on with no telnet, monitor or TCP console. The
  cause is not known; `tests/iris/boot-ip28.sh` waits for them.
