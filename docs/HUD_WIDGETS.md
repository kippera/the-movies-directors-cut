# HUD / UI element index (The Movies SE, 2560x1440 widescreen fix)

Index of every UI piece we know about, how it is drawn, how it is fixed, and what is still open.
Details and history are in `HANDOVER.md`; code is `gfx/d3d9proxy.c` (proxy `d3d9.dll`) and `mfl/*.py` (Flash tools).

## Two kinds of UI
| Kind | Examples | Fix technique | Knob |
|---|---|---|---|
| Flash movies (`Data/ui/flash/*.mfl`) | build menus, portraits, panels, star ratings, badges, bars, menus | loose-file override: root placements squeezed horizontally by K=0.75 (`mfl/mflsqueeze.py`), optional right-edge shift | files in `Game Files/Data/ui/flash/` (delete one to restore stock) |
| Native widgets (heap objects with vtable, drawn as pre-transformed quads) | time controls, date pill, options dialogs, money pill, timeline strip | scale the widget's layout inputs from the proxy (`widgets_apply`, `dialogs_apply`) | ini `HudScale`, `DialogScale`, `HudTextX/Y` |

Widget vtables: WImage `0xd39ae4`, WButton `0xd39d5c`, WText `0xd5e54c` / `0xd4ad2c`, option row `0xd4b8bc`, options panel root `0xd3358c`.
Edge records (0x24 bytes) at `+0x98` top, `+0xbc` left, `+0xe0` bottom/height, `+0x104` right/width; float0 = input, float1 = laid-out px.

## Flash HUD pieces (squeezed, user-verified unless noted)
| Piece | Movie(s) | Squeeze shift | Status |
|---|---|---|---|
| Build menu (round buttons, bottom-left) | `buildmenu4` (+2,3, `buildingmenu_*`) | 0 | done |
| Actor portraits + pop-up bubbles | `4pip`, `1pip` | 0 | done |
| Actor panel frames | `iconpanel2`, `iconpanel` | 0 | done |
| Level badge (arc of stars) | `league_position`, `starrating` | 8 | done |
| Movie-list item (reel + number) | `filmpanel2` | 8 | done |
| $ / star decorations | `sitt_star_root` | 8 | done |
| Stars row under the movie box | `starrating_small` | 0 | done (centred) |
| Top bars | `bar_primary/secondary/tertiary(_rev)`, `bar_progress`, `bar_battery`, `statbar`, `fightbar` | 0 | squeezed, not individually re-checked |
| Other (info, awards, watch windows, buttons, league, win/fun) | `infoness*`, `awardcorner*`, `watchwindow*`, `button_*`, `league_*`, `winfun*`, `badness*`, `stuntmanness*` | 0 | squeezed in bulk pass, not individually re-checked |
Squeeze formula: K = 4/3 * H/W (0.75 at 16:9); right-anchored pieces also need shift_px = stageW*(1-K). 68 files total.

## Full-screen movies (widened to 16:9, centred)
`pause`, `moviemaker`, `menu`, `challenge_fullscreen`, `fullscreen_person`, `button_pop`, `opening` -> done.
`frontend.mfl` -> main menu v4 layout (`mfl/mflspread2.py --ref`, matches the 4:3 reference); background video left stretched on purpose.
Intro/full-screen videos are stretched 4:3 -> 16:9 by the game; skip with Esc.

## Native pieces
| Piece | Status | How |
|---|---|---|
| Time controls (date pill, strip, 4 buttons, date text) | done, `HudScale=1.5`, text `HudTextX=-5` | 8 fingerprinted widgets (`g_wfp`), inputs scaled; hit areas follow |
| Options dialogs (Video/Audio/Game/Online) incl. bands, labels, values | done, `DialogScale=1.5` | panel roots scaled about their centre; bands matched to rows (`dialog_band_fit`); text anchors (`dialog_text_anchor`) |
| Resolution list | done | filter at `0x9a53c0` patched to allow 16:9 from 1280 wide |
| Dim overlay behind dialogs | done | |
| Timeline strip height (y 0..16) + its icons/markers | done, `BarZoom=1.5` | vertex zoom of draws with y1 <= BAR_Y (33); time-control block follows |
| Money pill (top right) | done, scales with `HudScale` | vertex zoom of its draws about its right end, below the strip |
| Menu button (first time-control button) | done | game re-sets its position every frame; code trampolines at 0x741a0a / 0x741c27 substitute our target |
| Mask/arrows icon above the actor list (x 30..185, y 78..122) | OPEN, still stretched | native texture; widget scale or aspect fix |
| Other in-game dialogs (save/load, prompts, confirmation) | UNCHECKED; seen unscaled: "Create Your Studio" screen, small Error dialog ("Could not find CD Key!") | probably the same panel roots (vt `0xd3358c`); check `DialogScale` coverage |
| Quit prompt (main menu > Quit) | OPEN: not centred on screen | native dialog, probably same panel root family as the options dialogs |
| Pop-up bubbles (tutorial/info bubbles, actor hover pills) | done, `BubbleScale=1.5`, click area remapped | native, animated; zoom about the tail tip, pointer remap on clicks |
| "Lot Prestige" pins | not feasible via the 2D path | 3D billboards |

## How to find the next widget
1. Touch `TheMoviesGfx.probe.now` next to the dll while the element is on screen -> `TheMoviesGfx.probe.log` lists draws with vertex bboxes (raw, pre-transform).
2. Touch `TheMoviesGfx.widgets.all` -> `TheMoviesGfx.widgets.csv` dumps all widgets (addr, vtable, inputs, abs rect, parent). Match the bbox to a rect, read the vtable and parent chain.
3. Add a fingerprint (vtable + input floats + abs rect) like `g_wfp`, scale the inputs, and check hit areas still match.
4. Reference dumps: `/tmp/claude-1000/shots/widgets_audio.csv`, `widgets_game.csv` (options pages; may be cleared on reboot). `annotated_ui_map.png` in this folder labels the studio HUD.
