# The boot rands: every `rand()` the original makes before its first frame

Measured 2026-09-28 from the live original under Wine, with the trace armed in the
hook's `DllMain` (`tools/oracle/hook/hook.c`, `WOS_RANDTRACE=1`) so nothing that happens
before the first pump step is missed. Reproduce with:

```sh
WOS_RANDTRACE=1 tools/oracle/run.sh tests/diff/boot_newsoul.dsc /tmp/orct
# -> randtrace.txt, one "call_index return_address [chain...]" line per rand()
```

`WOS_ENC_TRACE=1` additionally patches `FUN_0049B6C7` and writes one
`enc <n> this=<ptr> ret=<ptr> chain <...>` line per anti-cheat seal. Section 2 below is
that trace.

**A trap that cost a run:** the trace file must be named by an absolute Windows path
(`WOS_TRACE`). The game calls `SetCurrentDirectory(install root)` in `InitInstance`, and
a relative name resolves against the launcher's directory for the first ~1400 boot rands
and against the game directory for the rest — one run, two files. `run.sh` now exports
`WOS_TRACE` from `$PWD` by default.

---

## 1. The whole run is 1420 rands, in 13 sites

One run of `tests/diff/boot_newsoul.dsc` (1400 ms of script, 3 input events, 4 dumps).
All 1419 boot rands are consumed **before the first pump step**; the 1420th is the first
idle tick. The trace is byte-identical across runs.

| call range | count | return address | enclosing function | what it is |
|---|---|---|---|---|
| 1–1408 | 1408 | `0x0049B6F7` `0x0049B6FC` `0x0049B701` `0x0049B706` | `FUN_0049B6C7` (0x49B6C7) | the anti-cheat **EncInt seal**: 3 verification doubles, then 4 `rand()` into `k0..k3`. 352 calls x 4. |
| 1409 | 1 | `0x00426B27` | `FUN_004269AF` (0x4269AF, `CFrameWnd::CFrameWnd` via `CreateObject`) | the hero-table base ASLR draw |
| 1410 | 1 | `0x0048E1CF` | `FUN_0048E19A` | world-data block: picks a random start offset in the freshly `malloc`'d 0x3E0A0 block |
| 1411–1415 | 5 | `0x0042B4EA` `0x0042B4EE` `0x0042B4F5` `0x0042B4FC` `0x0042B50E` | `FUN_0042B4E0` | serial-number randomiser: `do { r=rand(); r=rand(); r=rand(); r=rand(); t=GetTickCount(); r=rand(); } while (mix & 0x3FFFFFFF)==0` — 5 rands + a tick read per iteration, 1 iteration observed |
| 1416–1419 | 4 | `0x00456BCC` | `FUN_00456B87` | chat/message queue push: appends to a 0x80-entry ring, stamps `GetTickCount()` at +0x20C, stores `(rand() % (param_3*2)) * 1000` ms at +0x60C — a **timed schedule in whole seconds** |
| 1420 | 1 | `0x00428996` | `FUN_0042895C` | the netgraph, reached only from `FUN_0040A7C7` (the 20 ms idle gate) |

### 0x00426B27 — the hero table is at a randomised address

The disassembly at 0x426B21 (`FUN_004269AF`, the `CMainFrame` constructor):

```
0x426b21  call *rand
0x426b27  cdq / xor eax,edx / push 0x15c0170 / sub eax,edx / and eax,0x3fff
0x426b36  xor eax,edx / sub eax,edx     ; EDI = (rand() % 0x4000) & ~15
0x426b47  mov [0x4e486c],eax            ; the 0x15C0170-byte hero block
0x426b81  add 0x4e486c,%edi             ; EDI = hero block + draw
0x426b8e  lea 0x1560a5c(%edi),%eax
0x426b94  mov [0x4e4870],edi            ; DAT_004E4870 = hero block + draw
0x426b9a  mov [0x0067fbf8],eax          ; DAT_0067FBF8 = hero block + draw + 0x1560A5C
```

So `DAT_0067FBF8`, which every module treats as "the hero table", is a pointer built at
runtime from a `rand()` draw. A hardcoded 0x0067FBF8 address in any reasoning about
"where the hero record lives" is a category error; the base moves by up to 0x4000 bytes
per session and the draw is rand #1409, i.e. after the 1408 EncInt draws and before the
serial randomiser. **Owner: FrontHero-2.**

**Correction (Core, 2026-09-28), and it matters.** An earlier version of this section said
`DAT_004E4870` held the *masked draw alone* and that a raw read of it was directly
comparable. **That was wrong, and the wrongness was mine, not the binary's.** The
`add 0x4e486c,%edi` at **0x426B81 executes before** the store at 0x426B94, so EDI already
carries the `malloc(0x15C0170)` block base by the time it is written. Verified by
re-disassembling `0x426B78..0x426B9B`:

```
0x426b81  03 3d 6c 48 4e 00   add    0x4e486c,%edi
0x426b8e  8d 87 5c 0a 56 01   lea    0x1560a5c(%edi),%eax
0x426b94  89 3d 70 48 4e 00   mov    %edi,0x4e4870
```

Consequences, all of which point the same way:

* An oracle read of `DAT_004E4870` **must be masked** — `& 0x3FF0` — before it is compared
  with a port-side `hero.base_offset`. Unmasked it is a live heap address and mismatches
  every run, and it mismatches as a large number against a small one, so it reads as an
  RNG divergence rather than as a units error.
* The same is true of everything derived from it. `DAT_004E4878` (the pet pen) is built
  at 0x426BEF as `DAT_004E4870 + 0xFEFCF0`, so **its base is not comparable between runs
  at all** — there is nothing to mask, because the base *is* the heap pointer. Only slot
  data indexed from it is comparable. Five sibling globals are derived the same way:
  `0x4E487C = +0xFFC294`, `0x4E4880 = +0xFA64F0`, `0x4E4884 = +0x98F4F0`,
  `0x4E4888 = +0xD5E4F0`, `0x4E4894 = +0xFFCA5C`.
* The same trap applies to `DAT_004E4874` in the section below: it is a heap base plus a
  draw too, not a draw.

### 0x0048E1CF — the world block

`FUN_0048E19A`, one draw, `DAT_004E4874 = DAT_00502834 + (rand() % 0x3E0A0 & ~15)`, then
`memset` of the whole 0x3E0A0 block and 0x4000 at 0x00D2C7D8. So the world/combatant
array is also placed at a random offset inside its allocation. **Owner: WorldData-2.**

### 0x00456BCC — the only bucket that can draw again during play

`FUN_00456B87` stamps a message into a 0x80-entry table with a random delay of
`0..(param_3*2-1)` **seconds** and a `GetTickCount()` at +0x20C. Four at boot. It is not
a boot constant: it is a schedule, and anything that pushes to that queue later draws
again. It is the one rand site in the whole binary that couples the LCG to the virtual
clock, which is exactly why both sides must model the clock before they can match
`rng.calls`. **Owner: Editors (chat).**

---

## 2. The 352 EncInt seals, mapped to addresses

`FUN_0049B6C7` is in `Souls.exe`'s own `.text`, so no IAT hook can reach it. The hook
patches it directly (`WOS_ENC_TRACE=1`): the entry is

```
0x49b6c7  sub  esp,4          \
0x49b6ca  push esi             >  six bytes, then
0x49b6cb  push edi            /
0x49b6cc  fild dword ptr [ecx]   <- TWO bytes
```

The `push imm32 ; ret` trampoline is six bytes, which would land **on** the `fild` — the
very instruction the trampoline jumps back to. The patch is therefore **seven** bytes and
the thunk replays the `fild` before jumping to 0x49B6CE. The thunk also has to save and
restore `ECX` around the C logging call, because the resumed `fild (%ecx)` dereferences
it and `enc_log` is ordinary C, so `ECX` is caller-saved.

Result: **352 calls, every one of them `ret=0x0049B765`**, i.e. every single boot seal is
`FUN_0049B75D` (the CLEAR constructor). No boot seal goes through `FUN_0049B71B` (SET),
`FUN_0049B734` (CLEAR proper) or `FUN_0049B73F` (ADD).

They split into exactly two tables, and the split closes the arithmetic that was open:

| group | seals | call order | `this` range | stride | owner |
|---|---|---|---|---|---|
| **A** | **100** | enc #1..#100 | `0x0052CA60`..`0x005339C0` | **0x120 (288)** | array base `0x0052C978`, EncInt at element **+0xE8** |
| **B** | **252** | enc #101..#352 | `0x005BB788`..`0x005C1460` | **0x38 (56)**, 7 per record | 36 records off `DAT_005BB764` |

### Group A: the `eh vector constructor iterator` array

`enc #1` is the outermost frame of all: its chain is `0x00401133 0x0040111F`, which is
`FUN_00401125` — the MSVC `eh vector constructor iterator` element thunk at 0x00401125,
reached through the iterator at 0x00401101 from `FUN_00401051`.

**The count is 100, not 99.** The `this` values run `0x52CA60` to `0x5339C0` in exact
0x120 steps: `(0x5339C0 - 0x52CA60)/0x120 + 1 = 100`. `FUN_00401137` memsets
`0x7080 = 100 x 288` at the same base, so the region is 100 elements and **all 100 are
sealed** — the earlier reading of 99 came from counting the memset region against a
99-element construction that does not exist. Element size 288, the EncInt at offset 232,
leaving 52 bytes of other fields below it.

### Group B, in context

`DAT_005BB764` is not a flat table: `FUN_0043BF47`, `FUN_0043BFB0` and `FUN_0043BFDE` all
walk it as `(&DAT_005bb764)[i * 0x5E6]` and stop at `0x005C15C4`, i.e. **four slots of
0x1798 (6016) bytes each**. All 252 boot seals are inside **slot 0**
(`0x5BB764`..`0x5D0EFC`).

Within slot 0 the seven EncInts of a group are consecutive, ascending, stride 0x38, and
the gap from the 7th of one group to the 1st of the next is 0x148 — so a group sits at
the **top** of a sub-record whose size is `0x148 + 6*0x38 = 664`, and the three groups
with a 0x188 gap sit in 728-byte sub-records. Group starts run from
`DAT_005BB764 + 0x24` to `DAT_005BB764 + 0x5BAC` in 36 steps: 32 of 664 and 3 of 728,
and `0x5BAC - 0x24 = 0x5B88 = 32*664 + 3*728` exactly. That is why the "one 288-byte
table of 352" reading could not be made to close: it is not one table.

In the port: `enc_construct_array(&table, 0xE8, 0x120, 100, ENC_ASCENDING)` for group A,
ascending — the vector-ctor iterator walks ascending, confirmed by the live addresses.


36 records starting at `DAT_005BB764 + 0x24` and ending at `DAT_005BB764 + 0x5BAC`.

The one thing this does **not** settle: which function drives the group-B loop. The
`FUN_0049B75D` -> `FUN_0049B734` leaf keeps no frame pointer, and the four-deep stack
scan bottoms out in a neighbouring static initialiser (`0x00427EB9`, an `_GLOBAL__sub_I`
entry that tail-calls `_initterm` at 0x404169) rather than in the real owner. What is
established is the shape, the addresses and the order, which is what the port needs.

### Corrections to figures circulated earlier in the tree

* The three EncInt constants are **`2.1459` / `1.4142` / `0.0123`** (`_DAT_004D0D38`,
  `_DAT_004D0D40`, `_DAT_004D0D48`), verified against `objdump -s -j .rdata`. Values
  quoted as 2.018237 / 1.926775 / 1.071800 are wrong.
* `sizeof(EncInt)` is 56, confirmed here independently: the group-B stride between
  consecutive sealed objects is 0x38.
* The boot total is **1420** rands for a 1400 ms script that reaches the world list, of
  which 1408 are the two tables, 6 are singletons, 4 are the chat queue and 1 is the
  first idle tick. `srand` is called twice before any of it.
