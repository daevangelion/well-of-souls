# quest.txt language and tables (Souls.exe)

## Summary

Documented the WoS quest.txt language, the scene interpreter, all table column layouts, and the world INI files as a C-port reference, with every claim cited to a Ghidra VA in work/decomp/all.c or a file:line in extracted/.

## Architecture

The world is a self-contained data package under `worlds/<Name>/`: `quest.txt` (a single flat script file, `#include`-composed) which carries BOTH the world definition tables and all the scene scripts; binary per-map files (`maps/<root>.jpg|.obl|.mon|.ter`); binary art (`art/`, `scenes/`, `Monsters/`, `skins/`); `MIDI/`, `SFX/`, `HTML/`; and a handful of INI files. Load order (VA 0x479798 FUN_004798a9 → 0x4797a7 FUN_0047977a → 0x479594 FUN_00479594 file reader → 0x4798a9 line splitter → 0x4799b3 section matcher → 0x479a02 FUN_00479a02 section dispatcher): all files are slurped into ONE flat text buffer, then split into a line-pointer array, then paired +/- sections are located, then 14 table parsers consume their line ranges, then the map binaries and the INI files are hashed into a world CRC.

# WoS quest.txt scripting language + table formats — implementer reference

All addresses are VAs in `work/decomp/all.c`. Column lists marked "hdr:" are quoted from the
author's own comment headers in the shipped data files (the author states the header *is* the spec).
Keyword names marked **[V]** are verified against the decompiled dispatch; those marked **[D]** come
from the author's official Quest Dictionary (synthetic-reality.com/wosquest.htm) and their pairing
with a specific opcode is inferred from semantics.

---

## 1. Load pipeline and lexical rules

### 1.1 Reading and `#include`  (FUN_00479594, 0x479594)

```
fopen(path); while (fgets(buf, 39999, f)) {
    skip leading {'\r','\n',' ','\t'};
    if (strlen(line) <= 1) continue;
    if (line[0] == ';') continue;                    // whole-line comment
    if (_strnicmp(line, "#include", 9) == 0) {      // 9 chars, case-insensitive
        dirname = strrchr(curpath,'\\') ? after it : whole string;
        p = line+9; skip {'\r','\n',' ','\t'};
        strcpy(dirname, p);
        q = strrchr(dirname, '"');  if (q) *q = 0;   // truncate at LAST quote
        truncate at first byte < 0x20;               // kills \r\n and trailing junk
        FUN_00479594(dirname, ...);                  // RECURSE, text spliced in place
        strcpy(line, "");                           // the #include line itself becomes empty
    }
    append line (strlen bytes) + NUL to growing buffer (realloc +10000)
}
```

Consequences for an implementer:
* `#include` is **filename only** — the resolved path is `dirname(current file) + <text after
  "#include " up to the first embedded control char>`. Subdirectories do not work.
* The text after the *last* `"` in the line is discarded, so `#include items.txt` and
  `#include "items.txt"` both yield `items.txt`; trailing `"` and anything after it is dropped.
* Included text is spliced **in place**, so line numbers in the final buffer are the concatenation
  of all files, and an included file that lacks a trailing newline will glue onto the next line.
* Included files are themselves scanned for `#include` (Evergreen's `questscenes170.txt` is
  lowercase and is never referenced by quest.txt — it is dead).
* Nesting depth is bounded only by the stack.
* quest.txt's own final comment exists because a truncated last line would otherwise be mis-parsed
  ("an old bug might rise up and smite you" — quest.txt:2391).

### 1.2 Line splitting  (FUN_004798a9, 0x4798a9)

The flat buffer is scanned once, char by char, producing a `char**` line table (`DAT_004fa848`):

```
if (*p == '\t') *p = ' ';                 // tabs -> spaces
else if (*p < ' ') *p = 0;                // all control chars incl \r \n -> NUL
if (inComment) { if (*p < ' ') inComment = false; }
else if (*p > 0x1f && *p != ';') { lines[n++] = p; inComment = true; }
```

* **A `;` anywhere on a line truncates the line at that point** (everything from the `;` onward is
  discarded, not preserved). So `GIVE I99  ; give him a ticket home` (quest.txt:1546) is fine.
* Lines are **not** right-trimmed (only the CR/LF that terminates them is eaten).
* Tabs become spaces, which is why the shipped files' tab-indented tables still tokenize.
* Blank/comment-only lines never enter the table, so scene line indices are dense.

### 1.3 Sections  (FUN_004799b3 + FUN_00479a02, 0x4799b3 / 0x479a02)

```
name = sectionNameTable[i];
if (_strnicmp(line+1, name, strlen(name)) == 0) -> section i      // PREFIX match, case-insensitive
'+'NAME : if (count[i] != 0) ERROR "*** Quest File has multiple %s";  start[i] = lineIndex
'-'NAME : start2[i] = lineIndex; len[i] = start2 - start - 1;
           if (len < 0) ERROR "*** Quest section is broken ***"
```

Per-section state is 16 bytes at `DAT_004fa858 + i*16`: `[+0]` name pointer, `[+4]` pair count,
`[+8]` first line index, `[+12]` length. Sections may appear in any order but must be well-formed
`+X` / `-X` pairs; a `-X` with no preceding `+X` yields length −1 and an error.

Section names recognised (author's list, quest.txt:31-47): `CREDITS STORY TERRAINS MAPS TOKENS
ELEMENTS HANDS ITEMS SPELLS MONSTERS GROUPS LEVELS SCENES EQUIP THEMES TROPHIES`. A `+SECTION` line
whose name does not prefix-match any of them logs `*** Unexpected Section name: %s`.

Table parsers are then run unconditionally over their line ranges (order in FUN_00479a02):
spells (0x47fced) → monsters (0x4809a3) → groups (0x483686) → levels (0x483984) →
tokens (0x4814d2) → items (0x482fc1) → trophies (0x4811b1) → terrains (0x481f8a) →
elements (0x48219a) → equip types (0x4824b4) → hands (0x4825df) → sound themes (0x48282e) →
maps (0x482c54). (0x481654 is a second pass that counts inline `TOKEN` lines inside +SCENES.)

### 1.4 Tokenizing one line  (FUN_0047a0e4, 0x47a0e4) — **this is the lexer**

Four-state scanner writing up to N tokens into `buf[i*0x104]` (260 bytes each):

```
state 0 (between tokens):  byte < 0x21 -> skip
                           ','        -> skip
                           '"'        -> state 2 (quote opens, char NOT stored)
                           else       -> state 1
state 1 (bare token):      byte < 0x21 -> emit token, state 3
                           ','        -> emit token, state 0
                           '"'        -> state 2
                           else       -> append (NUL-terminated in place each step)
state 2 (in quotes):       '"'        -> emit token, state 3
                           else       -> append (including commas and spaces)
state 3 (just closed):     byte > 0x20 -> if ',' state 0 else { rewind one char; state 0 }
                           else       -> skip
```

Rules that follow:
* **Separators are `,` AND any whitespace (byte < 0x21).** They are interchangeable. There is no
  quoted token that can contain a comma unless it is inside `"..."`, and quotes may only *open* at
  the start of a token.
* An **unterminated quote swallows the rest of the line** as one token (Evergreen does this on
  purpose: `MOVE 1, -50,50` and `ACTOR 3, Miranda, joshMiscellaneous, 19, 110, 78` rely on
  commas; the `'`-speech line `1: Owa` relies on plain space splitting).
* Empty quoted token `""` yields a **zero-length token**, which still occupies a slot. Evergreen
  uses this: `MUSIC ""` (QuestScenes60.txt:20) means "silence".
* `'` is NOT a quote character to the lexer — it is a *keyword* (`case 0x27 in FUN_0047a6b0`).
* The number of tokens returned is what the interpreter dispatches on; a keyword with no args
  returns 1.

### 1.5 Keyword dispatch  (FUN_0047a6b0, 0x47a6b0)

```
b = tok0[0];
if (b == '@')            return 0xFFFFFFFE;              // a label line, no-op
if (b == '\'' || b == '"') return 9;                      // ' speech
if (isdigit(b) && tok0[1] == ':') {
        *(int*)(DAT_004e4874+0x3E028) = b & 0x0F;         // current actor = digit
        return 9;                                          // "N: text"
}
if (toupper(b)=='H' && tok0[1]==':') return 0x25;         // H: = host speaks
if (toupper(b)=='N' && tok0[1]==':') return 0x28;         // N: = narration
for (i = 0; i < DAT_004FAFF8; i++)
        if (_stricmp(tok0, table[i].name) == 0) return table[i].opcode;
return 0xFFFFFFFF;                                        // "*** Unknown keyword: %s in line %d"
```

The table is a flat array of 8-byte `{int opcode; char *name;}` records at `DAT_004FAD50`
(names at `DAT_004FAD54`), count in `DAT_004FAFF8`. The raw bytes live in `.rdata`; the PE
string dump was not reachable from this environment, so opcode↔spelling pairs below are marked.

**The `N:`/`H:`/digit forms take their text from the RAW LINE, not the token buffer**
(FUN_0047d577 `case 9`): `line_ptr + 2` (skip `N:`), and for `case 0x25`/`case 0x28` it first skips
past the first space so the whole remainder becomes the bubble text. This is why a colon-dialogue
line can contain commas and colons freely.

### 1.6 Argument fetching

`FUN_00484b3e` (0x484b3e) is the universal "get next argument" routine: it walks the remaining
tokens of the current line, expanding `%X` substitutions in place, and returns the new length.
`FUN_0047cfde` (0x47cfde) pushes a fully-expanded string into the scene chat/bubble channel.
`FUN_0047c6da` (0x47c6da) resolves one `#<name>` / stock-cookie name into a value string.

---

## 2. The scene machine

### 2.1 Scene state (DAT_004E4874 block)

| offset | meaning |
|---|---|
| `+0x10` | current scene number |
| `+0x11C` | return line index (for CALL) |
| `+0x120` | **program counter = current line index** |
| `+0x128` | per-hero scene slots (0x1B8 bytes each, 0x3DE00 range ⇒ 144 hero slots) |
| `+0x3E028` | current actor ID |
| `+0x3E038` | WAIT start tick |
| `+0x3E03C` | WAIT remaining time |
| `+0x3E060/64/6C` | scene fx / weather / colorTable |

PC semantics: the interpreter runs **one line per call**; every opcode advances
`*(int*)(DAT_004E4874+0x120)++` unless it jumps. `*param_2` is set to 1 to request another step.
`DAT_004FB00C` is a re-entrancy guard; `DAT_004FA84C` is the total line count.

### 2.2 Opcode → keyword table

| op | keyword | syntax | verified behaviour |
|---|---|---|---|
| 0x00 | `THEME` | `THEME [n]` | `atoi(arg)` → `FUN_00456d2f`. −1 stops, 0 = use nearest link's theme [V] |
| 0x01 | `MUSIC` | `MUSIC <file>` | `FUN_00436fc1` (MIDI). `""` or missing file = silence [V] |
| 0x02 | `SOUND` | `SOUND <wav>` | `FUN_00484b3e` + `FUN_0047d464` (mode 1 = 3D positional, 2 = "all", else normal) [V] |
| 0x03 | `GOTO <label>` | label may be `47@label` | `FUN_0047a77b`; sets `+0x11C` if a scene number was given [V] |
| 0x04 | `GOTO EXIT` / `GOTO SCENE n` / `GOTO LINK map,link[,dropin]` / bare label | | `_stricmp(tok0,"EXIT")` → PostMessage 0x480 then PC = −1; `"SCENE"` → PostMessage(0x475, n) then PC = −1; `"LINK"` → SendMessage(0x476) + (0x46a, link, map), and if dropin≠0 clears the fade flags; anything else → label jump [V] |
| 0x05 | `ACTOR` | `id[.layer], "name", skin, pose, x, y [,colorTable][,pain.wav][,mode]` | 6 numeric args, name/skin strings; stores `id & 0x3F` into `+0x3E028`; if x,y omitted calls `FUN_004923c2`, else `FUN_004923ec` [V] |
| 0x06 | `POSE` | `POSE p [,p2,p3]` | 3 args → `FUN_00492563` [V] |
| 0x07 | `MOVE` | `MOVE id, x, y [,mode]` | 3 args → `FUN_00485e75`; mode 1 = teleport, 2..5 = 2×..5× walk speed; `MOVE H,..` moves the host [V] |
| 0x08 | `SEL` | `SEL <actorId>` | `*(DAT_004E4874+0x3E028) = atoi(arg)` [V] |
| 0x09 | `'` or `N:` | raw text after the token | `FUN_0047cfde` bubble; waits (`FUN_0046c44a`) until the previous bubble clears before advancing [V] |
| 0x0A | `WAIT` | `WAIT <seconds float>` | stores `GetTickCount()` at `+0x3E038`, `ftol(atof)` at `+0x3E03C`, returns state 2 (blocked, no PC advance) [V] |
| 0x0B | `FIGHT` | `FIGHT [monsterID[.x.y[.facing]]]...` | see §2.3 [V] |
| 0x0C | `OFFER2` | 2 numeric args | `FUN_00439f7a`+`FUN_00484e72` — "lowest level/max level" form with no class [V] |
| 0x0D | `OFFER2` | 3 numeric args | + `FUN_0045f37c` (filter by item class) [V] |
| 0x0E | `OFFER` | item list, variable length | loops `(nTokens−1)` times appending ids to a list → `FUN_0042126e`; caps at 100 entries (`strlen < 800`, `count < 100`) [V] |
| 0x0F | `ASK` | `ASK <seconds> [,1]` | sets wait state 9, `_DAT_004E62DC = atoi(arg2)`, clears the yes/no flags via `FUN_00429c9c` [V] |
| 0x10 | `END` | — | `PC = 0xFFFFFFFF`; `FUN_00479115`/`FUN_0047929f` clear the sub-interpreters; returns state 3 [V] |
| 0x11 | `GIVE` | object id | `FUN_00421529` [V] |
| 0x12 | `OFFER2` (repeat form) | triples `min,max[,class]` | walks the **item table** (`DAT_004E4884`, stride 0x30C) testing `level ≥ min && level ≤ max`, `price>0`, `inUse`, and optional class == arg3; appends to list, 100 cap [V] |
| 0x13 | `BKGND` | `BKGND <jpg>` | `FUN_0048a62c`; supports `name.WIDTH.HORIZON` (percent of window width, horizon %) [V] |
| 0x14 | `WEATHER` | `WEATHER n` | `FUN_0048a658` [V] |
| 0x15 | `FX` | `FX n` | `FUN_0048a67b` [V] |
| 0x16 | `LOCK` | `LOCK 0/1` | `FUN_0048a7a1` [V] |
| 0x17 | `GAME` | `GAME n` | `FUN_0048c5fe`; if it returns non-zero the next line is treated as the button label [V] |
| 0x18 | `COUNTDOWN` | `COUNTDOWN <seconds>` | `DAT_004FA854 = n*1000`; if 0 then `DAT_004FA850 = 0` (off) else `= GetTickCount()` [V] |
| 0x19 | `FIGHT2` | as FIGHT | sets `DAT_00502B14 = 3` then falls into the FIGHT body (sticky fight) [V] |
| 0x1A | `SET` | `SET cookie, value` | `FUN_0047ab07(name, expandedValue)` [V] |
| 0x1B–0x1F | `ADD` `SUB` `MUL` `DIV` `MOD`(`MODULUS`) | `OP cookie, amount` | `FUN_0047d1df(cookie, amount, op, isFloat)` — op 0 add, 1 sub, 2 mul, 3 div, 4 mod; integer (`atoi`) or double; div/mod by 0 is a no-op; result is also the **CONDITION CODE**; result written back via `FUN_0047ab07` [V] |
| 0x20 | `F_COMPARE` **[D]** | — | no local arg read; logs only — pairing unverified |
| 0x21 | `COMPARE` | `COMPARE A, B` | `CONDITION_CODE = atoi(A) − atoi(B)` [V] |
| 0x22 | `IF=` | | jump if CONDITION_CODE == 0 [V] |
| 0x23 | `IF>` | | jump if CONDITION_CODE ≥ 1 (A>B) [V] |
| 0x24 | `IF<` | | jump if CONDITION_CODE ≤ −1 (A<B) [V] |
| 0x25 | `H:` | | host speech bubble [V] |
| 0x26 | `COLOR` | `COLOR n`, `n[table]`, `n000[table]`, `n200[table]` | `FUN_0048a7da`; the string is uppercased first (`toupper(tok0[0])`) [V] |
| 0x27 | `FACE` | `FACE actorId, dir` | two args → `FUN_0048a8ae` [V] |
| 0x28 | `N:` | | narration; no bubble (`FUN_0046ceab`) [V] |
| 0x29 | `HTML` | `HTML <url-or-path>` | `FUN_0048a69e`; on success returns state 10 (suspend until closed) [V] |
| 0x2A | `FLAGS` | `FLAGS <mapFlags>` | `DAT_004FB030 = atoi(arg)`; `FUN_0041ad11` re-evaluates the map rules. Accepts `0x…` [V] |
| 0x2B | — | | `FUN_0048c5fe`; on non-zero jump to a label. This is the **second half of `GAME`** (the mini-game's own "on completion" label) [V] |
| 0x2C | `STRCMP` | `STRCMP a, b` | `CONDITION_CODE = _stricmp(a,b)` [V] |
| 0x2D | `STRSTR` | `STRSTR a, b` | both `_strlwr`ed, then `CONDITION_CODE = (strstr(a,b) != NULL)` [V] |
| 0x2E | `MISSIONS` (alias `MISSION`) | `MISSIONS j1,j2,...` | `FUN_00484e72` [V] |
| 0x2F | — | | `FUN_00484e72`+`FUN_0045f37c` [V] |
| 0x30 | `SET_LEN` | `SET_LEN cookie` | `FUN_0047ab07(cookie, itoa(strlen(expanded)))` [V] |
| 0x31 | `SET_SUBSTR` | `SET_SUBSTR cookie, start, len` | clamps start/len into `[0, strlen]`, truncates, stores [V] |
| 0x32 | — | | no local arg read; logs only (pairs with 0x20) [UNVERIFIED] |
| 0x33 | — | | `FUN_00484e72`; optional extra log if `*(DAT_0067FBF8+0xCC) != 0` [V] |
| 0x34 | — | | `FUN_00484e72`+`FUN_0045f37c`; same conditional log [V] |
| 0x35 | `PARTY` | `PARTY <condlist>` | `FUN_0049782b`; if it returns 0 the interpreter sets a 10-second wait and reads 3 more tokens (the multi-party forms) [V] |
| 0x36 | `EJECT` | `EJECT <condlist>` | same but without the re-wait [V] |
| 0x37 | `IF<=` | | CONDITION_CODE ≤ 0 [V] |
| 0x38 | `IF>=` | | CONDITION_CODE ≥ 0 [V] |
| 0x39 | `IF<>` / `IF!=` | | CONDITION_CODE ≠ 0 [V] |
| 0x3A | `CALL` | `CALL [scene@]label [,arg0..arg9]` | `FUN_004791aa` pushes the current line; jumps; `FUN_00479120` = stack-full check ("*** CALL STACK FULL ***") [V] |
| 0x3B | `RETURN` | `RETURN [cookie…]` | pops, sets PC and scene number, refreshes the stepper [V] |
| 0x3C | `PUSH` | `PUSH cookie` | `FUN_004792d1` [V] |
| 0x3D | `POP` | `POP cookie` | `FUN_0047933a` [V] |
| 0x3E–0x42 | `F_ADD F_SUB F_MUL F_DIV F_MOD` | | same body as 0x1B–0x1F (double precision) [V] |
| 0x43 | — **[D]** | 2 args, one `atof`, one `atoi` | `FUN_0049454f`; unpaired keyword [UNVERIFIED] |
| 0x44–0x47 | `AND OR XOR NOT` | `OP cookie[,amount]` | `FUN_0047d374(cookie, amount, op)`: 0 = `c & a`, 1 = `c \| a`, 2 = `~a`, 3 = 0. Result is also the CONDITION CODE [V] |
| 0x48 | `IFEVEN` | | `(CONDITION_CODE & 1) == 0` [V] |
| 0x49 | `IFODD` | | `(CONDITION_CODE & 1) == 1` [V] |
| 0x4A | `HOST_GIVE` | object id | `FUN_00421314` (vs `FUN_0042126e` for `GIVE`) [V] |
| 0x4B | `SHUFFLE` | `SHUFFLE <baseCookieName>, n` | Fisher-Yates over `0 < n < 256`, writes `<base><i>` cookies and `<base>Count` [V] |
| 0x4C | `GET_SERVER_VAR` **[D]** | `GET_SERVER_VAR n, n, name` | `atoi` then `FUN_0047d49c` [V] |
| 0x4D | (lowercases) | | `_strlwr(tok1)` then `FUN_0047ab07` — `SET_LOWER`-style cookie write [V] |
| 0x4E | `SET_SUBSTR`-with-delims **[UNVERIFIED]** | `n, m, "delims", str` | `strtok` walk: skip `n` tokens, then cut after `m` tokens; trailing spaces skipped if `m != 0` [V] |
| −2 (0xFFFFFFFE) | `@label` | | no-op, advance PC [V] |
| default | — | | `*** Unknown keyword: %s in line %d`, advance PC [V] |

Unassigned opcodes: 0x0B/0x19 (`FIGHT`/`FIGHT2`), 0x35/0x36 (`PARTY`/`EJECT`), 0x4A/0x30/0x0E
(GIVE family) are unambiguous from their bodies. The dictionary additionally lists
`PARTY_GIVE`/`PARTY_TAKE`/`HOST_TAKE`, `SET_SERVER_VAR`, `TIMER`, `TOKEN` — these must share
opcodes with the neighbours above (several spellings map to one opcode; the table holds one name
per opcode, so aliases are almost certainly folded) **[UNVERIFIED]**.

### 2.3 `FIGHT` in detail (shared body at `switchD_0047d643_caseD_b`, 0x47f9c8)

```
clear monster list (DAT_004FB000 = 0)
if (nTokens == 2 && atoi(tok1) == 0) useMapMonsters = true   // "FIGHT 0" = "also use map monsters"
for each scene hero slot (stride 0x6E0):
    if (slot alive && slot.state == -1 && (slot.resistFlag == 0 || (resistFlag == 0x7FFFFFFF && useMapMonsters)))
            FUN_0048e16e()                       // pull in that hero's escort/mercenaries
if (!useMapMonsters) {
    if (nTokens < 2) { state = 4; }              // bare FIGHT: standard map fight
    else for each remaining token t:
            if (strcmp(t, "*") == 0) sawStar = true;
            else { friendly = (*t == '+'); id = atoi(t);
                   FUN_00480499(abs(id)); FUN_0047c1e4(abs(id)); }
    DAT_005006B8 = 5; *(DAT_005006BC+0x78) = 1;
    state = sawStar ? 4 : 5;                     // 4 = normal fight, 5 = forced
}
advance PC
```

So: `FIGHT` with no args = ordinary map fight; `FIGHT *` = ordinary map fight **plus** the listed
monsters; `FIGHT 0` = the map's monsters only; `-n` = friendly/mercenary; `+n` = pet-hating
monster. After the fight, `IF WIN` / `IF LOSE` / `IF ALIVE` / `IF DEAD` test the outcome.

### 2.4 Label resolution (FUN_0047a77b, 0x47a77b)

```
q = strchr(arg, '@');
if (arg && *arg != '@' && q) {                 // "47@label"
    n = atoi(arg); line = FUN_0047a2a7(n, 1); // locate scene n's SCENE line (dry run)
    *outScene = n; arg = q; if (line < 0) return -1;
}
start = line + 1;
for (i = start; i < DAT_004FA84C; i++) {
    if (_strnicmp(lines[i], "SCENE", 5) == 0) return -1;         // never search past the next SCENE
    if (_strnicmp(lines[i], arg, strlen(arg)) == 0 && lines[i][strlen(arg)] < 0x21) return i;
}
return -1;
```

* Label lookup is **scoped to the current scene** and stops at the next `SCENE` line.
* Label matching is case-insensitive and requires the match to end at a token boundary.
* `+1` in the `jumped` path means "resume at the line *after* the label".
* Failure logs `*** SCENE *** unable to find label` and advances (no infinite loop).

### 2.5 Entering a scene (FUN_0047a2a7, 0x47a2a7)

Scans forward from the current index for the first line with ≥2 tokens whose tok0 is `SCENE` and
whose tok1 parses to the requested number. Then:

```
if (arg4 == 0) (*(DAT_004E4874+0x10)) = n;  stepper;  return line;    // probe only
*(DAT_004E4874+0x11C) = line; *(DAT_004E4874+0x120) = line + 1;
if (nTokens < 3) bg = nearest-link bg jpeg (or "fight")
if (nTokens < 5) title = nearest-link title (or "Lake Louise")
if (nTokens < 6) fx    = nearest-link fx    else fx    = atoi(tok4)
if (nTokens < 7) wx    = nearest-link wx    else wx    = atoi(tok5)
if (nTokens > 3 && _stricmp(tok2,"CUT")==0) DAT_00502A4C = 1;      // hide heroes
if (nTokens > 7) colorTable = atoi(tok6)
```

⇒ **`SCENE` argument order is exactly**: `id, background, style, name, fx, weather[, colorTable]`.
Missing arguments are inherited from the link that dropped you in. Error path:
`*** Can't find scene: %d`.

### 2.6 Scene presentation (actors, bubbles, camera)

* **Actors** live in the scene's per-hero-independent actor table; 64 slots (IDs 0..63,
  masked with `& 0x3F`). Only IDs 0..9 can use the `N:` shorthand (single digit). Re-using an ID
  replaces the actor in place.
* Actor x/y are **percentages of the window** (0..100); values outside 0..100 place them offscreen
  so a `MOVE` can walk them in. `MOVE` mode 1 teleports; 2..5 walk at 2×..5× speed.
* `POSE p` holds a frame; `POSE p1,p2,p3` cycles p1↔p2 with random dwell and occasional brief p3,
  and the animation **continues after `END`**.
* Speech bubbles: only one at a time; the interpreter blocks on `FUN_0046c44a` before advancing.
  Right-click clears a bubble; otherwise it auto-pops after a time proportional to text length.
* `WEATHER`: 0 none, 1/2/3 rain light/med/heavy, 7/8/9 snow light/med/heavy.
* `FX`: 0 none, 1 underwater ripple, 2 lake ripple (bottom 30 %), 3 video (every other scanline
  black), 4 jitter, 5 earthquake.
* `STYLE`: `WELL` | `FIGHT` | `SCENE` | `CUT` (CUT = heroes invisible).
* Reserved scene numbers: **0** = Well of Souls (incarnation), **1** = camp, **2** = fight
  (Evergreen scene 2 is literally `SCENE 2 / THEME / FIGHT / END`, quest.txt:1560-1566).
* Asynchronous **events** re-enter a finished script via reserved labels:
  `@eventActorClick<n>`, `@eventActorGive<n>`, `@eventActorAttack<n>`, `@eventActorSpell<n>`,
  `@eventHostGive`. Evergreen uses `@eventActorClick9` for the camp lock toggle (quest.txt:1548).

---

## 3. Conditions

`IF <idlist>, @label` — but Evergreen also writes `IF T22, @truth` *and* `IF T44, @C4` /
`IF I186 @hasItAlready` (QuestScenes18x.txt:14), so **the comma before the label is optional**:
the label is simply the last argument and the condition list is everything before it.

`PARTY` / `EJECT` take the same condition list but evaluate it **on each client**, not the host.

### 3.1 Condition IDs (authoritative list, quest.txt:730-790 / wosquest.htm)

`ALIVE DEAD WIN LOSE YES NO XP GS` and

| id | true when |
|---|---|
| `#nn` | host is on map nn |
| `C n` | host's character class == n |
| `E nn` | item nn is **equipped** |
| `F nn` | current map has map flag nn |
| `G nn` | host has ≥ nn gold |
| `H nn` | character is ≥ nn hours old |
| `I nn[.mm]` | host **possesses** item nn (or ≥ mm of it) |
| `J nn` / `JA nn` / `JQ nn` | mission nn completed / accepted / qualified |
| `KB n[.x]` / `KM n[.x]` | killed by monster n ≥x times / killed monster n ≥x times |
| `M0 M2 M4` | cheater / played in modified world / ever disabled the "avoid modified quest" option |
| `P0 P nn` | player killer / PKer with ≥ nn kills |
| `Q word` | last `ASK` reply contains `word` (`^` = space) |
| `R nn` | random: true nn % of the time (00-99) |
| `S nn` | host knows spell nn |
| `T nn` | host has token nn |
| `V nn` | host level ≥ nn |
| `Z nn[.mm]` | host has trophy nn (≥ mm of it) |

Boolean composition: `+` = AND, `-` = NOT on that term, `|` (pipe, A84+) = OR between clauses,
evaluated left to right, first true clause wins.

### 3.2 GIVE / TAKE object IDs

`G n` gold · `H n` heal n HP (capped at max, does not resurrect) · `I n` item · `L1` resurrect
(take only) · `M n` restore n MP · `S n` spell · `T n` token · `Z n[.count]` trophy.
`GIVE I23.10` gives ten. `GIVE` → everyone in scene, `HOST_GIVE` → host only,
`PARTY_GIVE` → the host's party, `TAKE`/`HOST_TAKE`/`PARTY_TAKE` are the inverses.

---

## 4. Variables

### 4.1 Tokens

`+TOKENS` rows are `id, "description"` with **id 0..4095** (`FUN_004814d2` errors
`*** Token has ID out of range (0-4095) ***` and `*** Duplicate use of token ID %d ***`; record
size 0x88 = 136 bytes, name 128 chars at +4). A token is one bit in the hero's persistent state.
`TOKEN n, "text"` is also legal **inside +SCENES** (Evergreen's QuestScenes*.txt use it) — the
loader runs a second pass (FUN_00481654) that counts them and injects them.

### 4.2 Cookies (`#<name>`)

`SET name, value` writes, and `#<name>` reads. Storage is a **Windows INI section named
`[cookies]` in the hero's own `.ini` file** (`FUN_0047ab07` → `WritePrivateProfileStringA("cookies",
key, value, "<worlddir>\<charname>.ini")`, path built by `FUN_00460962`).

* Cookies are **strings**; numeric commands `atoi`/`atof` them.
* Substitution happens **before argument evaluation**, so cookies work inside quoted strings
  (`GIVE I#<itemID>`), and cookies must not be nested.
* Anti-tamper: cookies named `secure*` (or all, per `config.ini cookieProtection`) also get a hash
  written to the `[hashes]` section — `hash = fold(lowercase(cookie ^ value ^ hero ^ world))`
  (`FUN_0047a91d`/`FUN_0047a972`, seed `0x89AD23F1`, rotate `(i*3) % 24`). On read, a mismatch adds
  **13 cheat points** to the hero (`*(int*)(hero+0xEF8) += 0xD`). **An implementer can drop the hash
  machinery entirely in a solo port.**
* Special cookie names handled inline in the writer (not stored in the INI): `g.num` (sets
  `hero+0xAA0 = value & 3`), `item.id`, `spell.id`, `monster.id`, `num.hostClass<n>` (converts the
  hero to that class/level, re-deriving HP/MP/PP/level-up message), `num.trophy*`, plus whatever
  `FUN_00467f30` claims.

### 4.3 Stock cookies (`FUN_0047c6da`, 0x47c6da) — read-only

Resolved in this order; first match wins; anything unmatched falls through to the user-cookie INI.

`#<name>` prefix families:
* `num.hostClass num.hostLevel num.hostX num.hostY num.hostAge num.hostTotalAge
  num.hostBirthDay/Month/Year num.hostHP/MP/MaxHP/MaxMP num.hostSTR/WIS/STA/AGI/DEX
  num.hostAttack num.hostDefense num.hostBravery num.hostAggressiveness num.hostXP/MaxXP/TotalXP
  num.hostGP num.hostPP num.hostTotalPP num.hostTNL num.hostHaloPoints num.hostHandRatio
  num.hostMagicRatio num.hostEquipID<n> (15-char prefix) num.hostPets num.hostPetLevel
  num.hostPetID…` — the whole hero-stat block (hero offsets: HP +0x70, MP +0x78, maxHP +0x74,
  maxMP +0x7C, atk +0x84, def +0x80, STR..DEX +0x680..+0x690, GP +0x6C, PP +0x69C, class +0x60,
  level +0x64, magicRatio +0x694, handRatio +0x9E0).
* `num.rule maxPlayers maxAFK minVersion Ladder NoCheat NoMod NoMigrate NoBleep NoEavesdrop
  NoPets AllPK ArenaPK` — multiplayer server rules (no-op offline).
* `num.trophy Id Name StackSize StackCost Probability Token Flags BagOpen BagWidth BagHeight
  BagSlots BagSlotsInUse BagEmptySlots BagRoom<n> IdInSlot<n> CountInSlot<n> Trophy<n>`.
* `item.* spell.* monster.* scene.*` — detail blocks for the last give/attack/cast.
* `num.mapNum num.mapFlags num.mapName num.closestLink num.closestLinkStyle num.sceneMapFlags
  num.wosVersion num.isPKAttack num.isTactics num.peopleInScene num.peopleInParty
  num.countDown num.holidayID num.item<n> num.timerLength<n> num.timerLeft<n>`
* `str.worldName str.serverName str.mapName str.name str.soul str.uniform str.actorName<n>
  str.holidayName str.holidayGreeting str.devTableRowId str.devTableCol<n>`
* `num.time` / `num.serverTime<n>` (via `FUN_0047bd99`).
* `g.*` → gender.ini lookup; `g.num` → 0..3 (see §7).
* `num.devTableId`, `str.devTableRowId`, `str.devTableCol<n>` (FUN_00467f8b).

### 4.4 `%` substitutions (FUN_00484b3e scanner + FUN_004847cd resolver)

`%%` emits a literal `%`. Otherwise `%` + a code char, with an optional numeric argument:

`%0` current speaker's name · `%1` scene host's name · `%2` your own name · `%3` random insult ·
`%4` last person to chat · `%5` last person whose chat line started with your name ·
`%C` host's class name (`%Cn` = class n's name) · `%E<n>` equipped item id in slot n
(`%E0` = right hand) · `%I<n>` item name · `%K0` total kills, `%Kn` kills of monster n ·
`%L` host's level name, `%Ln` level n's name, `%L-1` host's level number · `%M<n>` monster name ·
`%R<n>` random 1..n · `%S<n>` spell name · `%T<n>` token description · `%Z<n>` trophy name.
Unknown code → a literal `"%%%c"` is emitted (`sprintf("%%%c", toupper(code))`).

In the decomp this happens inside the argument fetcher, so **`%` substitution applies to every
argument of every command**, not just to dialogue — the author's manual only documents it for
dialogue.

### 4.5 `COMPARE` / CONDITION CODE

`COMPARE A,B` stores `atoi(A) − atoi(B)` in `DAT_004FB008` and does *not* touch the PC.
`ADD/SUB/MUL/DIV/MOD`, `AND/OR/XOR/NOT` also set it. `IF= / IF> / IF< / IF<= / IF>= / IF<> / IF!=
/ IFEVEN / IFODD` test it. `F_COMPARE` + the `F_` math ops use doubles. Note `IF>`, `IF>=`, `IF<`,
`IF<=` are relative to **zero**, i.e. A vs B.

---

## 5. Maps, links, and how a scene is triggered

### 5.1 `+MAPS` row (quest.txt:224-256, data `maps.txt`)

`arg0 id (0-999, 0 = main world)`, `arg1 mapImageFile (.jpg)`,
`arg2 root` (root name ⇒ `maps/<root>.obl`, `.ter`, `.mon`, and `MIDI/<root>.mid`),
`arg3 "Map Name"`, `arg4 mapFlags` (decimal or `0x…`), `arg5 soundTheme 0-255`.

Map flag bits (quest.txt:262-288): 1 pet-v-pet · 2 pets vs players/pets · 4 no PKer protection ·
8 no PKing at all · 16 ladder reporting · 32 show all link names · 64 no shadows ·
128 no pets · 256 no eavesdropping · 512 guild hall · 1024 humans can't damage monsters ·
**2048 NO_REWARD** · **4096 NO_HEAL** · 8192 no tickets · 16384 no minimap · 32768 no stun ·
65536 no buff · 131072 no PK-rez · 262144 hide link boxes · 524288 no waypoints ·
1048576 no items in scene. (Evergreen uses 131072 on wilderness maps, 8 on towns, 22/9 on arenas.)

### 5.2 `.OBL` — the link/scene binding

Loader `FUN_00463989` (0x463989): `memset(dst, 0, n*800); fread(dst, 800, n, f)`. The in-game
array is `DAT_00604808`, 256 records of **800 bytes** (the editor writes/reads exactly `0x100`
records — `FUN_0046393f`). Fields (offsets derived from the Link Editor dialog code
`FUN_0045cce5`/`FUN_0045d1ed` and the runtime drop-in `FUN_00463853`):

| off | size | field |
|---|---|---|
| +0x000 | 4 | in use (0/1) |
| +0x004 | 4 | object image id (0-1023) into the world's `OBJECTS.JPG/OBR` filmstrip |
| +0x008 | 4 | x, **map-image pixels** |
| +0x00C | 4 | y, map-image pixels |
| +0x010 | 4 | **link type**: 0 nowhere, 1 → scene, 2 → link, 3 return to previous link, 4 → scene |
| +0x064 | 4 | destination: **scene number** (types 1/4) or **link number** (type 2) |
| +0x068 | 4 | monster difficulty level (sign = which direction gets harder) |
| +0x06C | 4 | sound theme override |
| +0x070 | 260 | extra label string |
| +0x0C0 | 260 | link display name (drawn when the name is revealed / flag 32 set) |
| +0x140 | 260 | MIDI file for this link |
| +0x194 | 4 | destination **map** number (type 2) |
| +0x1A0 | 4 | always-show-name flag |
| +0x1A4 | 4 | required map flag — link is hidden on the minimap unless the current map has it |

Link creation (`FUN_00462f84`, 0x462f84):
`x = viewOriginX_px + (screenX / 4)`, `y = viewOriginY_px + (screenY / 4)` — confirming link
coordinates are in map-image pixels while the on-screen view is 4× zoomed. (Hero positions, by
contrast, are **8.8 fixed-point** map pixels: `FUN_0041f247` computes
`((screenX - viewLeft) + scroll - 0x80) * 0x100`.)

### 5.3 Walking onto a link → entering a scene (`FUN_00463853`, 0x463853)

```
*(int*)(DAT_0067FBF8 + 0x67C) = linkIndex;      // remember where we dropped in
switch (link.type) {
  case 1: case 4:  PostMessage(hwnd, 0x475, DAT_004DD20C, link[+0x64]);  return;   // ENTER SCENE
  case 2:          SendMessage(hwnd, 0x46A, link[+0x64], link[+0x194]);          // CHANGE MAP
                   FUN_00456D2f(map[+0x10]); return;                              // + theme
  case 3:          return;                                                        // previous link
  default:         return;                                                        // nowhere
}
```

`0x475` is consumed by `FUN_0042052d` → `FUN_00491767(sceneNum, …)` → `FUN_0047a2a7(sceneNum, 0)`
(`FUN_00491767` line 107055) which is the same path used for the Well (scene 0) and camp (1).
`0x46A` is map change: `wParam` = link index, `lParam` = map number — matching `GOTO LINK`, which
sends `SendMessage(0x46A, wParam=linkArg3, lParam=mapArg2)`.

**So yes: a `.OBL` link of type 1/4 whose `+0x64` is N will run the `SCENE N` block from
quest.txt.** Evergreen's map 0 (`evergreen.jpg`, 18 maps total) has 256 link slots; scene 3
("Soul Brother", the heal/revive/quest-giver scene on the main map) is reached by dropping into the
gateway link.

### 5.4 `.MON` — monster placement

Loader `FUN_00464461` (0x464461): `fread(dst, 1, 0x43620, f)` — a **fixed 276 000-byte file**,
i.e. **1000 records of 276 bytes (0x45 ints)**; the loader zeroes `record[4]` on every entry and
counts non-zero `record[0]` as "placed monsters". `record[0]` = monster id (must be ≥ 1; index
into the monster table at 0x248 = 584 bytes/record), `record[1]` = x px, `record[2]` = y px,
`record[4]` = respawn/lifecycle state. The renderer (`FUN_004644ee`) multiplies coordinates by 4
for the 4× zoomed view, exactly like links.

### 5.5 `.TER` — terrain

`FUN_00486690` → `FUN_00486770` reads a version word (`FUN_004894c0`) and dispatches to a v1
reader (`FUN_00488080`) or a v2 reader (`FUN_00488510`); the payload is MFC-serialised via
`CFile::Open(..., 0xA0)` (type-binary). **The two payload layouts are not recovered here** — for a
solo port the terrain layer can start empty: `+TERRAINS` is the semantic table
(`id 0-9, "Name", damage, requiredToken`) and terrain 0 must be free to cross.

### 5.6 Other per-map files

`maps/<root>.jpg` (mandatory image, shown 4× stretched in the walk view and unstretched in the
minimap), optional `maps/<root>X4.jpg` (exactly 4× linear, high-res walk view),
`worlds/<w>/OBJECTS.JPG|OBJECTS.BMP` (link icons; BMP preferred, 256-colour RLE), `OBJECTS.OBR`
(rect→name map for the object editor). `scenes/<name>.jpg` = scene backgrounds (extension optional
in the script), `Monsters/<skin>.bmp` = actor/creature filmstrips (frame index = "pose"),
`art/<class>.bmp` = item filmstrips, `MIDI/`, `SFX/`, `HTML/`.

---

## 6. Table column formats

### 6.1 `spells.txt` (hdr: lines 10-266)
`arg0 id (1-767)`; `arg1 name`; `arg2 ppCost` (−1 unlearnable, 0 auto);
`arg3 element (0-255; 0 = heal, 4 = death, 8-255 chaos)`; `arg4 damage[.monsterId]` (0 auto,
−1 resurrect, −2..−24 diseases, −100−n cures, −200..−203 summons, −300 summon item);
`arg5 mpCost` (ignored, computed); `arg6 reqAffinity[+100 for all][.flags[.minLevel[.token[.trophy…]]]]`
(spell flags 1 = monsters can't use, 2 = not offered to humans, 4 = self-only);
`arg7 path` (9-digit `AAABBBCDD`, DD = cloud shape); `arg8 effects row (0-199)`;
`arg9 maxCols (1-31)`; `arg10 maxFx (0-100)`; `arg11 msPerCol`; `arg12 gravity[.effects[.weather]]`;
`arg13 loop`; `arg14 sfxSummon`; `arg15 sfxTravel`; `arg16 sfxStrike`. **Spell 0 is a special
"algorithm tweaks" row** (see monsters.txt).
Runtime record: 0x188 = 392 bytes (`FUN_0047fc6d`).

### 6.2 `items.txt` (hdr: lines 19-345)
`arg0 id` — **1-1023 = stackable (up to wallet limit), 1024-5119 = unique** (`FUN_00482fc1`
range-checks 0..0x13FF = 5119 and flags `id < 1024` as "multiple"); `arg1 "Name"`;
`arg2 class` (0 potion, 1 antidote, 4 travel/ticket, 10 helmet, 11 armor, 12-19 right-hands,
20 boots, 21 shield, 22 ring, 23 amulet, 100-105 ability, 200 pet, 201 HTML);
`arg3 image[.extension 1-9]`; `arg4 GP` (0 = unsellable/ungiveable);
`arg5 Level.EquipToken.flags.maxCount.trophyNeeded.trophyMade.trophyCountNeeded.trophyCountMade`;
`arg6 spellBinding.bootEffect`; `arg7 element`; `arg8 defense` (negative = cursed);
`arg9 attack`; `arg10 HP` on use; `arg11 MP` on use; `arg12 abilityPoints` (seeds) or disease id
(antidotes); `arg13 findProbability[.monsterId]`; `arg14 long description (≤250)`;
`arg15 sound (or URL for class 201)`; `arg16 attackPath.imageID.flags.weather.effect`
(paths: 1 lunge, 2 jump, 3 leap, 4 stab, 5 dbl-stab, 6 trample, 7 hop, 30 arrow, 31 stone, 32 lob,
33 swoop, −1 random at world load, 0 none).
Runtime record: **0x30C = 780 bytes**, 4096 entries = 0x3CF000 bytes; the whole table is
checksummed (`FUN_004835f9`) and mirrored (`DAT_00911B58`) for change detection.
Loader details worth copying: `arg5`'s dotted tail is parsed separately (`FUN_0048cab8`) and the
last two sub-fields are clamped to ≥ 2; `arg13` is clamped to 0x14; `arg7/arg8/arg9` clamp to 0xFF;
`arg15` present sets a flag, and if `arg15` is empty while `arg2` is a weapon class, a lookup
happens instead (`FUN_0048fe21`).

### 6.3 `monsters.txt` (hdr: lines 7-302)
`arg0 id (1-4095)`; `arg1 name (16)`; `arg2 skinName`; `arg3 scaleFactor[.monsterFlags[.colorTable[.xparent]]]`;
`arg4 element[.alignment]`; `arg5 hp`; `arg6 mp`; `arg7 defense`; `arg8 offense`; `arg9 expPts`;
`arg10 gold`; `arg11 level`; `arg12 strength`; `arg13 stamina` (0 = derive from level);
`arg14 agility` (0 = derive); `arg15 dexterity` (0 = derive); `arg16 wisdom`;
`arg17 growl.wav`; `arg18 pain.wav`; `arg19 attack path (−1 random at world load, 0 none)`;
`arg20` onward = the spell list (spells 1 and 5 are special algorithm tweaks — see spells.txt).
Runtime record: 0x248 = 584 bytes (`FUN_0048076b`).

### 6.4 `levels.txt` (hdr: lines 1-193)
Row key is **`class*100 + level`**: `100` = the class entry (0..88), `101..199` = levels 1..99
(level 100 reuses level 99's entry). Class range check: `class < 89`, `level ≤ 100`
(`FUN_00483984`, record 0x1ACD0 = 109 520 bytes per class, 0x951450 = 89 classes).
`arg0 key`; `arg1` unused; `arg2 ΔmaxHP`; `arg3 ΔmaxMP`; `arg4 "Level Name"` (pipe-separated
gender variants: `"Emperor|Emperor|Empress"`, gender order 0,1,2,3 from gender.ini);
for the class row only: `arg5 magicRatio (0-100)`, `arg6 rightHand (0-8, 0 = use start_hand_pp)`.
Experience thresholds are **computed automatically**; you only need rows where something changes.
Blank name ⇒ inherit previous. Sub-commands placed between the `nn00` and `nn01` rows:
`DESCRIPTION "…"` · `MAGIC_RATIO n` · `HAND_RATIO n` · `START_ABILITY str,wis,sta,agi,dex` ·
`MAX_ABILITY …` · `START_ELEMENT_PP 0..7` · `MAX_ELEMENT_PP …` · `START_HAND_PP 0..7` ·
`MAX_HAND_PP …` · `AUTO_MAX minHP,maxHP,minMP,maxMP,startMPLevel` (overrides ΔHP/ΔMP entirely) ·
`START_LOCATION map,link[,dropIn]` · `MAX_WALLET n` · `NO_GIFTS` · `START_ITEMS 1,2,…` (≤8) ·
`DEFAULT_SKIN g0,g1,g2,g3` (per gender) · `MAX_LEVEL`/`MIN_*` **[UNVERIFIED]**.
The class's preferred right hand is always pre-trained to level 5 regardless of `START_HAND_PP`.

### 6.5 `groups.txt` (hdr: lines 1-45)
`arg0 difficulty (0-4095)`; `arg1..9` monster ids (up to 9, duplicates allowed — a monster
listed k times may appear up to k times). Loader `FUN_00483686` (record 0x2C = 44 bytes, 4096
groups = 0x2C000). **Group 0 is special**: its values are flags, and the doc's example `0, 1`
means "monster groups are NOT random; all members appear". Selection: a link's `+0x68` difficulty
gives the group; positive difficulty ⇒ the list gets *easier* closer to the link (towns),
negative ⇒ *harder* (guarded areas). Duplicate ids warn `*** Duplicate use of Monster Group %d ***`.
Evergreen only fills 1-99 (and 0); everything else gets a random monster from the map's `.MON`
placements.

### 6.6 `trophies.txt` (hdr: lines 78-92)
`arg0 id (1-4095)`; `arg1 name (≤31)`; `arg2 imageFileName` (in `art/`, `.bmp` assumed);
`arg3 imageIndex`; `arg4 stackSize (0-99)`; `arg5 gpForStack`; `arg6 demonId` (dotted list of up
to 10, each a number or `a-b` range); `arg7 probability %`; `arg8 tokenNeeded`;
`arg9 flags` (1 cannot be GIVEN, 2 cannot be SOLD, 4 cannot be DISCARDED).
Runtime record: 0x564 = 1380 bytes (`FUN_004811b1`, `FUN_0046f60f`).
Trophy *bag* geometry is cookie-driven, not table-driven: `num.TrophyBagWidth/Height`,
`num.TrophyBagOpen`, `num.TrophyBagSlots…` — Evergreen scene 3 grows the player's bag from 3×4 to
4×4 to 6×5 through `SET num.TrophyBagWidth, 4` (quest.txt:1610-1630).

### 6.7 `monsters.txt` row for monster id 0 (SPELL ZERO / algorithm tweaks)
`spells.txt` says "Spell 0 is a very special entry which is actually a set of algorithm tweaks.
Read monsters.txt for more details" — the tweaks live in the monster-0 / spell-0 rows and were
not transcribed here. For a first-fight port the defaults are fine.

---

## 7. World INI files

All are optional, all are read with `GetPrivateProfileStringA` against
`"%s\\worlds\\%s\\<name>"` (`s__s_worlds__s_config_ini_004fb9ec` etc., FUN_00479a02 / 0x477a5c5).

* **`config.ini`** `[General]` (already fully uncommented in Evergreen): `goldName`,
  `pkHandPercent`, `pkMagicPercent`, `spellSuccessPercent`, `karmaPointsAreAlsoWarPoints`,
  `monsterXPAreAlsoWarPoints`, `noGivingGP`, **`cookieProtection` (0 none / 1 only `secure*` /
  2 all — Evergreen = 1)**, `pkTrophy`, `petsCanBitePeople`, `maxUnspentPP`,
  `maxPKAttackAdvantage`, `worldHomeUrl`, **`startingGP` (Evergreen = 500)**,
  `tacticsSourceUrl`, `tacticsWinGivesWarPoints`. Only `startingGP` (and `cookieProtection` if you
  keep the hash check) matter for solo.
* **`gender.ini`** — sections `[0]`..`[3]` = gender id; keys are arbitrary keywords mapped to
  words (`he`, `his`, `him`, `male`, `menu`, `adventurer`, `pain1.wav`). Scripts read
  `#<g.num>` (0-3) and `#<g.anything>`; read-only. Evergreen is identical for 0/1/3 and female for 2.
  Matching is case-insensitive.
* **`music.ini`** — one section per map **root name** (from `+MAPS` arg2), plus `[common]` as a
  fallback consulted first-then-common-then-quest-table. Keys: `fight`, `numMidi`, `midi1`..`midiN`
  (1-based), `victory`, `lost`, `levelup`. Blank `fight` ⇒ random stock fight song; blank `midiN`
  ⇒ fall back to `<root>.mid`. Files resolve in `worlds/<w>/MIDI/` then the root `MIDI/`.
  The in-game reader is `FUN_0047c519`.
* **`springy.ini`** — `[General] width, gravity, airfriction, stepSeconds, numObjects, elasticity`
  plus `[Object0]`, `[Object1]`, … mass-spring bodies for the `/springy` easter egg. Skip.
* **`slots.ini`** — Slobber Slots minigame: 8 symbols, per-symbol percent weights summing to 100,
  plus 3-of-a-kind and 2-of-a-kind payouts, on a 3×100 wheel. Skip for MVP.
* **`missions.ini`** — one section per job number: `Name`, `Qualify` (an `IF` condition string),
  `Desc`, `Trophies` (`"3x12,4x17"`), `RewardGold`, `RewardGive`, `RewardTake`, `RewardLevel`
  (`"3.2"` = class 3 level 2), `RewardPP`, `RewardWP`, `RewardToken` (negative removes),
  `AcceptGive/Take/Msg`, `AbandonGive/Take/Msg`, `FootNote`. No `#include` allowed. No blank lines
  inside a section.

---

## 8. Minimum path: create a hero → first map → win a fight

**8.1 Load.** Parse `quest.txt` (with `#include`) → tables → for map *m*: load
`maps/<root(m)>.jpg` (+ `X4`), `.obl` (256×800 B), `.ter`, `.mon` (1000×276 B), then
`config.ini` / `gender.ini` / `music.ini` / `missions.ini`.

**8.2 Create a hero.** Iterate `levels.txt` class rows (`100,200,…`): name = arg4,
start HP/MP from `AUTO_MAX` (else the class row's arg2/arg3 for level 1), starting abilities from
`START_ABILITY`, skins from `DEFAULT_SKIN` (pick by gender), level 1, class *n*, right-hand
training = level 5 in `arg6`'s hand, GP = `config.ini startingGP` (500), apply `START_ITEMS` and
`MAX_WALLET`/`NO_GIFTS`.

**8.3 Incarnate.** Default spawn is map 0 / link 0 / *above* the link; `START_LOCATION map,link,
dropIn` overrides (Evergreen uses `START_LOCATION 2, 13, 1` for one class only). The hero then
walks scene 0 (the Well of Souls) — Evergreen's scene 0 is pure tutorial dialog. To leave it,
`GOTO LINK 0,0` then `GOTO EXIT`, or the camp/well link.

**8.4 Walk the map.** Hero position = 8.8 fixed-point map pixels; the view is the map image at 4×,
centred on the hero, with a 0x80-pixel sprite offset. Movement is applied by the walk handler
`FUN_0041f247` (also plays `walk.wav` at a computed pan/volume from the offset). Clamp to the map
rect. Check the `.ter` layer for impassable terrain (skip if you don't implement it).

**8.5 Hit a link.** For each `.obl` record with `inUse != 0`, build the hit rectangle from the
object image's size centred on `(x,y)`; if the hero is inside, run `FUN_00463853`:
type 1/4 → scene `+0x64`; type 2 → switch to map `+0x194`, stand on link `+0x64`; type 3 → back.

**8.6 Scene 3 (the main-map hub, quest.txt:1584-1700)** is the scene a new solo hero will meet.
It uses only: `SCENE`, `THEME`, `ACTOR`, `POSE`, `SEL`, `MOVE`, `SET`, `COMPARE`, `IF=`, `IF>`,
`IF`, `GOTO`, `'`, `1:`, `ASK`, `IF -YES`, `GIVE` (T1, L1, H5000, M5000, I99), `IF I99`,
`OFFER2 0,10`, `IF V10`, `IF -#0`, `TAKE Z1.100`, `GIVE G1000`, `WAIT`, `SOUND`, `END`, `#<…>`,
`%1`, `%Z1s`, `%3`. Nothing exotic — a perfect smoke test for a C interpreter.

**8.7 Win a fight.** Monster encounter = hero walks into a `.MON` placement (or a link with a
non-zero `+0x68` difficulty selects a `groups.txt` group). The game switches to **scene 2**,
which Evergreen defines as `SCENE 2 / THEME / FIGHT / END` (quest.txt:1560-1566). `FIGHT` with no
args = standard map fight. Resolve rounds, apply `arg5 hp / arg7 defense / arg8 offense`,
award `arg9 expPts` + `arg10 gold` + `trophies.txt arg6/arg7` drops, unless map flag 2048
(NO_REWARD). Afterwards `IF WIN` / `IF LOSE` / `IF ALIVE` / `IF DEAD` work, then the scene
returns to the map above the link it dropped into.

---

## 9. Which commands the first Evergreen scenes actually use

Scenes 0-19 live inline in `quest.txt:1440-1700`; the rest are in `QuestScenes*.txt`.

| Scene | Commands used |
|---|---|
| **0** Well of Souls | `SCENE 0 temple, WELL, "Well of Souls", 2, 7` · `THEME 1` · `ACTOR 1, …, joshMiscellaneous, 32, 0, 60` · `SEL 1` · `POSE 32,32,32` · `MOVE 1, 50, 60` · `WAIT 1.0` · `'` speech ×8 · `MOVE` ×4 · `END` |
| **1** Camp | `SCENE 1` (bare!) · `THEME` (bare) · `COMPARE #<num.isPKAttack>, 1` · `IF= @pk` · `COMPARE #<num.mapNum>, 2` · `IF= @inCastle` · `END` · `FLAGS 45056` · `LOCK 1` · `N:` narration ×3 · `LOCK 0` · `SET sceneLock, 0` · `ACTOR 9, "Lock", danNoFloat, 6, 95, 10` · `POSE 6` / `POSE 5` · `WAIT 3.0` · label `@eventActorClick9` |
| **2** Fight | `SCENE 2` · `THEME` · `FIGHT` · `END` |
| **3** Main-map hub | see §8.6 |
| **4** Casino | `SCENE 4 aztec` · `THEME 0` · `MUSIC waltz2.mid` · `ACTOR` · `SEL` · `MOVE` · `GAME 1` · `'` ×2 · `END` |
| **5-8** Shops | `SCENE n bwEuroStreet` · `THEME 0` · `MUSIC` · `ACTOR` · `POSE p,p2` · `SEL` · `MOVE` · `OFFER2 0,99,10,0,99,11,0,99,20,0,99,21` · `'` · `END` |
| **9** Seed shop | as above + `OFFER 8,20,21,22,23,25` (item-id form) |
| **10** Throne room | `SCENE 10 greek2, SCENE, "…", 0, 0` · 3× `ACTOR` + `POSE` · `SEL` · `IF T10, @hasT10` · `GIVE T10` · `1:`,`2:`,`3:` multi-actor dialog · `WAIT 3` · `END` |
| **11** Wyrm Cavern | `SCENE 11 scene11, SCENE, "Wyrm Cavern"` · `IF T11, @hasT11` · `ACTOR` · `POSE 1,4,2` · `SEL` · `1:` · `MOVE 1, -50,50` · `WAIT 5.0` · `FIGHT 210` · `IF WIN @heWon` · `GIVE T11` · `END` ×2 |
| **12** Bootmaker | `OFFER2 0,99,20` · `IF/GIVE T12` · `TAKE`-free · `MOVE 2, 110, 25` (offscreen exit) |
| **13** Thief hideout | `FIGHT 111,112,111,113` · `IF WIN, @heWon` · `GIVE I702` / `GIVE T15` |
| **14,16,17,18** | `SCENE n sceneNN,FIGHT,"…"` · `THEME n` · `FIGHT id,id,…` · `END` |
| **15** CUT scene | `SCENE 15 scene15,CUT,"…"` (heroes hidden) |
| **19** Temple of Forgetfulness | `THEME 5` · `IF -T5-T6-T7, @S1` · `ASK 30.0` · `IF -YES, @S1` · `TAKE T5`…`TAKE T169` (~170 TAKEs) |
| **20-39** (QuestScenes20/30) | adds `ASK 30` + `IF YES` / `IF -YES` branching, `%3`, `%I193`, `GIVE I193` |
| **50** (QuestScenes50) | `OFFER2 0,99,0,20,40` (repeated triples) |
| **60** (QuestScenes60) | `MUSIC ""` (silence), `SCENE … 0, 9` (weather arg) |
| **110** (QuestScenes110) | multi-wave `FIGHT` + `IF DEAD, @dead` ×6 |
| **120** (QuestScenes120) | 20 actors, `MISSIONS 1,2,…,20` |
| **2000** (QuestScenes2000) | `MISSION 1,2,3,4` (singular alias) |
| **1800+** (QuestScenes18x) | `IF I186 @label` (no comma), `IF WON @label` |
| **200** (QuestScenes200) | `ACTOR n, "name", skin, 99, x, y, 30` — layer argument `, 30` and pose 99 |

**Commands present in the dictionary but never used anywhere in Evergreen** (so untested by the
shipped data): `FACE`, `COLOR`, `HTML`, `CALL`/`RETURN`, `PUSH`/`POP`, `SET_LEN`/`SET_SUBSTR`,
`SHUFFLE`, `STRCMP`/`STRSTR`, `AND`/`OR`/`XOR`/`NOT`, `F_*`/`F_COMPARE`, `PARTY`/`EJECT`/`LOCK`
(lock is used), `TIMER`, `COUNTDOWN`, `GET/SET_SERVER_VAR`, `PARTY_GIVE`/`PARTY_TAKE`,
`HOST_TAKE`, `BKGND`, `WEATHER`, `MENU`, `TOKEN` (used inside +SCENES by the QuestScenes files).

## 10. Gotchas that will bite a C reimplementation

1. **A `;` truncates the line** — you cannot put a semicolon inside a string literal.
2. **Whitespace is a token separator too**, so `IF T1, @x` yields 3 tokens, `IF T1,@x` yields 2,
   and a quoted string with a space yields one token. Never assume "one arg per comma".
3. **An unterminated quote eats the rest of the line** — including the trailing `;` comment.
4. **The label argument of `IF` is just "the last token"**; the comma is optional.
5. **Column prefix matching for sections** means `+SCENE` would match `SCENES` — always use the
   full name.
6. **Missing `SCENE` args inherit from the link** (bg, title, fx, weather) — a bare `SCENE 1`
   is legal and is exactly what Evergreen's camp and fight scenes do.
7. **`SELECT` is spelled `SEL`; `MODULUS` and `MOD`, `MISSIONS` and `MISSION`, `IF<>` and `IF!=`
   are all aliases** for one opcode.
8. **`%` substitution happens in every argument**, not just dialogue, so a literal `%` in any
   argument must be doubled.
9. **The interpreter runs one line per call** and several opcodes *return without advancing the
   PC* (WAIT/ASK/FIGHT/HTML/GAME) — a port must model that as a suspended state, not a loop.
10. `FIGHT` resolves the party/mercenaries by scanning the scene's 144 hero slots, so a
    single-player port can shortcut straight to building the monster list.


## Sources

- `extracted/worlds/Evergreen/quest.txt`: Master script: 2395 lines. CREDITS/STORY/TERRAINS/EQUIP/THEMES/MAPS/TOKENS/ELEMENTS/HANDS/ITEMS/TROPHIES/SPELLS/MONSTERS/GROUPS/LEVELS tables, then +SCENES with scenes 0-19 inline and the rest via #include. Header comments (lines 1-1345) are the author's authoritative command reference.
- `extracted/worlds/Evergreen/QuestScenes20.txt`: Scenes 20-29 (Quest 2). Uses SCENE/THEME/MUSIC/ACTOR/POSE/IF/GIVE/`N:` dialog.
- `extracted/worlds/Evergreen/QuestScenes30.txt`: Scenes 30-39. Uses ASK 30 + `IF -YES`/`IF YES` branching.
- `extracted/worlds/Evergreen/QuestScenes110.txt`: Multi-wave FIGHT scene 110: repeated `FIGHT` + `IF DEAD, @dead`.
- `extracted/worlds/Evergreen/QuestScenes120.txt`: Scene 120 uses `MISSIONS 1,2,3,...` and 20 actors.
- `extracted/worlds/Evergreen/QuestScenes2000.txt`: Uses the singular alias `MISSION 1,2,3,4` (same opcode as MISSIONS).
- `extracted/worlds/Evergreen/QuestScenes18x.txt`: Uses `IF I186 @label` with NO comma before the label, and `IF WON @wonFight`.
- `extracted/worlds/Evergreen/maps.txt`: 18 MAPS rows: `id, image.jpg, root, "Name", mapFlags`.
- `extracted/worlds/Evergreen/items.txt`: Item table, 17 documented args, arg0 = 1..5119.
- `extracted/worlds/Evergreen/spells.txt`: Spell table, 17 args + SPELL ZERO algorithm-tweak entry.
- `extracted/worlds/Evergreen/monsters.txt`: Monster table, 20 args.
- `extracted/worlds/Evergreen/levels.txt`: Class/level table. Class 0 rows (100,200,...) carry the class name + magic ratio + right hand; per-class sub-commands (DESCRIPTION, AUTO_MAX, START_ABILITY, START_LOCATION, DEFAULT_SKIN, MAX_WALLET, NO_GIFTS, START_ITEMS...).
- `extracted/worlds/Evergreen/groups.txt`: Difficulty-level -> monster-ID list table; group 0 = special flags.
- `extracted/worlds/Evergreen/trophies.txt`: Trophy table, 10 args.
- `extracted/worlds/Evergreen/config.ini`: [General] world overrides read by GetPrivateProfileString.
- `extracted/worlds/Evergreen/gender.ini`: [0]..[3] gender sections; keyword -> word map used by `#<g.keyword>`.
- `extracted/worlds/Evergreen/music.ini`: Per-map-root sections with fight/victory/lost/levelup/numMidi/midi1..N; [common] fallback.
- `extracted/worlds/Evergreen/springy.ini`: [General] width/gravity/airfriction/stepSeconds/numObjects/elasticity + [Object0..] mass-spring toy.
- `extracted/worlds/Evergreen/slots.ini`: Slobber Slots minigame odds/payout tables.
- `extracted/worlds/Evergreen/missions.ini`: One [jobNumber] section per mission; Name/Qualify/Desc/Trophies/Reward*/Accept*/Abandon*/FootNote.
- `extracted/worlds/Evergreen/maps/readme.txt`: Documents the .JPG/.OBL/.TER/.MON/X4.JPG/OBJECTS.JPG/OBJECTS.OBR per-map file set.
- `extracted/worlds/Evergreen/readme.txt`: World folder inventory.
- `work/decomp/all.c`: Full Ghidra decompilation; every claim in the report cites a VA from it.
