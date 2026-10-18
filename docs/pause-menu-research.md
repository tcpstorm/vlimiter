# VLimiter: an "FPS Limiter" row in GTA V's pause menu (Settings > Graphics) under FiveM

Research only; no project code was changed. FiveM sources were cloned (sparse, blob:none) into
`...\scratchpad\pausemenu_research\src\fivem` (master, 2026-10-18); natives docs (`natives.json`,
`natives_cfx.json`) were fetched from runtime.fivem.net/doc/ into `...\pausemenu_research\natives\`.
The installed client at `%LOCALAPPDATA%\FiveM\FiveM.app` was only read. All of it was treated as data.

## Verdict

**Feasible with moderate risk, as a two-part design.** The Graphics list is fully data-driven:
`common:/data/ui/pausemenu.xml` declares each row as `<cTextId>` + `<MenuPref>` (an `eMenuPref` enum
value) + `<MenuOption>` (an `eMenuOption` enum value whose label list is declared in the same file).
FiveM ships its own edited copy (`FiveM.app\citizen\common\data\ui\pausemenu.xml`, mounted over
`common:/`) and, when `sv_pureLevel == 0`, also mounts every `FiveM.app\addons\*.rpf` whose `common/`
folder overlays `common:/` with higher priority. So an `.asi` can ship (or generate on first run) an
RPF7 carrying a modified `pausemenu.xml` with rows inserted beneath `VID_VSYNC`, **without hooking
anything**. The catch: `eMenuPref`/`eMenuOption` are compiled enums, so new names cannot be invented;
the rows must repurpose an existing preference slot that no live row uses, and the plugin reads the
chosen value back by polling the game's preference array (`CProfileSettings`; address pattern
maintained by FiveM in `GameProfileSettings.cpp`). Row labels are the hard part: a new GXT key needs
either a `.text` detour on the game's text lookup (FiveM already redirected the two call sites it
knows, so the plugin must find the callee body itself) or reuse of an existing label. Recommended
route: **(A) addon-RPF `pausemenu.xml` override + read-only polling of the pref array**, labels as
the one open item. Nothing in that route touches a vtable, `Present`, a Cfx DLL or read-only data.
Scaleform hooks and a FiveM "Settings > FiveM" tab are not practical (that tab does not exist;
FiveM's own `PM_PANE_CFX` pane is a *key-binding* category, not a settings page).

## 1. How Settings > Graphics is defined (verified)

### The data file
`FiveM.app\citizen\common\data\ui\pausemenu.xml` (3785 lines, `<CMenuArray v="5.0">`, contains FiveM
edits, e.g. `<!--Changed to allow Shadows=OFF-->`). Vanilla: `common.rpf\data\ui\pausemenu.xml`.
FiveM's copy is on the pure-mode allow-list `g_basePaths` as `"data/ui/pausemenu.xml"`
(`code/components/rage-device-five/src/CitizenMount.Shared.cpp` lines 36-50), so FiveM's own override
loads even under `sv_pureLevel 1`.

Graphics screen (line 2173 onwards):

```xml
<Item>
  <MenuScreen>MENU_UNIQUE_ID_SETTINGS_GRAPHICS</MenuScreen>
  <depth>2</depth>
  <Flags>SF_NoClearRootColumns</Flags>
  <ScrollBarFlags>Width_2</ScrollBarFlags>
  <MenuItems>
    ...
    <Item> <cTextId>GFX_TXAA</cTextId> <MenuAction>MENU_OPTION_ACTION_PREF_CHANGE</MenuAction> <MenuPref>PREF_GFX_TXAA</MenuPref> <MenuOption>MENU_OPTION_DISPLAY_ON_OFF</MenuOption> <MenuUniqueId>MENU_UNIQUE_ID_SETTINGS_LIST</MenuUniqueId> <Contexts>*ALL*,GFX_SUPPORT_TXAA</Contexts> </Item>
    <Item> <cTextId>VID_VSYNC</cTextId> <MenuAction>MENU_OPTION_ACTION_PREF_CHANGE</MenuAction> <MenuPref>PREF_VID_VSYNC</MenuPref> <MenuOption>MENU_OPTION_VID_VSYNC</MenuOption> <MenuUniqueId>MENU_UNIQUE_ID_SETTINGS_LIST</MenuUniqueId> </Item>
    <Item> <cTextId>MO_STER</cTextId> ... <MenuPref>PREF_VID_STEREO</MenuPref> ... <Contexts>*ANY*,VID_STEREO</Contexts></Item>
    <Item> <cTextId>VID_FOCUS</cTextId> ... <MenuPref>PREF_VID_PAUSE_ON_FOCUS_LOSS</MenuPref> <MenuOption>MENU_OPTION_DISPLAY_ON_OFF</MenuOption> ... </Item>
    <Item>  <MenuAction>MENU_OPTION_ACTION_SEPARATOR</MenuAction> </Item>
```

Display-value lists are declared once per `MenuOption` in a `MenuDisplayOptions` table (lines ~270-480):

```xml
<Item>
  <MenuOption>MENU_OPTION_VID_VSYNC</MenuOption>
  <MenuDisplayOptions>
    <Item> <cTextId>MO_OFF</cTextId> </Item>
    <Item> <cTextId>MO_ON</cTextId> </Item>
    <Item> <cTextId>MO_VID_VSHALF</cTextId> </Item>
  </MenuDisplayOptions>
</Item>
```

47 such tables exist; `MENU_OPTION_SLIDER` has none (numeric bar). Items are gated with
`<Contexts>*ALL*,token</Contexts>` / `*NONE*` / `*ANY*`; contexts are toggled from code (FiveM:
`PatchPauseMenuBuilds.cpp`) or script (`PAUSE_MENU_ACTIVATE_CONTEXT`). Action rows use
`<MenuAction>MENU_OPTION_ACTION_TRIGGER</MenuAction> <MenuUniqueIdHash>RESTORE_DEFAULTS_ADVANCED_GFX</MenuUniqueIdHash>`.
Instructional button `MO_GFX_APPLY` (line 1326) appears under context `GFX_Dirty`: graphics pref
changes are staged, then converted into `CSettingsManager` (settings.xml) on Apply.

### Storage: two layers
* **Preferences** (`CProfileSettings`): one `int` per `eMenuPref` value in a contiguous array.
  `code/components/gta-core-five/src/GameProfileSettings.cpp`:
  - `g_prefs = hook::get_address<int*>(hook::get_pattern("8D 0C 9B 8B 14 AA 8D 3C 4A 83 F8 FF 0F 84", -4));`
  - `g_profileSettings = hook::get_address<void**>(hook::get_pattern("44 38 7B 20 0F 84 ? ? 00 00 41 8D 7F 02", -4));`
  - enum read from the parser at runtime: `rage::GetStructureDefinition(0xA0063C79)->m_members[2]->m_definition->enumData`, iterating `fields` (`hash`, `index`), named via a `PREF_*` table (`HashRageString("PREF_VID_VSYNC")` etc.).
  - "pref changed" entry point: `_updatePref(int idx, int, int)` = `hook::get_call(hook::get_pattern("E8 ? ? ? ? 83 3D ? ? ? ? ? 44 8B C3"))`, called as `_updatePref(idx, 2, -1)`.
  - FiveM mirrors every pref into an archived ConVar `profile_<camelCase>` except `profile_vidMonitor` and all `profile_gfx*`; `PREF_VID_VSYNC` is exposed as `profile_vidVsync`.
  - Persisted in the profile file, redirected to `%USERPROFILE%\Saved Games\CitizenFX\GTA5\` (`SaveDataRedirection.cpp`, `MH_CreateHook(hook::get_pattern("FF 51 10 85 C0 78 32", -0x74), GetProfileFileName, ...)`).
* **Graphics settings** (`CSettingsManager`, `settings.xml`): built from prefs on Apply. FiveM comment in `GameProfileSettings.cpp`: "*This is where the Game converts the value from profile/preferences in the pausemenu to the CGraphicsSettings in the CSettingsManager*", patch `hook::nop(hook::get_pattern<unsigned char>("89 43 ? 8B 05 ? ? ? ? FF C0 89", 9), 2);`. Settings parser load hooked at `hook::get_pattern("4C 8B C7 48 89 44 24 20 E8 ? ? ? ? 84 C0 75 14", 8)` (`parser_SettingsLoad`, shadowQuality at `outStruct + 40`). `VidBehavior.cpp` forces defaults in that struct (`settings + 264` vsync = 0, `+260` borderless, `+248/+252` size).

### Code path and Scaleform
FiveM has not reversed `CPauseMenu`/`CSettingsMenu`; the names appear only in `gta_vtables.h` (RTTI:
`CPauseMenu`, `CSettingsMenu`, `CMenuBase`, `CScaleformPauseMenuLUTClass`, `DynamicPauseMenu`) and
`GameSkeleton.cpp` (init-function names). The Scaleform side (`pause_menu_*.gfx` in
`x64b.rpf/data/cdimages/scaleform_frontend.rpf`, `glue/include/BaseGameRpfHeaderHashes.h` line 19) is
filled by `CPauseMenu` from the parsed XML; FiveM never calls into it. Labels are GXT keys resolved
through the text lookup FiveM hooks in `CustomText.cpp`.

## 2. What FiveM itself does to the pause menu (verified, verbatim)

| File | What | Hook |
| --- | --- | --- |
| `rage-device-five/src/CitizenMount.Shared.cpp` 240-260 | Mounts `FiveM.app\citizen\common\` over `common:/` and `commoncrc:/` via a `PathFilteringDevice` (`commonFilter:/`); same for `platform` and `platform-<build>`. | `rage::fiDevice::OnInitialMount`, `fiDeviceRelative::Mount` |
| same, 286-325 | `if (fx::client::GetPureLevel() == 0)`: every `cfx:/addons/*.rpf` is opened as `rage::fiPackfile` (`OpenPackfile(fullFn, true, 3, 0)`), mounted at `addons:/<name>/`; `addons:/<name>/common/` is mounted on `common:/` + `commoncrc:/`, `platform/` on `platform:/` + `platformcrc:/`. Applied *after* the citizen mounts; `UpdateRpfOverrideMount.cpp` ("Mount some of the original game again, so they have higher priority") shows later mounts win. | plain mounts |
| `citizen-mod-loader-five/src/ModPackage.cpp` 130-200 | `if (GetPureLevel() >= 2) return;` then `cfx:/mods/*.rpf` OpenIV-style packages (`assembly.xml`, `target="Five"`) are mounted; `MountModDevice` mounts their `common/` on `common:/`. `ModsNeedEncryption()` is true at pure level >= 1. **FiveM does read `FiveM.app\mods`, contrary to the brief's assumption; it needs an OpenIV package layout.** | plain mounts |
| `gta-core-five/src/GameProfileSettings.cpp` | prefs array, enum, `profile_*` ConVars, shadow-quality patches. | `get_address`, `hook::call`, `hook::nop`, `hook::put` |
| `gta-core-five/src/PatchPauseMenuBuilds.cpp` | Activates context `b<build>` so XML can gate rows per build: `activatePauseMenuContext(HashString(fmt::sprintf("b%d", xbr::GetRequestedGameBuild())))`; stub `hook::get_pattern("48 8D 4C 24 30 89 44 24 30 E8 ? ? ? ? 84 C0 75 ? 0F", -11)`, call site `hook::get_pattern("8A DA 8B F9 E8 ? ? ? ? E8", 4)`. | `hook::call` |
| `gta-core-five/src/LeaveGameHandler.cpp` | Intercepts pause-menu trigger dispatch (`CustomTrigger(uint32_t trigger, a2, a3)`, 145 = account picker -> disconnect). Call sites: b3407+ `48 8D 8D ? ? ? ? BE ? ? ? ? 89 B5` (-5); b2802+ `48 8D 8D 18 01 00 00 41 BE 74 26 B5 9F` (-5); older `48 8D 8D 18 01 00 00 BE 74 26 B5 9F` (-5). | `hook::call` |
| `gta-core-five/src/CustomText.cpp` | GXT override map (`PM_PANE_LEAVE` = "Disconnect", `FE_THDR_GTAO` = "FiveM"). Patches two call sites: `hook::pattern("48 8B CB 8B D0 E8 ? ? ? ? 48 85 C0 0F 95 C0").get(0).get<void>(5)` and `hook::pattern("48 85 C0 75 34 8B 0D").get(0).get<void>(-5)`; original kept in `g_origGetText`. Exposed to resources as `ADD_TEXT_ENTRY` / `ADD_TEXT_ENTRY_BY_HASH` (`citizen-resources-gta/src/TextChangingFunctions.cpp`). | `hook::call` |
| `gta-core-five/src/GameInput.cpp` 1174-1442 | The only FiveM-added pause-menu *content*: key-binding category `PM_PANE_CFX` ("FiveM") under Settings > Key Bindings, via MinHook `.text` detours: `GetMappingCategories` (`hook::get_pattern("74 2A 48 63 CB 8D 56 10 48", -0x23)`), `GetMappingCategoryInputs` (`"45 33 ED 4C 8B E2 41 8B C5 45 85 C0 0F 84", -0x24`), `GetBindingForControl`, `GetNameForControl`, `MapControlInternal`, `GetControlForName`, `HandleMappingConflicts`. Rows are synthetic ids (`HashString(cmd) | 0x80000000`). Precedent that appending rows via `.text` detours on a list builder is tolerated by the anti-cheat, but specific to the key-mapping screen. | MinHook |
| `extra-natives-five/src/CameraNatives.cpp` | `game::AddCustomText("MO_FPS_FOV", "First Person Field of View")`. | text map |
| `gta-streaming-five/src/ScaleformHacks.cpp` 343 | Scaleform->game call logging by **vtable write** (`hook::put(&sfCallGameFromFlashVTable[1], ...)`): FiveM may, a plugin may not. | vtable write |
| `code/components/*`, `ext/native-decls` | **No** `CScaleformMgr::CallMethod` hook, no "Settings > FiveM" settings page, no FiveM native that adds pause-menu rows (only `GetPauseMapPointerWorldPosition.md`). | - |

## 3. Candidate approaches, ranked

### A. Override `pausemenu.xml` via `FiveM.app\addons\<name>.rpf` (recommended; pure level 0 only)
Mechanism: an RPF7 containing `common/data/ui/pausemenu.xml` = FiveM's current file with two rows
inserted after `VID_VSYNC` (`CitizenMount.Shared.cpp` 286-325). The addon mount is applied after
`citizen/common`, so it wins. The plugin can generate the RPF on first run from the live FiveM copy
(preserving FiveM's own edits) and write it next to `plugins\` (`MakeRelativeCitPath`). Never edit
`citizen\common\data\ui\pausemenu.xml` itself: the launcher re-verifies `citizen/` against
`content_index.xml`/`caches.xml` (`code/client/launcher/Updater.cpp` 314-370) and the file changes
with FiveM updates.

Rows must use existing enum values:
```xml
<Item> <cTextId>VLIM_ON</cTextId>  <MenuAction>MENU_OPTION_ACTION_PREF_CHANGE</MenuAction> <MenuPref>PREF_X</MenuPref> <MenuOption>MENU_OPTION_DISPLAY_ON_OFF</MenuOption> <MenuUniqueId>MENU_UNIQUE_ID_SETTINGS_LIST</MenuUniqueId> </Item>
<Item> <cTextId>VLIM_FPS</cTextId> <MenuAction>MENU_OPTION_ACTION_PREF_CHANGE</MenuAction> <MenuPref>PREF_Y</MenuPref> <MenuOption>MENU_OPTION_SLIDER</MenuOption>         <MenuUniqueId>MENU_UNIQUE_ID_SETTINGS_LIST</MenuUniqueId> </Item>
```
- `PREF_X`/`PREF_Y` candidates (in the enum, referenced only inside XML comments, no live row):
  `PREF_GFX_NIGHT_LIGHTS`, `PREF_VID_STEREO_SEPARATION`, `PREF_VID_ADAPTER`, `PREF_CINEMATIC_SHOOTING`,
  `PREF_REPLAY_AUTO_RESUME_RECORDING`, `PREF_REPLAY_MODE` (`PREF_PCGAMEPAD` appears in the XML but not
  in FiveM's name table). Whether the engine still *consumes* these (night-light quality into
  `CSettingsManager`, adapter index) is **unverified**; a stereo pref is the safest guess,
  `PREF_VID_ADAPTER` the most dangerous.
- A fixed list (30/60/90/120/144/165/240) needs a `MenuOption` whose `MenuDisplayOptions` table has 7
  entries. Tables can be edited in the same file (`MENU_OPTION_REPLAY_MEM_LIMIT` has 11,
  `MENU_OPTION_FEED_DELAY` 10) but that also changes the row that already uses them, so either accept
  `MENU_OPTION_SLIDER` (0..N mapped to the fps table by the plugin) or take over an option whose own
  rows are already hidden on FiveM (`MENU_OPTION_REPLAY_*` rows are gated `*ALL*,ReplayEditor`).
  Entry labels are GXT keys too.
- Labels (`VLIM_ON`, `VLIM_FPS`, values): need a GXT override. Options: (i) a `.text` MinHook detour on
  the game's text-lookup *function body* (the callee of the call sites FiveM patches; the plugin
  cannot `get_call` those sites because they now point into a Cfx DLL, so it needs its own body
  pattern: **unverified**); (ii) reuse existing keys (`MO_ON`/`MO_OFF` fine for values; nothing
  suitable for "FPS limit" found); (iii) a companion server resource calling `ADD_TEXT_ENTRY`
  (not client-only). Hooking inside Cfx DLLs is on the no-go list.
- Read-back: poll `g_prefs[idx]` once per frame from the existing present-site detour or the worker
  thread; resolve `idx` at runtime through the parser enum (`_parser_getStructure` =
  `hook::get_call(hook::get_pattern("8B 10 E8 ? ? ? ? 4C 8D 5C 24", 2))`, `g_parser` =
  `hook::get_address<void**>(hook::get_pattern("45 33 C9 E8 ? ? ? ? 48 8B F0 48 85 C0 74", -18))`,
  structure `0xA0063C79`, member 2, field with `hash == HashRageString("PREF_...")`). Writes from the
  plugin's own ImGui/ini go into the same slot (optionally `_updatePref(idx, 2, -1)`). The value also
  persists in the user's profile; keep the ini authoritative and treat the pref as the UI mirror.
- Rules: **no no-go item**; the only code hook is optional (text lookup, `.text`). Build survival:
  three FiveM-maintained patterns; the XML is shared across builds (FiveM uses one file with
  `b<build>` contexts). Pure-mode servers (`sv_pureLevel >= 1`) ignore `addons/`; the row just will
  not appear and the plugin's own window remains the fallback.
- Effort: low for XML/RPF (RPF7 writing is already documented in the vlights notes), medium for
  labels and the pref-slot audit.

### B. `.text` detours on the Graphics list builder / pref accessors (feasible, higher effort)
Detour the function that turns `CMenuItem`s into Scaleform rows, or the pref getter/setter, to append
rows or remap a slot (`GameInput.cpp` proves appending rows via a list-builder detour is tolerated).
FiveM has not reversed `CPauseMenu`, so every pattern is the plugin's own (disassembler, per build),
and rendering needs the right `SET_DATA_SLOT` arguments. Worth it only to *extend* A (e.g. clamp or
translate the pref write). `.text` only, main thread: acceptable. Effort high.

### C. Hooking Scaleform method calls (not recommended)
Detour `CScaleformMgr`'s method-call path to inject rows and intercept selection callbacks. FiveM only
touches Scaleform via a vtable write (not allowed for the plugin) and has no `CallMethod` pattern to
borrow; selection comes back through `sfCallGameFromFlash` (vtable). Rows added this way have no pref
backing. Medium risk, very high effort, fragile across builds.

### D. A FiveM resource using natives (does not solve the UI; companion only)
Natives *control* the pause menu (`ACTIVATE_FRONTEND_MENU`, `SET_PAUSE_MENU_ACTIVE`,
`PAUSE_MENU_ACTIVATE_CONTEXT`, `_GET_PAUSE_MENU_SELECTION_DATA`, `_PAUSE_MENU_GET_UNIQUE_ID_OF_MOUSE_HOVERED_SLOT`,
`GET_PAUSE_MENU_STATE`, `GET_PROFILE_SETTING(int)`, `STAT_SET_PROFILE_SETTING_VALUE` "does not take
effect immediately") and add text (`ADD_TEXT_ENTRY`), but **none adds rows**. No "Settings > FiveM"
page exists. A resource is server-side and the plugin cannot call natives (`sv_scriptHookAllowed`).
A resource could complement A: `ADD_TEXT_ENTRY("VLIM_FPS", "FPS Limit")` for labels,
`PAUSE_MENU_ACTIVATE_CONTEXT` to show/hide the rows, `GetConvarInt("profile_...")` for non-gfx prefs.

### E. Other credible options
- `FiveM.app\mods\*.rpf` OpenIV package with the same XML: works up to pure level 1 (needs an
  encrypted RPF there, `ModsNeedEncryption()`), heavier format; prefer `addons/` unless pure level 1
  support matters (encryption requirement **unverified**).
- A `MENU_OPTION_ACTION_TRIGGER` row with a custom `MenuUniqueIdHash` ("next FPS value") plus a
  `.text` detour on the trigger dispatcher that `LeaveGameHandler.cpp` wraps. No pref slot needed,
  but the dispatcher body pattern is the plugin's own and a trigger row cannot display a value.
  Fallback if no safe pref slot exists.

## 4. Per-approach summary

| | Read-back | Build survival | No-go list |
| --- | --- | --- | --- |
| A XML override + pref poll | poll `g_prefs[idx]`, idx from parser enum; optional `_updatePref` | 3 FiveM-maintained patterns; XML shared across builds | clean (optional text-lookup `.text` detour) |
| B list-builder detour | hook getter/setter | own patterns, per build | `.text` only, fragile |
| C Scaleform hook | intercept flash->game callback | own patterns | needs vtable write or deep detours |
| D resource | `GetConvarInt`, natives | n/a | n/a (server-side) |
| E trigger row | detour trigger dispatcher | own pattern | `.text` only |

## Unverified / could not confirm
1. Parser behaviour on a `pausemenu.xml` with unknown enum names (irrelevant if only existing names
   are used) and on a malformed file (crash vs. skip).
2. Which "free" `eMenuPref` slots are inert on PC and whether changing one sets `GFX_Dirty`,
   requires Apply, or triggers a device reset. In-game test needed.
3. The game's text-lookup function body pattern (FiveM patches call sites only).
4. Mount precedence `addons/` vs `citizen/common` inferred from order and a FiveM comment, not tested.
5. Whether the launcher ever cleans `FiveM.app\addons\` (no reference in `code/client/launcher`;
   `Streaming.cpp` 237 treats `addons:/` as "mods installed by the users themselves").
6. Structure hash `0xA0063C79` could not be matched to a class name; reused as FiveM does.
7. How common `sv_pureLevel 0` is on the user's servers; everything in A depends on it.

## Next steps if proceeding
1. Audit pref slots: dump the parser enum at runtime (FiveM's method), log all `PREF_*` names and
   values, pick a slot, flip it from the ImGui window, confirm nothing visible changes and
   `settings.xml` is untouched after Apply.
2. Build the XML: copy the live FiveM `pausemenu.xml`, insert two rows after `VID_VSYNC`, pack as RPF7
   (`common/data/ui/pausemenu.xml`), drop into `FiveM.app\addons\`, join a pure-level-0 server, check
   the rows render (labels show the raw key until step 3).
3. Labels: prove the keys with `ADD_TEXT_ENTRY` from a local resource, then choose between a
   text-lookup detour and reusing vanilla keys.
4. Wire read-back: poll the slot from the present-site detour, map slider value -> fps table, write
   ini; push ini changes back into the slot so the menu shows them.
5. Document in `docs/quirks.md`: pure level 0 requirement, "FiveM reads `addons/` and `mods/`",
   pattern provenance.