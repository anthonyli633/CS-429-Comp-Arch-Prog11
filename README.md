# Programming Assignment 11: Cache

Name: Anthony Li

EID: acl3856

## Build and Run

From the project root:

```sh
./build.sh
./build/hw11
```

The executable runs the tests in `main.c`. The cache implementation is in
`libtcache/tcache.c`, using the interfaces from `libtcache/tcache.h`.

## Testing Methodology and Results

The tests in `main.c` include targeted unit tests for cold misses and hit
accounting, write hits, L1 data LRU replacement, dirty L1 write-back into L2,
L2 replacement ordering after an L1 write-back, and split L1 instruction/data
coherency. The coherency test writes through the data cache, confirms the stale
instruction line is invalidated, then reads the same address through the
instruction cache and verifies it receives the updated value after the dirty data
line is written back to L2.

To compare replacement policies, the test harness runs the same conflict-heavy
trace once with `LRU` and once with `RANDOM`. The trace cycles over five cache
lines that map to the same L2 set, which has four ways. On this trace, LRU
thrashes because each newly requested line is the one it evicted on the previous
cycle, producing 20 L2 misses. The deterministic random replacement policy
keeps some useful lines by chance and produced 11 L2 misses in the local run.
