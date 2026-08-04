# `-O` — onboarding output

A host tool that provisions a device needs a handful of facts from it: is a
password set, is WiFi up, what is the IP, which networks are in range. Reading
those out of a status display means parsing human formatting that nobody thinks
of as an API — and that anyone improving the display is free to change.

`-O` is the alternative. A command that a flasher depends on takes `-O` and
prints exactly what onboarding needs, nothing else.

The flag *is* the contract. It says, on the device side, that this output is
depended upon and by whom, so it is greppable and cannot be tidied away by
accident.

## Format

`key=value`, one per line, the value running to end of line. No quoting, no
escaping — trivial to emit and to parse. The transport imposes no delimiter
constraints of its own, since [frames](framed-rpc.md) are length-prefixed.

Two rules for readers, and they are what let the device side evolve:

- **Unknown keys are ignored.** A command may grow keys without breaking
  anything that already reads it.
- **A missing key is unknown, not a default.** `admin` absent means "we don't
  know whether a password is set", never "no password".

Emitting a key with an empty value and omitting it entirely mean the same thing,
so a command is free to leave out what it has no answer for.

## The commands

    auth -O
      <realm>=set|unset|locked          one line per realm that exists

    net -O
      state=ap|sta|connecting|down
      ssid=…
      ip=…
      hostname=…

    net scan -O
      count=<n>
      ap=<rssi> <open|closed> <ssid>    × n

`auth -O` reports whichever realms exist and onboarding reads only `admin`, so
that set can grow freely. `net scan -O` emits `count` first — computed after
dropping any SSID that is not representable on one line — so a reader knows how
many records to expect and a short read is a truncated reply rather than a
device that lied. `ap=` puts the SSID last so spaces need no quoting.

`show` is not in this list and does not need to be: it is already
machine-shaped (`key = value`) and takes a prefix, so `show sys.build` and
`show sys.flash` each fetch a whole subtree in one round trip.

## Cost

A second output path per command, which no human ever looks at and can rot
silently. The mitigations are to keep `-O` output minimal, and that onboarding
working at all is the test.

## See also

- [framed-rpc.md](framed-rpc.md) — the transport these answers come back over.
- [auth.md](auth.md), [net.md](../../spangap-net/docs/net.md) — the commands
  themselves.
