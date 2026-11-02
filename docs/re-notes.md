# Reverse-engineering notes (DivaMegaMix.exe v1.03)

Build date 2022-10-28, ImageBase `0x140000000`, no ASLR. Section names are scrambled by the
protector: real game code lives in the section named `.bss` (RVA `0x1000`-`0xB24A00`), and the huge
`.code` section is the protector's virtualized code. Some game functions are thin `jmp`s into it
and can't be read statically. `.pdata` (exception directory) is intact and gives function bounds.

Reference: [ReDIVA](https://github.com/korenkonder/ReDIVA) is a decompilation of Arcade Future Tone,
which MM+ is built on. Names below come from it.

## Song launch

| What | Address | Notes |
|---|---|---|
| `task_pv_game_open` | `0x1405DA0D0` | only xref to the `"PVGAME"` task name (`0x140CB9428`). `rcx` = args, `[rcx]` = pv_id |
| `task_pv_game_init_pv` | `0x1404414B0` | reads the current stage entry, builds args, calls `task_pv_game_open`. Hooked as `PvGameInitPv`. Both callers are inside virtualized code |
| stage data getter (`sub_14038BB30` in AFT) | `0x14040B2A0` | `lea rax, [0x1416E2B90]; ret` |
| stage data (`struc_717`) | `0x1416E2B90` | see layout below |
| stage count | `0x14040B440` | virtualized |

Stage data layout (from `task_pv_game_init_pv`):

- `+0x08` int32 current stage index (0..3, otherwise entry 2 is used)
- `+0x30` 4 entries of `0x6A8` bytes (`struc_716`; AFT stride was `0x668`)
- per entry: `+0x2C` pv_id, `+0x34` difficulty, `+0x38` edition, `+0x4C` clear % x100,
  `+0x50` life gauge safety time, `+0x54` life gauge border, `+0x69` ex_stage,
  `+0x78..` per-performer module ids (6 x int32), `+0xA8..` customize items
- after the launch block, `+0x140..` holds the round's result (ReDIVA `struc_662`: grade, hit counts, max combo,
  score...), exact MM+ layout being mapped with the round finder (below)

## Score data

Earlier versions read the score, combo, completion and result through addresses and a result struct inherited from
the original MultiDiva (which took them from RocketRacer's ShareDiva and from Braasileiro), along with four hook
signatures and its score-delta heuristic. All of that was removed in 2.1.2 and located again with our own tooling.

`game/round_finder.cpp` (research builds) logs, at the result screen, the current stage entry's result block and
every place in the exe's static data holding one of its values, with the memory around each. Four rounds (Oct 22):

- **Result** (stage entry `+0x140..`, ReDIVA `struc_662` from `field_0`): `+0x140` grade, `+0x144` total_hit_count[5],
  `+0x158` hit_count[5], `+0x16C` note count, `+0x170` the counts the result screen shows (cool, fine, safe, sad,
  worst), `+0x184` their percentages x100 (they sum to 10000), `+0x198` max_combo, `+0x19C` challenge time bonus,
  `+0x1A0` hold bonus, `+0x1A4` score_final, `+0x1A8` percentage x100, `+0x1AC` slide bonus. Checked against song
  select's records (AFTER BURNER 347850 / 94.50 %, ACUTE 374070 / 92.48 %).
- **Live play data**: the final score was also at `0x1412EEFD4`, `0x1412EF568` and `0x1412EF56C` in every round. Around
  `0x1412EF564` the values follow ReDIVA `pv_game_data` from `life_gauge` on: `+0x00` life gauge (0..255), `+0x04`
  score, `+0x08` score (second copy), `+0x14` current combo, `+0x28` max_combo, `+0x2C` total_hit_count[5], `+0x40`
  hit_count[5], `+0x54` reference score, `+0x58` note count, `+0xD0` completion % (float, matches the result's
  percentage).

## Result screen

`GameResultTask` (string `0x140C82268`, only xref in its constructor around `0x1401E7F35`): the constructor stores
the vtable (`0x140C821F0`) with `lea rax, [vtable]; mov [task_ptr], rbx; lea rdx, ["GameResultTask"]; mov [rbx], rax`.
Slots as ReDIVA's `app::Task`: 0 dtor, 1 init (`0x1401E80B0`), 2 ctrl, 3 dest, 4/5 disp/basic. The bodies are
virtualized, so the mod swaps vtable slot 1 to learn when the result screen opens.

## Tooling

`tools/re/` (capstone + pefile, see requirements.txt): `dre.py` helpers, `mksig.py <va> <len>` signature generator, `sigcheck.py` verifies sigs.cpp against the exe. Used for string/xref search and signature generation.

## Launch block (multiplayer song pick)

The stage entry range `[+0x2C, +0x140)` (ReDIVA `struc_775`) holds everything a launch needs:
pv id, difficulty, edition, clear/life settings, module ids (`+0x78`, 6 x int32, a second copy at `+0x90`),
items (`+0xA8`, 6 x 5 x int32) and item masks (`+0x120`, 6 x 5 bytes). Result data starts at `+0x140`.

Copying the host's block into a client's current entry inside the `PvGameInitPv` hook launches the
host's song with the host's performers and outfits, regardless of what the client selected
(verified in-game: Freely Tomorrow selected -> Roshin Yukai with Rin, module 40).
Overwriting only pv/difficulty keeps the selected song's performers (wrong characters).

Dead end: `0x1401D5F40` (called by `task_pv_game_open` with the pv id) is virtualized and does not
behave as a plain `get_pv`: it returned the same static table at `0x1412B5A80` for every pv id.
Per-player outfits (each player's own module choice) would need the pv_db / module table, deferred.

## Modes and auto-start

RTTI is intact (`.?AVPVsel@@`, `.?AVModeGamePS4_PvselImpl@@`, ...), so `dre.vtables_for()` finds class vtables.

| What | Address | Notes |
|---|---|---|
| mode state getter | `0x1402C4810` | `lea rax, [0x14CC61078]` |
| `shift_next_mode(int)` | `0x1402C4BB0` | sets state `+0x04`, `+0x10 = 1` |
| `shift_next_mode_sub(int)` | `0x1402C4BE0` | sets state `+0x20`, `+0x2C = 1` |
| `ModeGamePS4_PvselImpl` vtable | `0x140C837D0` | entry 3 `0x1401ECD10` = song select ctrl; on confirm: `shift_next_mode_sub(7)` then `0x140653C30(_, 0, stage[+0x19] == 0)` |

Mode state as int32 array: `[0]` current mode, `[1]` next mode, `[2]` previous mode, `[5]` frames in sub mode, `[7]` current sub,
`[8]` next sub, `[9]` previous sub. Observed ids: main menu = mode 9 / sub 42, rhythm game = mode 2,
song select = sub 5, gameplay = sub 7. The gameplay sub mode's init calls `task_pv_game_init_pv` ~270 ms later.

Auto-start (`launch::StartNow`): from song select, arm the host's block, call `shift_next_mode_sub(7)` and
`0x140653C30` like song select does. Verified in-game: clean transition, preview music stops, correct song and outfits. Only difference: no "start" jingle (played elsewhere, likely in virtualized code).

Dead end (host pick without playing): swallowing song select's `shift_next_mode_sub(7)` leaves the game on an
endless NOW LOADING screen (song select has already closed itself), and at that point the stage entry has
pv/difficulty but **no modules yet** (they are filled between the request and `task_pv_game_init_pv`).
So the host's own Start is the round start: the complete block is captured in the `PvGameInitPv` hook.

## Synchronized start (loading barrier)

| What | Address | Notes |
|---|---|---|
| `TaskPvGame` vtable | `0x140CB9438` | RTTI `.?AVTaskPvGame@?A0x62ecae2b@@`; ctrl `0x1405DA060` -> `pv_game_parent_ctrl` `0x140266420` |
| `pv_game_parent_data` | `0x1412F0248` | `playing` at `+3`, `update_func` at `+0x18` |
| `pv_game_parent::ctrl` (update_func) | `0x140265E60` | `if (pv_game_ptr) goto 0x140268100`; the steps behind it are virtualized |
| `pv_game_ptr` | `0x14CC105A0` | outer state `+0x10` (0 loading, 1 playing, 2 ending), inner loading step `+0x14` |

Loading steps observed: 0 -> 1 -> 2 (asset loading, 2-6 s) -> 3 -> 4 -> 5 -> 6 -> outer 1, steps 3-6 within
~0.25 s. Step 3 matches AFT's "end wait screen, start song clock". Skipping `pv_game_parent::ctrl` while at
step 3 keeps NOW LOADING up with everything loaded; on release the song starts immediately and stays in
sync (verified in-game, also through the multiplayer barrier with a delayed bot).

## Pause

Dead end: `GameEventFunctionPause<GameSwitchControl>` (vtable `0x140CBE318`) and `...Unpause...` (`0x140CBE358`) are
listener objects `{vtable, control, handler}` registered by `0x14061E3C0`; the dispatcher calls the handler pointer at
`+0x10` (`0x14061E090` sets `control+0x70 = 1`), not vtable slot 2. They only react to a pause.

The pause itself starts in `pv_game::ctrl` (`0x140241DF0`): if `pv_game+1` (paused) is 0 it asks the pause manager,
`TaskPauseManagerSwitch` (static object `0x14CC0CA90`, interface at `+0x68` = `0x14CC0CAF8`, vtable `0x140CC2548`),
slot 3 `0x1401F0C30` = `task alive && dword [iface+8] != 0` (pending request). True (or the force flag
`pv_game+0x2D2B5`, probably focus loss) runs the pause block (`0x1402424B3`, fires the pause event at `0x1402425A8`).
Second dead end: refusing that query only stops the song; the pause menu still opens. The menu is the task
itself: `TaskPauseManagerSwitch` ctrl `0x14065BE90` is a state machine on `task+0x70` (0 = closed, i.e. the
"pending" dword above). In state 0 it checks the pause button with `button_tapped(input, 0xA0)` (`0x1402AB260`,
shared by the whole game; this call returns to `0x14065BF8C`) and opens the menu, which then makes the slot-3
query true. The pause guard Detours `0x1402AB260` and answers false only for that return address; paused =
`dword [0x14CC0CB00] != 0`. `*pv_game_ptr` is not the object `pv_game::ctrl` runs on (its `+1` reads 1 in play).

## Outfits (per-player modules)

Song select does not leave the modules for later: confirming a song builds a launch-settings record and writes
it into the stage entry (verified with a write watch on entry +0x78: song select wrote module 266, Rin's yukata).
- `0x14040B600(stage_data, settings*, bool)`: copies `settings` into stage entry [index]: settings +0x00 pv,
  +0x08 difficulty, +0x0C edition, +0x30..+0x44 modules[6] -> entry +0x78, +0x48..+0x5C -> entry +0x90 copy.
  Callers: 0x1401EB84D, 0x1401EBC69, 0x140206BF8, 0x140570459, 0x1406F2EF1, two in virtualized code.
- `0x1406F2AE0(pvsel)`: song select's "start this song" builder (pv `pvsel+0x262a4`, difficulty `+0x262c8`,
  edition `+0x262cc`). Per performer, the module comes from the global `0x141750890 + 0x1C0 + slot*4`
  (`0x141750A50`, the highlighted song's selection), resolved by `0x1404427B0(ctx, module, ...)` (negative
  values = special choices such as recommended, jump table).
- Experiment that failed: writing only pv/difficulty/edition into the stage entry before auto-start leaves the
  modules 0 (default Miku); nothing fills them later.
- `0x1401EAD50(record, pv, source = null)`: loads the saved choices for `pv` into the record (song select
  calls it every frame for the highlighted song; per slot `0x1401D5EC0(save data 0x14114C664 from 0x1401D6510(), pv, slot)` then the
  virtualized `0x1401D94F0`). Resolver context: `0x140441B60()`; resolver third argument: record
  `+0x338 + 0x10 + slot*0x14`.
- Per-player outfits (verified in-game, two songs): `launch::OwnModules(pv)` = LoadChoices + Resolve, used by
  `StartNow(block, ownOutfits)` for clients (setting `own_outfits`, default on).

## Songs (installed check, song select highlight)

- Installed check: `0x140581F50(0x140580490(), pv)` (pv_db lookup, virtualized but callable) returns a pointer
  to the entry pointer, null when the song isn't installed; `0x1404BCA70(entry, difficulty, edition)` returns
  the chart or null (edition 2 = either). Song select's start builder uses the same calls.
- Highlighted song: `0x1406DF6F0(list)` runs every frame on song select, reads the pv as int16 at
  `list+0xA88` and tail-jumps to the outfit loader `0x1401EAD50`. **Patching it (Detours) crashes the game
  when song select opens** (fault in an unknown module, consistent with the protector checking that code), so
  `songs.cpp` uses a hardware execute breakpoint (DR1) and a vectored handler instead (reads rcx, sets RF).
- pv_db entry (0x3E0 bytes each, contiguous): `+0x08` std::string song name as song select shows it, `+0x28`
  a second copy (both English with the game in English), `+0x50` song file, `+0x1A0` the pv id as text.
  `songs::Title(pv)` reads `+0x08`.

### Alt-tab / system pause

Hiding WM_ACTIVATE / WM_ACTIVATEAPP / WM_KILLFOCUS from the game does not stop it (dead end). The song's
per-frame code `0x140268460` asks `0x1406083B0()` (a global flag, focus lost) and others (`0x1405FEF60`,
`0x1402B8110`, ...) and then calls the pause manager interface's slot 7 (`[vtable+0x38]`, `0x14065C780`,
"request pause"), which sets the open flag `iface+0x4C9` (= task `+0x531`). The menu's state 0 then opens it
(`0x14065BF51`, found with a write watch on task `+0x70`). The pause guard swaps slot 7 in the vtable
`0x140CC2548` and drops the requests during a round.
Verified in-game: with the slot-7 guard the song keeps running while alt-tabbed (borderless), but the game
is silent in the background. The focus flag (`0x1406083B0` = window object `0x14CC014C0` state `+0x68` in
{4,5}) is only read by the two pause requests (`0x140268460`, `0x14061FF10`) and the input system
(`0x1402AA419` -> `0x1402AC250`, input ignored while unfocused): the mute is elsewhere (audio engine?). Open.

## UI assets (research for the official-looking room screen)

- Menu art lives in `diva_main.cpk` (23 GB, CRI CPK v7, files 2048-aligned from TocOffset 0x800, not
  CRILAYLA-compressed) under `rom_steam/rom/2d/`: `spr_nswgam_{menu,cmnbg,cmn,songselector,result,...}.farc`
  (sprites) and `aet_nswgam_*.bin` (AET layouts), per-language copies in `rom_steam_cn/...` etc. Fonts:
  `spr_fnt_36.farc`, `spr_fnt_bold36.farc`.
- Every CPK entry is AES-256-CBC encrypted (key/IV in MikuMikuLibrary `CpkArchive.cs`); inside, the sprite
  archives are FT-style encrypted FARCs (`FARC` signature, AES-128-CBC + gzip per entry, MikuMikuLibrary
  `FarcArchive.cs`), then sprite sets with TXP textures. A throwaway CPK reader is in the session scratchpad.
- Community state (GameBanana, Oct 2026): UI mods only recolor or rearrange existing screens (After Effects +
  Samyuu's AetPlugin); none adds a new screen. Plan: draw our own screens in the overlay, styled after the game,
  later with the game's own sprites taken from its loaded sprite sets at runtime (no files, no keys shipped).
- Runtime capture (Ctrl+F8 "Capture this screen's sprites", ui/gpu_capture.cpp): menu sprites are drawn as indexed
  quads, vertex = x y z (screen px), packed RGBA, u v; one texture (slot 0) per draw. Sprite sheets are 2048^2
  BC5 with 2 mips = YCbCr: mip 0 R = luma, G = alpha; mip 1 (half size) R = Cb, G = Cr, each `*1.003922 - 0.503929`;
  R = Y + 1.5748 Cr, G = Y - 0.1873 Cb - 0.4681 Cr, B = Y + 1.8556 Cb (MikuMikuLibrary TextureDecoderCore).
  Rows are stored bottom-up. The text font sheet is a 4096^2 BC3 (spr_fnt_*), one quad per glyph.
- Sprite set .bin (little endian, MikuMikuLibrary SpriteSet.cs): u32 signature, texturesOffset, textureCount,
  spriteCount, spritesOffset, textureNamesOffset, spriteNamesOffset, spriteModesOffset; sprite = u32 texture,
  u32 0, uv begin, uv end, x, y, width, height (40 bytes). spr_nswgam_menu.bin: 8 textures, 83 sprites, e.g.
  SELECTOR_RHYTHMGAME (plate), _EFF (glow), _TXT (label), BTN_TXT_YES_SEL, POPUP_BG01, FOOTER_*, KEYICON_*.
- Loaded sprite sets at runtime: `std::map<uint32 set id, SPRSET>` at `0x14CBFF3A0` ({head, size}; 3933
  entries = every set in spr_db, loaded or not), filled by `0x1405B8C20`. Node: left, parent, right, color,
  isnil `+0x19`, key `+0x20`, `SPRSET` (RTTI `.?AVSPRSET@@`, 0x70 bytes) `+0x28`. SPRSET: `+0x10` the loaded
  file, `+0x18` its header relocated to 64-bit pointers (u32 sig, u32 texturesOffset, u32 textures, u32
  sprites, sprite infos*, texture names*, sprite names*, modes*; TXP data is gone after upload), `+0x20`
  texture object* array, texture object `+0x28` -> D3D holder with the texture and its SRV at `+0x10`/`+0x18`.
  Set ids seen loaded on the main menu: 2976 menu (84 sprites, 9 textures), 2970 common (279), 2981 (344,
  CLEAR_ICON_*), 1432/1978 (BG_GREEN...), 3922 (258), font sets 1903-1914. ui/game_sprites.cpp uses all this.
- The map key is the set's *index* in spr_db (rom_steam_<lang>/rom/2d/spr_db.bin: u32 setCount, setsOffset,
  spriteCount, spritesOffset; set = u32 id, nameOffset, fileNameOffset, index), not its id: 2970 = SPR_NSWGAM_CMN,
  2971 CMNBG, 2976 MENU, 2977 MENUBG, 2980 RESULT, 2981 SONGSELECTOR; fonts 1903 SPR_FNT_36, 1907 SPR_FNT_36LATIN9,
  1909 SPR_FNT_BOLD36, 1913 SPR_FNT_BOLD36LATIN9, 1914 SPR_FNT_CMN (old digit strips NUM12X16...NUM56X46).
  The user's game runs in English (rom_steam_en sets); only the keyboard is French (key chips say ENTREE/ECHAP).

## Font (research for our text in the game's font)

- fontmap: rom_steam/rom/fontmap.farc (FARC, plain gzip) -> fontmap.bin "FMH3" (ReDIVA src/CRE/font.cpp):
  u32 sig, pad, fontCount (22), fontOffsetsOffset; font = i32 mapId, u8 advance, lineHeight, boxW, boxH, flags,
  columnScaleNum, Denom, pad, u32 ?, charsPerRow, charCount, charsOffset; char = u16 code, u8 halfwidth, pad,
  column, row, glyphOffset, glyphWidth. Glyph rect on the sheet: (column*boxW + glyphOffset, row*boxH,
  glyphWidth, lineHeight); halfwidth glyphs advance boxW/2-ish, proportional with flags & 2.
- 36 px sheets: one sprite DIVA_36_38 (4096^2 BC3, white + alpha, bottom-up), 38 px cells, 107 per row,
  7145 characters (ASCII, Latin, Greek, Cyrillic, symbols, kana, kanji). Width table pairing (tightest fit of the
  ink against each map's boxes, research script): LATIN9 thin -> map 20, BOLD LATIN9 -> map 21, JP thin -> 15,
  JP bold -> 17; maps 16/18/19 are monospace (CN/KR/full sets). The 12-13 character maps are digit fonts.
- Song select titles = the BOLD sheet at ~1.19x (glyphs ~43 px tall at 1080p) with ~+4.5 px between letters
  (IoU fit against the screenshot: bold 0.69, thin 0.59).
- Bold italic words ("Help", "Sort", "OK", "Rhythm Game"...) are pre-drawn sprites per language (song select
  KEYTXT_N_01..13 on a 3-piece chip KEYBS_N_L/C/R; the key name is drawn on the chip at runtime). The exe imports
  DirectWrite (CDirectWriteFont) but no font family name is in plain sight. Song select digits: per-digit
  sprites SCORE_NUM24X28_00..09, SONG_NUM40X34_*, RANK_NUM40X48_*; results: SCO_NUM*, FS_NUM*, VP_NUM58X58 strips.
- Runtime glyph tables (found with Ctrl+F8 "Find font map in memory", verified): static 0x40-byte font records (ReDIVA
  `font`): +0 i32 sprite id, +4 u8 glyph w, h, box w, h, +8 u8 column scale num, denom, +0xC i32 fontmap id,
  +0x10 f32 column scale (0.5), +0x18 u64 characters per row (107), +0x20 std::vector<6-byte char> begin/end/cap
  (65534 entries, indexed by UTF-16 code: init, halfwidth, column, row, offset, width), +0x38 bool spacing.
  0x14CC60FF0 = map 20, sprite 0xEECE SPR_FNT_36LATIN9_DIVA_36_38; 0x14CC61030 = map 21, sprite 0xEED0
  SPR_FNT_BOLD36LATIN9_DIVA_36_38 (spr_db). ui/game_sprites.cpp DrawText/TextWidth use them.

## Song jackets (sprite set requests)

Found statically (the real code is in the file's `.bss`; xrefs to the string `./rom/2d/` at 0x140C8CF88 and to
`SPR_SEL_PV%03d` at 0x140CB5FB8), layout as in ReDIVA's `SPRSET` (src/CRE/sprite.cpp), shifted by one field:
- `SPRSET::request(this, const char* file, const {char* b, e}* mdata dir)` 0x1405B6BA0: reference `+0x28` (>0: ++ and
  return), file name std::string `+0x50`, then an async read of `<dir or ./rom/2d/><file .farc>`; ready `+0x38` = 0.
  File handler `+0x30`. An empty {begin == end} dir means the default (mods are redirected by DivaModLoader).
- `SprMgr::request(_, idx, file, dir)` 0x1405B8F10 (find by map index 0x1405B8EC0, the map at 0x14CBFF3A0).
- Public, by spr_db **set id**: `spr::request(id, dir*)` 0x1405B4770, `spr::wait(id)` 0x1405B4810 (call every frame
  until ready: parses the file, makes the textures, sets ready), `spr::free(id)` 0x1405B48B0 (reference--, unload at 0).
  `SPRSET::wait` 0x1405B7180, `SPRSET::free` 0x1405B7230.
- spr_db: set name -> `const uint32_t* id` 0x1405BC770(_, const {begin, end}*); id -> entry 0x1405BC600(_, id),
  entry `+0x18` file name, `+0x28` map index; sprite name -> id 0x1405BC8F0 (-1 if none). The first argument is
  ignored by all three (global db under a lock).
- Song select (0x140581180) builds `SPR_SEL_PV%03d` (set), `SPR_SEL_PV%03d_SONG_JK%03d[_EX]`, `_SONG_LOGO%03d[_EX]`,
  `_SONG_BG%03d[_EX]`, thumbnails `SPR_SEL_PVTMB_%03d[_EX]`; defaults 0xD50 / 0xD81 = the NOW PRINTING dummies.
  A song set holds SONG_BG<pv> (1280x720), SONG_JK<pv> (502x502), SONG_LOGO<pv> (860x300: rainbow title + credits).
- ui/game_sprites.cpp `Jacket(pv)` uses all this (byte-checked before calling; Ctrl+F8 sprite test draws song 1's).

## Start sound

`play_se(name, volume)` `0x1405AA540`: found by searching the exe for `se_ft_music_selector_enter_01` (`0x140C85AE0`);
song select loads it with `lea rcx, [name]` and calls `0x1405AA540` (`0x14020A95C`, `0x1406F8C72`). Its first
instructions (`mov rdx, rcx; movaps xmm2, xmm1; mov ecx, 1`) are the bytes `launch.cpp` checks before calling it.

## Provenance audit (2.1.2)

Every address, signature and struct in the mod was found with this project's own tools, from ReDIVA's names and
strings in the exe; each section above says how. Before 2.1.2 the code was compared against the public source of
the original MultiDiva (client and server, every version), ShareDiva, PDRPC, DivaDllMods (both forks), New Classics,
ExPatch, MMPlusMods, DivaModLoader and about 30 other open Mega Mix+ plugins: no shared code, signatures or structs
apart from standard API calls (Direct3D, ImGui backends, Windows) and DivaModLoader's credited `helpers.h`. A few
game addresses also appear in other mods (the stage data getter, the sprite set loader, `play_se`, the pause button
check, the song select start builder): they are the same game functions, found here independently as described.

## ReDIVA layouts to confirm on Mega Mix+ (2.3.0 research)

ReDIVA (github.com/korenkonder/ReDIVA) is the decompilation of the Arcade game, so these are leads, not facts. The
song entry's first fields already match this document (name `+0x08`, second string `+0x28`, file name `+0x50`).
- `pv_db_pv` (`src/CRE/pv_db.hpp`): `id` +0x00, `date` +0x04, `song_name` +0x08, `song_name_reading` +0x28,
  `remix_parent` +0x48, `bpm` +0x4C, `song_file_name` +0x50, ..., `difficulty[5]` (vectors of `pv_db_pv_difficulty`).
  `songs::Bpm` reads +0x4C and accepts 30..400.
- `pv_db_pv_difficulty`: `difficulty`, `edition`, `attribute` (type + bitset<128>), `script_file_name` (std::string),
  `level` (`pv_level`, half stars: 0x0F = 7.5, 0x14 = 10.0), `level_sort_index`, ..., `songinfo` (music, lyrics, arranger...
  for artist credits). `songs::Level` reads +0x40 of the chart and accepts 2..20.
- `InputState` (`src/ReDIVA/input_state.hpp`): bitsets Tapped, Released, Down, DownPrev, DoubleTapped, Hold... of 111
  buttons, 16 bytes each from +0x00 (Down estimated at +0x20). The `button_tapped` hook sees the object as its first argument.
- `PlayerData` (`player_data.hpp`): `player_name`, `level`, `level_name`, module equips, via `player_data_array_get`.
- `pv_game_data`: `life_gauge`, `life_gauge_border`, `hit_count[5]`, `chance_point`, `no_fail`, `no_clear`, ...
Debug panel (Ctrl+F8, research=1) buttons log what is needed to confirm each one.

Song select input (2.3.1 log, button log on): `button_tapped` button 5 is checked from `0x1406F030F` and button 6 from
`0x1406F02F1`, in the same function next to the highlight thunk (ReDIVA `InputButton` order: UP 3, DOWN 4, LEFT 5,
RIGHT 6), so they are the left/right handlers that change the selection. Chart struct: `+0x00` difficulty, `+0x04`
edition, `+0x40` level (half stars), `+0x44` level sort index.

Song select difficulty (2.3.3 / 2.3.6 logs): LEFT/RIGHT change the difficulty (button 5 from `0x1406F030F`, button 6 from
`0x1406F02F1`), UP/DOWN the song, confirm is button 10 from `0x140669B42`; a started round's `PvGameInit` showed
diff 0,1,2,3,3+EX after the matching number of right taps. The selected difficulty is NOT in the song list object (the
highlight hook's rcx: `+0xA88` is the highlighted pv, `+0xADC` a `p_02`..`p_04` tag that is not the difficulty) nor in
the first 0x600 bytes of the selection record at `0x141750890`: taps changed nothing in either. The song select
object is recreated every time song select opens. Next step: a write watch on the stage entry's difficulty field,
logging registers and the code before the write, to find where the confirm handler reads it from.

Confirm-time difficulty copy (2.3.7 write watch on the stage entry's difficulty, `0x1416E2BF4`): the writer is
`mov [r14+8], eax` ending at `0x14040B6C5`, after `mov eax,[r15+8]`, with `r14 = entry+0x2C` (the PvId field): a
function copies three ints (pv, +0x30, difficulty) from a struct at `r15` (a frame-local struct, `0x14D7C0` every time;
`rbp = 0x14D850`) into the entry; edition follows. Call chain: `14040B66B` <- `1406F2EF6` (the start builder, notes say
`0x1406F2AE0`) <- `1406F2A8C` <- `1406EE589` <- main loop. Just before, the same function zeroes the entry (`rax =
0x141750890`, the outfit selection record). `[r12]` with `r12 = 0x1416E2B90` (stage data base) was 0,1,2,3,3 for the
five rounds of that log, used as `rcx = [r12+8] + 4*[r12]` to index tables at `0x1416E2AD0/B10/B50`: it equals the
difficulty, but the rounds were played in rising order, so it may also be a round counter capped at 3. 2.3.8 adds a
"diffhunt" that intersects candidate fields over rounds started in any order.
Result (2.3.8 log, 10 rounds on pv 50 and 402, Easy..Extra each): the only non-trivial surviving field is stage data
`+0x000` (`0x1416E2B90`), equal to the difficulty at every confirm, and it went back to 0 when the song changed, so it is
not a round counter. Whether it follows left/right live in song select (so it can be read while browsing) is what the
2.3.9 song select watch checks ("stage +0x..." lines). Edition (Extra chart) is a separate field, not found yet.

Selected chart in song select (2.3.11 log, confirmed): the first two ints of the stage data (`0x1416E2B90`) follow
left/right live: `+0x0` = difficulty 0..3 (Easy, Normal, Hard, Extreme), `+0x4` = 1 for the Extra chart (then `+0x0` stays
3). Right on the last chart wraps to Easy and left on Easy wraps to Extra. `songs::SelectedChart` reads them and the host
sends them with `hover` (2.4.0).

Song select objects (Cheat Engine session, confirmed): the highlighted pv is written by `1406DEA22: mov [rcx+0xA88], edx`
with `rcx` = the song list object and `rdi` = the song select object (`pvsel`); the list sits at `pvsel+0x3380`. The 4 byte
field at `pvsel+0x262A4` (the song to start) follows the highlight live and holds `-2` (0xFFFFFFFE) while Random is chosen;
`pvsel+0x262C8/0x262CC` do NOT follow left/right while browsing (the live difficulty is in the stage data). The list's
`+0xA88` shows the real song id in random mode too.

Highlight setter (2.4.5 watch log, disassembled with capstone): `0x1406DE9F0(list, pv)`: `push rbx; sub rsp,60h`, calls
`0x1402B8080` (returns al: if non-zero it returns without doing anything), returns if `byte [list+0xA91]` is set, then
`mov [list+0xA88], edx` (the write), looks the song up (`0x140580490` db, `0x140581F50` entry) and stores `0x1404BCCC0(entry)` at
`list+0x600`, then calls `0x1406DF720(list)` and `0x1406DF970(&local, list, [list+0x600])`. Two kinds of callers per
cursor step: `1401EAE3E` (called from `1406F4370` <- `1406EFE48` <- `1406F79D0` <- `1401DEFF9`) and `1406E1157` (from
`14032F500` <- `1409777EF` <- `140165A2D` <- `1406E2F41` <- `1406E1ADD`); Random landings come only through `1406E1157`
(from `1401DA834` <- `1406F0262` <- `1401DEFF9`), one write per second while it spins.

Setting the highlight directly does not work (2.4.6 log): calling `0x1406DE9F0(list, pv)` from the overlay changed
`+0xA88` but the game wrote the cursor's song back within a second, and confirming still started the cursor's song. The
list is driven by a pointer to the current item (`mov rdx,[[rsi]]; mov edx,[rdx]` = the item's first int is the song id,
read just before the setter call at `1406E1152`), not by `+0xA88`. The later return addresses in the stack-scan logs were
leftovers: use the unwound frames of 2.4.7.

Song select object fields (2.4.9 object watch, `pvsel` = list - 0x3380, 0x26400 bytes watched): sort mode is
`+0x17DFC` (T cycles Star 6 > Singer 1 > Score 2 > Name 0 > Star), with a 1,2,3,0 step counter at `+0x17E0C` and `+0x262C0`;
the selected difficulty is `+0x262C8` (3 -> 2 -> 3 for left, right), so the earlier "does not follow" CE result came from
the wrong base. The character/star tab strip depends on the sort (Star: 6, 6.5 ... 10, ALL, heart; Singer: Miku, Rin,
Len, Luka, KAITO, MEIKO, ALL, heart): ALL is always the second to last tab. The tab index itself is not pinned down yet
(fields near `+0x3978..+0x3990` change with the E/Q presses but not as a plain count).

Confirmed in game (2.3.1 log): pv_db chart level at chart `+0x40` in half stars (pv 96: raw 6/8/10/14 = 3.0/4.0/5.0/7.0,
EX chart raw 16 = 8.0) and song entry BPM at entry `+0x4C` (pv 96 = 183).

## Song select: what is known (status for "follow the host's song", 2.4.10)

Objects (addresses are per song select session; offsets are stable):
- **Song select object `pvsel`**: its first 8 bytes are the mode vtable `0x140C837D0` (unverified). The song list object is a
  member at `pvsel+0x3380`. `rdi` held `pvsel` and `rcx` the list when the highlight was written.
- **Highlighted song**: int at `list+0xA88` (written by `0x1406DE9F0(list, pv)`, see "Highlight setter"). Derived from the
  cursor's current item (`[item]` = pv). **Writing it or calling the setter does not move the cursor** (2.4.6): the game
  writes the cursor's song back and confirming starts the cursor's song.
- **Song to start copy** `pvsel+0x262A4` follows the highlight live and is `-2` (0xFFFFFFFE) while Random is chosen.
- **Selected difficulty**: `pvsel+0x262C8` and the stage data header `0x1416E2B90 +0x0` (0..3); `+0x4` = 1 for the Extra
  chart. Both follow left/right live. The mod reads the stage data (`songs::SelectedChart`).
- **Sort mode** `pvsel+0x17DFC`: T cycles Star (6) > Singer (1) > Score (2) > Name (0) > Star. Step counter 1,2,3,0 at
  `pvsel+0x17E0C` and `pvsel+0x262C0`.
- **Tabs (Q / E)**: the strip depends on the sort. Star: 6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5, 10, ALL, heart. Singer: Miku, Rin,
  Len, Luka, KAITO, MEIKO, ALL, heart. ALL is always the second to last tab, heart the last. Q and E are the game's L (11)
  and R (12). The tab index field is NOT identified yet.
- **Counter** "NNN / 492 songs" is the cursor's position in the current (filtered, sorted) list; the list wraps.
- **Random** moves the cursor to a song at once (one highlight write per second while it spins). The real list order
  and filter differ per player, so jump by song id, after making sure the song is in the list (ALL tab).

Input: left/right = difficulty (button 5 from `0x1406F030F`, button 6 from `0x1406F02F1`); up/down = song (interval-tapped,
bit 3/4 of the input object `+0x60`); confirm = button 10 from `0x140669B42`; Q/E = 11/12; T = sort.

Open (what is needed to jump the cursor to a song id): (1) the tab index field and how to set ALL (tab count - 2, or
wrap with Q from tab 0); (2) the list control's "go to this song" function, the one random landing and the sort/tab
rebuild use to keep the cursor on a song (it takes a song id or an item pointer); its entry registers, via a watch on its
first instruction. Tools: write watch + unwound frames (2.4.7), object watch with tapped buttons (2.4.10).

## Sound streams (2.4.12 file open trace)

A song's `.ogg` reaches `CreateFileW` (the file layer probes every mod folder in turn, then `rom`) through this chain on
the main thread: `14099492C` (C runtime fopen-like) <- `140995424` <- `140172C40` (file wrapper) <- `14016BFA6` (path
normalisation, looks for `rom/`) <- `1402A5330` <- `1405AF560` (starts streaming a file) <- `1405AF420` (state machine,
jump table for states 0..6, state at object `+0x50`) <- `1405AF2A0` (per-object update) <- `1405AED20` (loops over the three
objects, 0x58 bytes each, global array at `0x14CBFF0A0`) <- `1405AA037` <- `1402B68C0` <- the task loop. So the game has
three stream players updated every frame; the preview/BGM/voice use them. The request that starts a stream is made by
other code earlier (the open runs from the update loop), so a write watch on a player's state field is the way to find it.

Stream player 0 is the song select preview (2.4.13 dump, pv 1231): `+0x00` std::string with the file name (text or pointer,
size `+0x10`, capacity `+0x18`), `+0x20` pointer to the stream object, `+0x28` / `+0x48` volume-like floats (0.18),
`+0x2C` a float 172.7 (length of the track?), `+0x30` a float that counts up in real time (35.8, then 38.1 two seconds
later: the playback position), `+0x50` the state, which the update loop rewrites every frame (0 then 1), so a write watch
on it only shows that loop. Players 1 and 2 were idle (state 0, nothing playing).

## Sound stream players, read from the 16 MB dump (static, capstone)

- `0x1405AEDA0` = `GetStreamPlayer(int index)`: returns `0x14CBFF0A0 + index*0x58` for index 0..2, else null.
- `0x1405AF2A0` = per-player per-frame update (fade counter at +0x44, state at +0x50, std::string name at +0x00).
- `0x1405AED20` / `0x1405AED70` = update-all / reset-all loops (3 players, stride 0x58).
- `0x1405AB150..0x1405AB250` are thin wrappers `Xxx(index)` calling `GetStreamPlayer` then `0x1405AF010/110/140/150/1B0` (is-playing style queries).
- `0x1405AB7A0` is a sound-effect cue map loader, `0x1405AFD30` returns a volume-config struct (not the players). Neither is the play function.
- Only 9 call sites use `GetStreamPlayer`, none sets the file name. The play function therefore writes the player directly (e.g. via a different accessor/inlined index math), so it cannot be found by xref alone.
- Next step: run "Watch that player's file name" (F8, 2.4.14) while a preview starts; its call chain names the play function.

## Preview start found (log 3a10b073 + dump, 2.4.15)

The F8 "Watch that player's file name" test caught the writer. Call chain (all game thread, song select frame):
`1406EDC40` (pvsel update, rcx = pvsel) -> `1406F7870` (preview state machine, rcx = pvsel+0x258D8) -> `1406F7650` (StartPreview) -> `1405AB190` (player 0 wrapper) -> `1405AF110` -> `1405AF010` (assign name to player) -> `14014BA50` (std::string assign).

Names written are 26 chars (`rom/sound/song/pv_NNNN.ogg`-style, copied from the pv entry +0x50).

- `StartPreview(state=rcx, const char* name=rdx, int id=r8d, float xmm3, [rsp+0x20]=float, [rsp+0x28]=float, [rsp+0x30]=bool)`; fields stored in state: +0 mode, +4 id, +8 xmm3, +0xC, +0x10, +0x20 std::string name.
- The state machine (`1406F7870`) calls it with name = state+0x20 and floats state+8/+0xC/+0x10; the song-list code (`1406EEDE0`, rdx = pv entry) calls it with name = entry+0x50, xmm3 = entry+0x88, stack float = entry+0x8C, second float 0 (entry+0x88/0x8C are most likely the chorus start/length).
- Plan: DR execute breakpoint on `1406F7650` (like filetrace's); when follow-host is on, replace rdx with our own char buffer (host's file name) and xmm3/stack floats. Needs the guest's cursor/state to trigger it, so best combined with the go-to-song work.

## Song select cursor and list (CE findings + static dump, 2.2.1-dev)

`pvsel` = song select object = highlight hook's `rcx` (the list object, `pvsel+0x3380`) minus `0x3380`.

- Cursor: `widget = pvsel+0x18F08`, `[widget+4]` = index of the selected row (confirmed: follows Down/Up in CE). Reached in
  code as `1406DE430(pvsel+0x1A50)` (+0x163E0) -> `1406E2FA0` (+0x10D8) -> getter `14023E8C0` (`mov eax,[rcx+4]`).
- Setter `14023E8D0(widget, _, int newIndex)`: if `[widget]&4` calls `14023F190(widget,newIndex)` (a jump into protected
  code, moves the view), then `[widget+4] = newIndex`. The go-to-song candidate. Not yet called from the mod.
- Per frame `1406EFD10` copies the cursor to `pvsel+0x261F0` (previous index, change detection; writing it does nothing)
  and derives the song: `item = at(pvsel+0x25930, index)`; `pv = [[[item]]]` i.e. `r15 = [[item]]`, `pv = [r15]` (`1406EFDBF..1406EFDE5`),
  stored to `pvsel+0x262A4` (`1406EFE8A`, written every frame, 15107 hits in the CE count). -1 = no item, -2 = random slot.
- `at(C, i)` (`140217640`): `V = [C+0xF0]`; count = `([V+8]-[V]) / 8` (`1402174E0`); element = `[[V] + i*8]`. So the list the
  player sees can be walked with plain reads: for i in 0..count, item = `[[V]+i*8]`, pv = `[[[item]]]`... as above.
- Random does not scroll: it replaces the random slot's song. `0x1406EFDA6/1406EFE8A/1406DEA22` etc. are in the CE list.
- `161CA2BEB` (protected code) is a `push_back` of ints into a vector at a game global `0x14CBFA9B0 (+0xA0)`; not needed now.
- Not found yet: the tab (ALL/favourites/...) field. The index above is into whatever list the current tab/sort shows.
