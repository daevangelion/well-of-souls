# rand/srand and time sources census (Souls.exe)

## Summary

Census complete: 180 `rand()` + 4 `srand()` (182 sites, matching the plan's count), ~200 `GetTickCount`, 6 `SetTimer`/6 `KillTimer`, 2 `QueryPerformanceCounter`, 0 `timeGetTime`, 20 `time()`. Every rand site is given with its all.c line, enclosing FUN_ VA, purpose and subsystem, resolved by taking the last top-level function definition preceding each site (exact method: an empty window proves the enclosing definition is further back, so any definition found in the window is by construction the nearest preceding one). The two most important findings beyond the enumeration: (1) `FUN_00443929` (0x443929) uses a save/restore seed idiom — `local_10 = rand(); srand(param_1); ... srand(local_10);` — so the port must emit crt_rand BEFORE crt_srand, then restore; (2) `FUN_0046260E` (0x46260E, the per-tick encounter roll) burns 2-4 rands per tick behind a `GetTickCount` 5-second cooldown, and `FUN_00448E810` (0x48E810, monster spawn) burns 2..771 rands in one call, so both are single-point-of-divergence for any replay. Also newly found and not in docs/re: `FUN_0044A63A` (0x44A63A) is a 1000 ms "Status::OnTimer freshening monsters" tick that is solo-reachable and rule-affecting.

## Architecture

Read-only source analysis; no repo files were modified. Method: every `rand()`/`srand()`/`GetTickCount`/`time()`/`SetTimer`/`KillTimer`/`QueryPerformanceCounter` occurrence in work/decomp/all.c (138000 lines) was located with regex, then the enclosing `FUN_` VA for each rand site was resolved by taking the last top-level function-definition line (`^[A-Za-z_].*[FL][UA][NB]_00xxxxxx(`) preceding the site. The method is exact: an empty window proves the enclosing definition lies further back, and any definition found in the window is by construction the nearest preceding one. `work/decomp/functions.tsv` (VA, name, byte size) was used to cross-check region identity, and docs/re/{battle,maps,script,boot_flow,art,formats_online}.md plus docs/gap_analysis.md supplied the function names. Timers, srand and time() were resolved the same way.

# RNG and time-source census — `work/decomp/all.c` (WoS A96, `extracted/Souls.exe`)

Every row is cited by `all.c` line and enclosing function VA. **Counts: 180 `rand()` calls, 4 `srand()` calls
(180+4 = 182 sites, matching the plan), ~200 `GetTickCount()` calls, 6 `SetTimer`, 6 `KillTimer`,
2 `QueryPerformanceCounter`, 0 `timeGetTime`.** Confirms Core: the PE import table has no `timeGetTime`.

Notation: `FUN_00xxxxxx` is the enclosing function. "solo" = reachable in offline single-player;
"net" = SRNet / PK / ladder only. Names in parentheses come from `docs/re/*.md`; where a doc names nothing I
say `[region]` and mark the identification as inferred.

---

## 1. `srand` — the four seed sites

| all.c | VA | Enclosing | Seed source | Subsystem |
|---|---|---|---|---|
| 27953 | 0x4269AF | `FUN_004269af` (`CFrameWnd::__fastcall` ctor, app startup — the `InitInstance`-time global init) | `srand((uint)time(NULL))` | front/boot — **solo** |
| 27987 | 0x4269AF | same function, 34 lines later | `srand((uint)time(NULL))` | front/boot — **solo** |
| 48593 | 0x443929 | `FUN_00443929` (random map/world generator; reached from the "create your own world" flow) | `srand(param_1)` — an **explicit caller-supplied seed**, not a clock | editor/world-gen — **solo** |
| 48643 | 0x443929 | same function, exit path `LAB_00443b05` | `srand(local_10)` where `local_10` is the value drawn by the `rand()` at 48592 **before** the reseed | editor/world-gen — **solo** |

**Critical, and easy to get wrong — the save/restore idiom at 0x443929.** The prologue is
`FUN_0042b394(...); local_10 = rand(); srand(param_1);` and the epilogue is `srand(local_10); return 0;`.
So the original *draws one value from the live stream*, reseeds with the explicit seed, generates the world,
then reseeds with the drawn value to restore the stream position. The port must mirror this exactly as
`crt_rand(); crt_srand(seed); …; crt_srand(saved);` — three RNG operations, in that order. Reordering the
first `crt_rand()` after the `crt_srand()` shifts every subsequent call in the session.

**Also note (0x4269AF):** the app seeds twice with `time(NULL)` a few lines apart; the *second* `srand` wins.
Both must be executed or `rng.state` diverges at first load. Seeded from `clock_time_s()`, not `clock_ms()`.

---

## 2. `rand()` by subsystem

### 2.1 Front end / boot / MFC dialogs (0x401000–0x41bxxx) — solo

| all.c | VA | Use | Notes |
|---|---|---|---|
| 3057 | 0x4054E8 | `rand()%7+5` → `FUN_00429a31(…,…)` (sound/message id), then clears a flag bit | dialog callback |
| 3971, 3973 | 0x405EA9 | 30 % gate, then a 0–3 branch pick: 2 rands, **conditional count** | sits between `GetTickCount` calls at 3926/4182 → an OnIdle/heartbeat handler |
| 4129, 4132 | 0x4066A9 | picks a row of the `DAT_004dc8e0` table (`% DAT_00538368`) and a value in `0x20..0x3f` | UI list fill |
| 7889 | 0x40ADBB | Fisher–Yates shuffle of `DAT_00538670` (52 entries) — 1 rand per swap, count = 51 | **count depends on the table size constant only** |
| 17467 | 0x41862F | `rand() % LVar2` → `SendMessageA(hwnd, 0x186, …)` — random control/row selection | 0x186 = `LB_SETCURSEL`-ish; only when `0 < LVar2` |
| 22013 | 0x41D121 | `rand() % DAT_004df928` — random index into a **list built earlier in the same call**; the next statement tests `FUN_004194bb(DAT_004dd20c)` and loops | **loop ⇒ rand count is state-dependent** |
| 25262, 25263 | 0x42198F | Quadris splash: two rands per iteration drawing a random rect, loop bounded by **`GetTickCount` elapsed < param_2** | **wall-clock-bounded ⇒ nondeterministic call count; must be driven by the virtual clock, and the port should treat the loop bound as a clock comparison, not a fixed iteration count** |
| 27954 | 0x4269AF | `rand() & 31` feeding `FUN_00426149(0x15c0170)` (a boot key/nonce) | 1 rand immediately after the first `srand` |
| 48592 | 0x443929 | the "save current stream" draw described in §1 | ordering-critical |
| 30576 | 0x42895C | `rand()` discarded, then `GetTickCount() - _DAT_004e48cc < 25` zeroes a flag | **[net]** SRNet performance graph (`FUN_0042895c` is the QPC netgraph). Net-only, but it **consumes a rand**; a port that omits it shifts the whole offline stream. |
| 32144–32149 | 0x42B4E0 | 6 rands + `GetTickCount` mixed and re-hashed in a `do…while` that repeats until the folded value is non-zero | **[net]** SRN stepper warm-up. Net-only, but the loop makes the count data-dependent. |
| 34171, 34172 | 0x42CB8F | 2 discarded rands before `FUN_004622f4()` | **[net]** big server opcode interpreter; only on the `DAT_0067fbf8+0xcc == pcVar6` branch. **2 rands, always paired — do not drop one.** |
| 34293, 34294 | 0x42CB8F | same idiom, second branch | as above |
| 26475 | 0x423C6D | `rand()` spliced into a `http://www.synthetic-reality.com/…` URL and **returned to the caller** | **[net]** |

### 2.2 Map subsystem — solo

| all.c | VA | Use | Notes |
|---|---|---|---|
| 70922 | 0x46260E | **encounter roll**: `rand()%10000 < (diff*3)/(bitlen+3)` | `docs/re/maps.md` §8a names this function. Guarded by `GetTickCount()-_DAT_004f2220 > 5000` |
| 70926 | 0x46260E | second roll in the same `if`: `rand()%10000 < DAT_0067fbf8[0x281]`, again 5 s-gated, followed by `GetTickCount` and `FUN_0046259a(0)` | **2–4 rands per tick depending on branch** — the highest-risk ordering site in the whole file |
| 72539, 72551, 72564 | 0x464DAF | 25 % / 15 % / 5 % spawn gates, each then calling `FUN_00480499` + `FUN_0047c1e4` | `.mon`-proximity encounter resolver (`docs/re/maps.md`). **1 rand per gate, mutually exclusive branches → count varies 0–1** |
| 62052 | 0x456AA1 | `(rand() % (n*2)) * 1000` added to a due-time, inside a `GetTickCount` 1 s-cadence block | minigame/Tactics slot scheduling; **time-gated** |
| 62118 | 0x456B87 | `(rand() % (param_3*2)) * 1000` — same idiom, second site | as above |

### 2.3 Battle (0x48xxxx–0x4Axxxx) — solo

| all.c | VA | Use | Notes |
|---|---|---|---|
| 32480 | 0x42B867 | trophy roll: `rand()%2000 < monster[0xb4]` plus a 5-second/11-tick freshness test | `docs/re/battle.md` §9 names `FUN_0042b867`; gap_analysis Med "no victory trophy roll" |
| 32689, 32691, 32692 | 0x42BB5C | payout/level-up: 3 rands (`rand()&0x400`, a length, a 0–1 flag) → `FUN_0042b867(rand-derived winner, …)` | `docs/re/battle.md` §9 names `FUN_0042bb5c`. **3 rands, always all three.** |
| 34614 | 0x42CB8F | `0x1f`-masked value + `FUN_0048b13c()` → `actor[0x9e]` | **[net]** server-driven battle spawn; solo uses 94093 instead |
| 45939, 45940 | 0x43FEFF | 1 discarded + `rand()&0xfff < 100` (≈9.8 %) gate on a `GetTickCount()+500` deadline | **scene/battle actor animation**; the gate is wall-clock, so the count is time-dependent |
| 739… no — see 102929 | | | |
| 93973 | 0x47FCED | `actor[0x55] = rand()` when it is still 0 | one-shot per actor; **must consume a rand in the same place** |
| 94093 | 0x480499 | `0x1f` mask + `FUN_0048b13c()` → `actor[0x9e]` (sprite slot jitter) | solo counterpart of 34614 |
| 104688 | 0x48E19A | `rand()%0x3E0A0 & 0xFFFFFFF0` → random offset into the **big map DIB** | random-map scroll origin; cosmetic but rule-visible via `DAT_004e4874` |
| 104740 | 0x48E19A | `rand() % DAT_00502990` then loops against `FUN_0049b70f()` | same function as 104688; **loop ⇒ variable count** |
| 104903 | 0x48E62C | picks a target from `local_268[rand() % local_8]` when the candidate list is non-empty | to-hit candidate selection |
| 105036 | 0x48E810 | one seed `rand()`, then a 0x300-slot ring walk using `(seed + k) % 0x300` — **1 rand feeds the whole loop** | monster spawner (`docs/re/battle.md`) |
| 105054 | 0x48E810 | inside the ring walk: `rand()%3000 < monster[0x430]` decides whether a free slot is taken | **up to 0x300 iterations ⇒ 0..768 rands for one spawn** — the single largest rand-count range in the file |
| 105077, 105079 | 0x48E810 | `rand()%500` then conditionally a second `rand()%1000`, both against `param_1+0x430` | **1 or 2 rands, data-dependent** |
| 105170 | 0x48EBE3 | `local_c24[rand() % local_c]` | group/element pick |
| 105398 | 0x48EF3F | `local_288[rand() % local_20]` | |
| 105515 | 0x48F364 | `local_1a0[rand() % local_c]` | |
| 105745 | 0x48F816 | `rand()%100 < iVar3` — the **to-hit chance** roll | `docs/re/battle.md` §8 |
| 105824, 105849 | 0x48F913 | 100 % AI-decision roll, then a 33 % "look for another actor" roll | actor picker (`docs/re/battle.md`) |
| 105857 | 0x48F913 | `rand()%1000 < 250` gating `FUN_0048ebe3` (pet/escort pull-in) | **three separate rands in one AI decision** |
| 105896, 105898 | 0x48F913 | `actor[0xa0] += rand()%10-5` and the same for `0xa1` | monster flee: `(rand()+4)*10` style jitter, **2 rands, always paired** |
| 105974 | 0x48FE21 | `sprintf("…%d…", rand()%param_4 + 1)` | combat-log line number |
| 106193 | 0x4904EB | `bVar1 = rand()%100 < iVar2` — the **hit/miss roll** | `docs/re/battle.md` to-hit `FUN_004904eb`. **If `*(int*)(iVar4+0x394) != 0` the result is forced false but the rand was still consumed.** |
| 106433 | 0x49099B | `rand()%100 < local_8` gate before `FUN_00480499` | group encounter resolver (`docs/re/maps.md`) |
| 106454 | 0x49099B | `piVar2[rand()%local_10 + 2]` — fallback monster pick after the `"PickRandomMonster didn't pick an…"` complaint | **only reached when the first pass found nothing** |
| 106952, 106958 | 0x4915ED | `rand()&1` twice, cross-assigning `bVar2`/`bVar3` | initial **turn order coin flip**; 2 rands, always both |
| 107376 | 0x491E45 | `actor[0x11f] = (GetTickCount() - rand()%5000) - 3000` | **animation-phase jitter seeded from the tick and a rand — the 2 sources are mixed, so the virtual clock value is part of the battle state.** |
| 107395, 107406 | 0x491E45 | `0x1f` (or `0x3f`, negated) mask + `FUN_0048b13c()` → `actor[0x9e]`, gated on `DAT_00502b14` and `param_1` | two mutually exclusive branches ⇒ 0 or 1 rand |
| 107836, 107838 | 0x492900 | 2 rands placing a sprite in a rect (`% local_c`, `% local_10`) | **paired** |
| 107855, 107857 | 0x492900 | 2 more rands for a second sprite position | **paired** |
| 108462 | 0x49331E | `rand()%100 < 10` deciding whether an actor reverts type 4↔1 | **paired with the `*piVar10 == 1 \|\| == 4` guard — count varies** |
| 109333 | 0x4946B2 | `field += rand()%(n+1) + n/2`, clamped to 0x7FFF | apply-damage (`docs/re/battle.md` `FUN_004946b2`) — the accumulation/bleed field |
| 109639 | 0x49590D | `(rand()&0xe0)>>5 < 3` — a 3-in-8 branch | |
| 120087, 120089 | 0x4A3A54 | `x += rand()%(2n)-n`, `y += rand()%(2n)-n` | target-cell scatter, **paired, guarded on `param_4 != 0`** |
| 121261, 121374, 121383 | 0x4A4D70 | damage/particle scatter: `y = base-24 + rand()%0x30`, then two `x = rand()%width + …` | 1 rand on the entry path, **2 on the two continuation branches** |
| 121868, 121873, 121877, 121880, 121884 | 0x4A6974 | level/scale jitter: `(*param_1)/5 + base`, a `param_5` divisibility test, then either `rand()%3+2` or a discarded `rand()` | **up to 4 rands for one value; the 4th is a bare `rand();` that must still be counted** |
| 122841 | 0x4A7794 | `rand()%100 < FUN_004a7456(monster+0x440)` — the **damage-variance** gate | damage roller (`docs/re/battle.md` `FUN_004a7794`) |
| 123001 | 0x4A7794 | `rand()%100 < iVar2` → returns 0x7FFB (a "no effect" sentinel) | same function; **2 rands per damage roll** |
| 123258 | 0x4A8AFB | `_DAT_004dd52c = rand(); FUN_0040ab35(param_1 ^ _DAT_004dd52c);` | secure-cookie key refresh. **The rand value is a persistent global that XORs every cookie write** — offline-relevant if cookie integrity is ever compared. |

### 2.4 Scene VM, dialogs, art and presentation — solo

| all.c | VA | Use | Notes |
|---|---|---|---|
| 12283, 12285, 12288 | 0x410D9A | **3 rands per iteration** of a scene-actor placement loop: `x`, `y`, then a 1–8 `InflateRect` amount | **3×N rands, N is the actor count** |
| 13514, 13516, 13518, 13520 | 0x412E2F | four `(rand()%1000000+1000000)/2` values into a creature/terrain table | 4 rands, fixed |
| 13525, 13527, 13528, 13529 | 0x412E2F | inside a 0x19 %-gated ring: **3 rands form one RGB triple** (`<<8 \| <<10 \| …`) | 0 or 3 rands |
| 13538, 13539, 13541 | 0x412E2F | `_Dst[0x8b] = -rand()`, then two `rand()%100` pairs for `0x31/0x32` and `0x33/0x34` | 3 rands |
| 13838, 13843 | 0x4135DA | 30 % → `4`, else 10 % → `2`, else 0 | **2 rands, always both consumed** (the first is unconditional) |
| 14346, 14354 | 0x414059 | `0x1f` mask ± `FUN_0048b13c()` → `actor[0x9e]`; the second is the `param_1 == 2` (mirrored) variant | mutually exclusive |
| 14449 | 0x4142F2 | `rand()%100 < 0x1e` (30 %) → a scene message | guarded by `(1000000 - field < 100000)` |
| 14730 | 0x414CC9 | picks a phrase from `PTR_DAT_004dea88` (`% DAT_004deab4`) when `param_4 == 0` | scene text variant |
| 49310 | 0x4446FA | `rand(); iVar1 = ftol();` then a clamp against `param_1` | random-index helper; **1 discarded rand** |
| 739… — see §2.5 | | | |
| 73753 | 0x4666B1 | `rand() % 0x12C0` (4800) per table row | **fixed-count loop over a fixed 0x12C0-entry table** |
| 76906 | 0x469FFC | `rand()%7+1` per entry up to `&DAT_00698FCC` | **fixed-count loop; 7 = one die** |
| 77319 | 0x46A758 | `rand()%7+1` once, after a loop over the same table | minigame dice |
| 79195 | 0x46C8E9 | `rand()%100 < *(int*)(local_8+0xc)` — **encounter/opportunity roll** with a per-object probability | minigame |
| 85505, 85507 | 0x47437E | 1 + 8 rands folded into a `float10` (`u = u<<4 ^ r>>4`, 8 iterations) | minigame value generator; **9 rands, fixed** |
| 85556, 85557 | 0x4743E8 | 2 rands choosing a cell in the 4-column playfield | **paired** |
| 80536, 80540, 80542, 80544, 80555 | 0x46E14C | particle init: `rand()%1000*k`, `rand()%200-100`, `rand()%(2n)`, a grey-level `rand()%100+0x80` packed into 0xRRGGBB, then `rand(); ftol()` in a `sin` loop | **5 rands + a loop**; purely visual but **still burns the stream** |
| 80594, 80596 | 0x46E29B | 100 iterations × 2 rands = **200 rands** placing 100 random points | fixed count, but a large one |
| 80639, 80641, 80643 | 0x46E3AA | 3 rands per iteration (a length `0x14..0x31`, an x, a y) passed to `FUN_0046e14c` | **3×N, N is a caller-supplied loop count** |
| 80844, 80850, 80856 | 0x46E4D1 | 80 % / 50 % / 30 % gate, each converting a stat through `ftol(…, 0x32/0x4B/100)` | **1 rand per gate, all three evaluated sequentially — up to 3 rands** |
| 82064 | 0x46FBF1 | `rand()%100 <= piVar3[0x155]` adding an entry to a result list | minigame scoring |
| 92620, 92621 | 0x47D49C | 2 rands per iteration: pick a source then swap | **2×N shuffle loop** |
| 97125 | 0x4847CD | `rand()%param_2 + 1` → `sprintf` — a **random number 1..N emitted into scene text** (`%` substitution) | solo |
| 97698 | 0x4851F1 | `rand()%100 < local_8` — a generic scene probability gate | |
| 98151, 98172 | 0x485F21 | `rand()%5 == 3` and `rand()%0xf` driving the **random hero-attribute/name generator** (`GetTickCount` at 98175) | front/scene hybrid |
| 98195, 98200, 98204, 98210, 98218 | 0x485FF5 | **the random-insult generator** (`PTR_s_artless_004fb050` %49, `%s_base_court` %49, `%s_apple_john` %50), with `DAT_004f7590` as a separator and `param_2` selecting 1–3 clauses | This is the **`%3` insult substitution**. Count is 1, 2 or 3 depending on `param_2` and on the `(rand&0xc00)` gates — see §4. |
| 98232 | 0x4860C3 | `return rand() & 0x7FFF` — the general **unsigned random helper** | every caller of this must map 1:1 |
| 99285, 99308 | 0x4868C0 | JPEG-decode table lookups driven by `rand()` byte patterns | art; consumes rands, no rule effect |
| 99585 | 0x4874A0 | `rand()%param_4` inside a clamp loop | |
| 99778, 99780 | 0x487D76 | 2 rands per iteration for a jittered `BitBlt` | **2×N** |
| 102929 | 0x48B1AD | `rand()%100 > 49` → a 49/51 coin | battle setup |
| 103602, 103604 | 0x48CA8E | `(rand()&7)*0x4000000 \| (rand()&0x30)<<8` — packs a **bit field** from two rands | **always exactly 2 rands; the first is only 3 bits of the result** |
| 105974 (dup of 2.3), 113757–113763 | 0x49B6C7 | 4 rands stored at `param_1+0,+4,+8,+0xc` — a **4-int random key/state vector** | editor/base |
| 105849/105857 dup of 2.3 | | | |
| 105896/105898 dup of 2.3 | | | |

### 2.5 Editors, world generation, tactics and other dialog code — solo

| all.c | VA | Use |
|---|---|---|
| 48597, 48599, 48619 | 0x443929 | the generated map's width/height `(rand>>4) % n` and a `0..3` terrain/feature pick — **all inside the `srand(param_1)` region**; count is loop-driven |
| 52940 | 0x449BD6 | `rand()%5` chooses one of 5 tiers, then a loop of **up to 1000 iterations** calling `FUN_00448fd3(tier)` — up to 1000 rands for one dialog open |
| 7056* — see 2.2/2.3 | | |
| 62052, 62118 | 0x456AA1 / 0x456B87 | Tactics/PK slot timing (also in §2.2) |
| 61431, 83896, 85451, 116587 | — | the `SetTimer` sites, §3.4 |
| 121261, 121374, 121383 | 0x4A4D70 | also used by the editor's particle preview |

### 2.6 Chat / net-only

| all.c | VA | Use |
|---|---|---|
| 26475 | 0x423C6D | rand embedded in a synthetic-reality.com URL, returned to the caller |
| 30576 | 0x42895C | netgraph jitter |
| 32144–32149 | 0x42B4E0 | SRN stepper entropy |
| 34171/34172, 34293/34294, 34614 | 0x42CB8F | server opcode / server-spawned battle actors |
| 39072 | 0x436C9D | `GetTickCount`-bounded `rand()%100 < 30` — adjacent to `"Calm Down! Please"` / `"You haven't finished packing your…"`; the packing path is solo, the 30 % roll is a **net-session guard** |
| 45939/45940 | 0x43FEFF | 10 % special animation, time-gated — runs solo |

---

## 3. Time sources

### 3.1 `time()` (20 sites) — none of them gate a rule; all are wall-clock stamps

| all.c | Purpose |
|---|---|
| 156, 704 | per-character / per-slot timestamps stored beside record data |
| 6755 | boot: timestamp for the SRNet driver path (`srwsipx`) |
| 5803 | **boot: `DAT_0053866c = time(NULL)`, paired with `_DAT_004dd0f0 = GetTickCount()`** — the epoch/session origin |
| 7551, 16338, 16385, 16776, 16921, 23076, 24022, 24066, 32974, 33519, 35157, 36947, 41032, 41307, 41319, 41807, 42014, 43585, 47385 | record stamps, `lastDownload`, network-failure deadline, chat timestamp, mission deadlines |

`time()` feeds **no** rand and **no** turn/timer decision in solo play; it is display and persistence only.
The two that matter for the port are the two `srand` seeds (§1) and `DAT_0053866c`/`DAT_004dd0f0` as the
session origin used by `FUN_004094a3` (online-playtime accounting).

### 3.2 `QueryPerformanceCounter` — 2 sites, both in the net perf graph

`all.c:30593` and `all.c:30603`, both inside `FUN_00428939`/`FUN_0042895c` (0x428939/0x42895C).
Core is right: these are netgraph/anti-tamper only. Do **not** model them in the port.

### 3.3 `GetTickCount` — the only rule tick source

~200 call sites. Core's `docs/re/timing.md` is the authority for the full table; the parity-critical ones
(i.e. those that *gate, bound or seed* a rand call) are:

| all.c | VA | What it gates |
|---|---|---|
| 25250, 25259, 25270 | 0x42198F | the Quadris splash's random-rect loop — **elapsed-time bound ⇒ nondeterministic rand count** |
| 30577 | 0x42895C | the 25 ms gate on the discarded `rand()` at 30576 |
| 32148 | 0x42B4E0 | mixed *into* the 6-rand hash |
| 39071 | 0x436C9D | the 30 % `rand()` at 39072 is inside a `GetTickCount() < t0+t1` short-circuit |
| 45937 | 0x43FEFF | 500 ms deadline gates the 2 rands at 45939/45940 |
| 70925, 70927, 70929 | 0x46260E | the **5 s encounter cooldown** gates rand calls 70922 and 70926 — the single most fragile ordering in the file |
| 98175 | 0x485F21 | timestamp written next to the two rands at 98151/98172 |
| 107375 | 0x491E45 | `actor[0x11f] = GetTickCount() - rand()%5000 - 3000` — **clock and rand are combined into one state word** |
| 13411–13440 | 0x412D2F | the scene's 100 ms tick: `sin(GetTickCount()*k)` drives the ambient wobble; **it is a per-100 ms handler, not a frame** |
| 13831, 13952 | 0x4135DA | `sin(tick*k + …)` actor bobble; `GetTickCount() & 0x200` gates a 3-value variant |
| 21112, 21182, 21243 | 0x41xxxx | the front view's 13 s idle check (`GetTickCount() - [this+0x13F4] > 13000`) |
| 21329 | 0x40A7C7-family | the 5-minute `0x493E1` SRNet check |
| 27785–27791 | 0x4267C2 | 30 s `Sleep(100)` wait loop at boot |

Everything else (`0x4000`-region throttles, the 21725–21795 Work-DIB draw chain, the 49231/50093 effect
schedulers, the 62046 1 s sprite cadence, the 85451/83896 dialog timers) is presentation.

**Message-pump facts to keep in the port (confirmed against Core's `clock.h` note):**
- The pump is `CWinApp::Run` at **0x40A8D9**, order **INPUT → TIMER → IDLE/PAINT**.
- The idle gate is `FUN_0040A7C7` at **0x40A7C7**: `GetTickCount() - last > 19` — i.e. a **20 ms** idle tick, not 16.7 ms and not 60 Hz.
- The import table contains **no** `timeGetTime`, no `GetMessageA/TranslateMessage/DispatchMessage`
  (the loop is a bare `PeekMessage` pump). So the port has exactly one tick source: `clock_ms()`.

### 3.4 `SetTimer` / `KillTimer` — 6 + 6, all `TIMERPROC = 0` (WM_TIMER to the window)

Every one stores the returned timer id in a member field and re-arms only if the field is 0. No
`case 0x113` exists in `all.c` — the handlers are reached through the MFC message map, so the handler
bodies are the `__fastcall` methods of the same class; I have named the owning class from the
SetTimer/KillTimer sibling pair and from identifying strings, and flagged the rest as inferred.

| all.c | VA | Set / Kill | id | interval | window / class | handler | subsystem |
|---|---|---|---|---|---|---|---|
| 52216 set / 52019 kill | 0x4483A6 / 0x448375 | SetTimer | **2** | **100 ms** | `this+0x20`, id stored at `this+0x1FAC`; class also owns the `cadence.mid` sound (`FUN_0040B1AC` ctor) | MFC `OnTimer` of the `FUN_004483xx` dialog | front/UI — **solo** |
| 52327 kill | 0x448CFC | KillTimer | — | — | same `+0x1FAC` slot (destructor path) | — | front/UI |
| 52601 set | 0x449144 | SetTimer | **4** | **1000 ms** | `this+0x20`, id at `this+0x140`; the class loads `shopSquareBorder256.bmp`, uses `FUN_00448FD3` over 5 tiers and `Tempus Sans ITC` fonts | MFC `OnTimer` of the `FUN_00449xxx` shop/errand dialog | panels/shop — **solo** |
| 61431 set | 0x455F56 | SetTimer | **2** | **100 ms** | `this+0x20`, id at `this+0x60`; the class's other method prints `"Tactics dlg ini found (%d) tactics)"` / `"WoS Tactics for %s"` | MFC `OnTimer` of the **WoS Tactics** dialog | minigame/Tactics — **solo**, but Tactics itself is a net-tracked feature |
| 83896 set | 0x472292 | SetTimer | **2** | **1000 ms** | `this+0x20`, id at `this+0x60`; the class owns `"LagOMeter"` (`FUN_004722B2`) and fills a list of in-progress items `"In: %d"` from `DAT_0054a2a8` / `DAT_0055a062` | MFC `OnTimer` of the status / PK-duel list dialog | **net-only** (PK list) |
| 85451 set | 0x4742F0 | SetTimer | **2** | **50 ms** | `this+0x20`, id at `this+0x60`; the class owns `FUN_0047437E` (9-rand `float10` generator) and `FUN_004743E8` (4-column playfield pick) | MFC `OnTimer` of the 20 Hz minigame/animation dialog | minigame — **solo** |
| 116587 set | 0x49EF7B | SetTimer | **2** | **100 ms** | `this+0x20`, id at `this+0xb8`; the class shows `"Select an object to edit"` (`s_Select_an_object_to_edit_005071B4`) and calls `FUN_0049EDC9` | MFC `OnTimer` of the **object editor** | editor — **solo** |
| 3737 kill | 0x405D88 | KillTimer | — | — | `param_1+0x60` (dialog ctor/dtor) | — | front/UI |
| 4099 kill | 0x406687 | KillTimer | — | — | `param_1+0x60` | — | front/UI |
| 8104 kill | 0x40B1AC | KillTimer | — | — | `param_1+0x77C`; ctor calls `FUN_00436FC1("cadence.mid")` | — | front/UI |
| 13092 kill | 0x412797 | KillTimer | — | — | `param_1+0x60` | — | front/UI |

Bonus timer-like handler with no `SetTimer`: **`FUN_0044A63A` at 0x44A63A (all.c:53498–53669)** is the
status window's own `OnTimer`-style tick — it is literally named by its assert string
`"Status::OnTimer freshening monsters"` (all.c:53683) and refreshes monsters every 1000 ms
(`if (1000 < GetTickCount() - [this+0x44])`, all.c:53682). **Solo, and it changes battle contents** — the
port must reproduce it, not treat it as cosmetic.

---

## 4. Ordering risks

**A. Sites where the rand *count* for a single user-visible event is data-dependent.** These are where a
differential replay will diverge first:

1. **0x46260E encounter roll (70922, 70926).** Two rands, each independently short-circuited by a
   `GetTickCount()` 5 s cooldown and by `param_4`/mode flags. Any change in how often the handler runs
   changes the *number* of rands, and therefore every later value in the session. Highest risk.
2. **0x48E810 monster spawn (105036, 105054, 105077, 105079).** One seed rand, then a 0x300-slot ring
   walk that draws **up to 768 more** rands, then 1 or 2 more. Total per spawn ∈ [2, 771].
3. **0x485FF5 the `%3` insult generator (98195–98218).** The code is:
   `if (param_2==1) goto PICK1; else if ((rand()&0xc00)==0) skip;`
   `if (param_2!=2 && (rand()&0xc00)==0) goto DONE;`
   then PICK1 draws 1 rand, the base-court clause draws 1 rand, and `param_2==3` draws 1 more.
   → **1, 2 or 3 rands**, and the *discarded* rands at the two `&0xc00` gates are consumed whether or not
   they take the branch. Both must be modelled.
4. **0x449BD6 (52940).** Up to 1000 iterations, each one `rand()%5`, guarded by `0 < iVar4 && local_8 < 1000`.
5. **0x42198F Quadris splash (25262/25263).** Loop bound is `GetTickCount() - start < param_2`, 2 rands per
   iteration ⇒ the count is wall-clock-dependent. It runs at boot, so it poisons `rng.calls` for the whole
   session unless the loop is driven by the virtual clock and the port accepts the same nondeterminism (or
   the replay pins the clock).
6. **0x46E3AA (80639–80643)** 3 rands × a caller-supplied iteration count; **0x46E29B (80594/80596)**
   exactly 200; **0x46E14C (80536–80555)** 5 + a `sin`-driven loop.
7. **0x46E4D1 (80844, 80850, 80856)** — three sequential 80 %/50 %/30 % gates, so 0–3 rands.
8. **0x492900 (107836–107857)** — 2+2 rands, two independent placement branches.
9. **0x4915ED (106952/106958)** and **0x48F913 (105896/105898)** and **0x4A3A54 (120087/120089)** —
   unconditionally paired rands. The risk is *dropping the second*, not the count varying.
10. **0x42B4E0 (32144–32149)** — `do…while` that re-draws 5 rands + `GetTickCount` until a fold is non-zero.
    Unbounded in principle; the folded value is `(…<<4^…^tick^…&0x3FFFFFFF)`, so it terminates almost
    immediately, but it is *not* a fixed count.

**B. Ordering that must be preserved exactly (not just the count):**

- **0x443929**: `rand()` → `srand(seed)` → generation → `srand(saved)`. The first `crt_rand()` must precede
  the first `crt_srand()`, or every subsequent value in the session shifts.
- **0x4269AF**: `srand(t)`, `rand()`, …, `srand(t)` again 34 lines later. Both seeds run; the second wins.
- **0x48CA8E (103602/103604)**: the first rand contributes only 3 bits (`&7`) but must still be counted
  before the second.
- **0x412E2F (13525–13529)**: an RGB triple is built from **three consecutive rands** in a fixed order
  (`r<<8 | g<<10 | b`); a port that collapses them to one rand changes the colour *and* the stream.
- **0x4A8AFB (123258)**: the rand result is stored in a global (`_DAT_004dd52c`) and XORs every subsequent
  `FUN_0040ab35` cookie write. It is a rand with lasting state, not a scratch value.
- **0x491E45 (107376)**: `GetTickCount()` and `rand()%5000` are combined into `actor[0x11f]`; both
  `clock_ms()` and `crt_rand()` must be sampled at the same point in the same order.
- **0x4A6974 (121868–121884)**: up to 4 rands, one of which is a bare `rand();` whose value is discarded.
  A decomp-driven port that omits a "useless" rand breaks the stream.
- **0x48B1AD (102929)**, **0x4851F1 (97698)**, **0x4904EB (106193)**: the roll happens and the result is
  then overridden by a flag (`DAT_00502b14`, `*(int*)(iVar4+0x394)`). The rand is still consumed.

**C. Per-frame call sites that are *not* per-frame.** Nothing in the original re-runs on a 60 Hz frame
(Core confirmed this independently). Several rand sites look like they are in a draw/animation path
(0x46E14C, 0x46E29B, 0x46E3AA, 0x487D76, 0x4868C0) — they are called from `OnPaint`/idle paths whose
**invocation count is message-driven**, so a port that calls them once per rendered frame will diverge
immediately. Map each to the original's actual trigger (idle tick, `WM_PAINT`, or a message) before
implementing.

---

## 5. Facts for the orchestrator / doc corrections

1. **The plan's "182 rand call sites" is 180 `rand()` + 4 `srand()`.** The parity plan and
   `work/parity_plan.md` §1 describe them as "182 rand call sites and 4 srand sites"; the true split is
   180/4. Worth fixing in the plan text so owners do not go looking for 2 extra rands.
2. **There is no `timeGetTime`** anywhere in `all.c` (grep over the whole file returns nothing), consistent
   with Core's import-table finding. `clock_ms()` is the single tick source, as the plan already states.
3. **`srand` at 0x443929 is a save/restore pair, not a plain reseed** — this is the one seeding site with
   ordering significance and it is not described in any of `docs/re/*.md`.
4. **`FUN_0044A63A` is a real solo behaviour** (status window refreshes monsters every 1000 ms, named by its
   own assert string at all.c:53683). It is not in `docs/gap_analysis.md` and is not cosmetic — it changes
   which monsters are in the fight. It needs an owner and a `clock_set_timer(…, 1000, …)`.
5. **`FUN_00485FF5` (0x485FF5) is the `%3` insult generator** and `docs/re/script.md` §4.4 correctly lists
   `%3` as missing in the port. The port needs the **1/2/3-rand** variant structure, not one roll.
6. **`FUN_0048CA8E` (0x48CA8E) needs 2 rands** for one bit-packed value; a single-rand port implementation
   of the equivalent bit-field will desynchronise everything after it.
7. The `SetTimer` inventory in §3.4 is new information: six periodic dialog timers at
   **100 ms ×3, 1000 ms ×2, 50 ms ×1, 4 ×1**, all `TIMERPROC=0` (WM_TIMER). Only the `0x4483xx` (100 ms,
   `+0x1FAC`) and `0x455F56` WoS Tactics (100 ms, `+0x60`) handlers are confirmed by identifying strings;
   the other four owners' handler bodies are reached via the MFC message map and I have not matched them
   to specific methods. `docs/re/timing.md` (Core) should own that last mapping.


## Sources

- `work/decomp/all.c`: Source of every citation: Ghidra decomp of extracted/Souls.exe. 138000 lines, 180 rand() + 4 srand sites, ~200 GetTickCount sites, 6 SetTimer / 6 KillTimer sites, 2 QueryPerformanceCounter sites, 0 timeGetTime.
- `work/decomp/functions.tsv`: Function index (VA, name, byte size) used to sanity-check region identity for the resolved VAs.
- `docs/re/battle.md`: Naming authority used for: FUN_0046260e encounter roll, FUN_00464daf .mon resolver, FUN_0049099b groups.txt resolver, FUN_00490e7c battle round state machine, FUN_0048f913 actor picker, FUN_0048f816 monster flee, FUN_0048e810, FUN_004904eb to-hit, FUN_004946b2 apply damage, FUN_004a7794 damage roller, FUN_0042b867 trophy roll, FUN_0042bb5c payout, FUN_00494fcd resurrect, FUN_00480499 monster spawn, FUN_00448fd3 (5-tier cost/difficulty), FUN_0046260e 5s cooldown.
- `docs/re/maps.md`: Naming authority for the map subsystem sites (walkability, links, encounter roll and its 5 s cooldown, .mon proximity).
- `docs/re/script.md`: Naming authority for scene-VM / cookie / %-substitution sites, incl. the %3 random-insult generator and the secure-cookie key refresh.
- `docs/re/boot_flow.md`: Naming authority for front-end, New Soul creation and death/resurrect sites.
- `docs/re/formats_online.md`: Naming authority for item classes, OFFER/OFFER2 shop filter, and the WoS Tactics (net-tracked) surfaces.
- `docs/gap_analysis.md`: Existing port gap list; used to classify which rand sites are already mirrored by the port and which are not.
- `docs/re/rng_calls.md`: Destination for this census (the orchestrator saves it here; I am read-only and wrote nothing).
