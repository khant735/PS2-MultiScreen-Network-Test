# GT3-revisited networking reference for PS2 MultiScreen

Reviewed: 22 October 2026

Upstream: https://github.com/matt-pulsipher/GT3-revisited (MIT; modern C++ reimplementation, **not original PS2 code**).

## Confirmed useful upstream references

- [Online architecture](https://github.com/matt-pulsipher/GT3-revisited/blob/master/docs/dev/ONLINE.md): up to six nodes, host and clients, roster, start barrier, clock sync, keepalives and timeouts.
- [UDP socket abstraction](https://github.com/matt-pulsipher/GT3-revisited/blob/master/src/net/socket.cpp): nonblocking UDP; desktop BSD/Winsock code requires replacement with PS2SDK networking.
- [Online race glue](https://github.com/matt-pulsipher/GT3-revisited/blob/master/src/app/screen_race_online.cpp): host-coordinated loaded/go state and clock-driven ticks.
- [Race state](https://github.com/matt-pulsipher/GT3-revisited/blob/master/src/race/net_race.cpp): example of timestamped remote-state handling.

The upstream README and ONLINE.md explicitly state that i.LINK Battle is *reimplemented using UDP* and **does not communicate with original PS2 consoles**. It is not evidence of original GT3 FireWire framing, original screen numbering, or an authentic GT3 multi-monitor protocol. Its 60 Hz car snapshots are not appropriate as a default throughput-test packet rate.

## Proposed integration into our PS2 ELF (not implemented yet)

1. Keep existing DEV9 Ethernet and any separately supported i.LINK transports; never replace them with desktop Winsock.
2. Define a small, versioned UDP control packet with magic, protocol version, message type, session ID, sender ID, sequence, and monotonic timestamp. Validate all lengths and versions.
3. Discovery: broadcast HELLO on the selected local interface, accept a host OFFER, and JOIN with a session nonce. Use a configurable timeout and fall back to the one-screen view when no peers remain.
4. Assign unique screen IDs according to our existing GT-style screen-position rule. **Do not** copy upstream player IDs as screen IDs.
5. Heartbeats and a state machine (searching, negotiating, connected, degraded, disconnected) should expose link age, last RX, RTT, packet loss, and negotiated transport/speed.
6. Add four-timestamp ping/pong clock-offset estimates, reject outliers, and make start/display coordination conditional on readiness. Use a monotonic timer, not wall-clock adjustments.
7. Separate low-rate control traffic from configurable throughput-test streams. Report measured payload bytes/sec, total bytes, sequence gaps, and elapsed time; never label estimated values as measured.
8. Make all timeouts, packet sizes, and retry intervals configurable; validate first on two PCSX2 instances and then real PS2 hardware.

## Validation gates

- One console: stable screen 1 after timeout.
- Two consoles: deterministic host election, unique IDs, connect/disconnect recovery.
- Six consoles: correct screen order, no duplicates, consistent session ID.
- Delayed/lost/reordered packets: no hangs or stale-connected state.
- Throughput counters: independent send/receive totals and verified timer calculations.
- i.LINK: independently verify hardware transport and protocol; UDP reference alone cannot establish support.

## Status

Reference audit documented only. No PS2 ELF source was available on the repository default branch at review time. No source integration, build, emulator test, or hardware test has been completed.
