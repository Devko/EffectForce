# Vendored: sd88me/mpc-vst-plugins (MIT)

Copied from https://github.com/sd88me/mpc-vst-plugins at commit
`670b20b9ab655140f324f9d093aa5c6f35929f73` (2026-10-04). License: see `LICENSE`
(MIT, Copyright (c) 2026 sd88me); `tools/vendor/force-shadow/` carries its own MIT LICENSE.
Unmodified apart from the marked local patches listed below.

What we use it for (we do NOT link their `vst2_wrap.c` / `engine.h` wrapper — our plugin glue is
`plugin/` in this repo):

| File | Used for |
|---|---|
| `tools/gen_vst.py`, `shadow_skin.py`, `skin_assets.py`, `params.py`, `shadow_art.c`, `vendor/force-shadow/` | `params.json` + `layout.conf` -> `params.h`, the touchscreen skin (`TUI.json`, `Q-Links.json`, PNGs) and the `pluginList-arm` entry |
| `tools/studio.py`, `studio_web.py` | preview skin pages as PNG; browser layout editor |
| `tools/release.py`, `tools/release/*` | release zip + on-device `install.sh` / `uninstall.sh` (stop MPC, back up and edit `MPC.settings`) |
| `wrapper/popup.h`, `wrapper/plugin_dir.h` | popup-picker param helpers; find the plugin's own folder at runtime |

Do not edit files here beyond the marked patches; patch around them so an upgrade stays a plain
re-copy.

## Local patches (PolyWerk)

Copied into SubWerk from [PolyWerk](https://github.com/Devko/PolyWerk) with its patches as they
are (SubWerk uses patches 1-4; it has no meters). EffectWerk copied SubWerk's copy unchanged.

`tools/shadow_skin.py` carries five small, marked (`PolyWerk local patch`) changes:
1. `theme_title=` / `title_size=`: frame title colour and size (were fixed ACCENT_HI / 26 px).
2. Popup list options are drawn with the real title font (`SHADOW_TITLE_FONT`) like enum
   segments, instead of the 9x9 bitmap font.
3. `theme_tile_on=`: a filled lit state for `list` tiles (was a 3 px outline only).
4. `slider_v`/`slider_h` accept `cw=` (component width), so a tight row of sliders doesn't
   overlap hit areas and value labels.
5. A `meter` without `strip=` builds with the C renderer: its strip is drawn as a vertical
   slider's (`sh_meter_<w>x<h>.png`) for a post-step to redraw (PolyWerk's wave view:
   `surface/skin_polish.py`). With the browser renderer a meter still needs `strip=`.
Re-apply them after re-copying upstream.

## Local patches (SubWerk)

6. `tools/release.py` (marked `SubWerk local patch 6`): the generated `INSTALL.md` gave every
   `--user-data` entry a trailing slash, files too (`preset_favorites.txt/`); now only folders get one.
   Ported to EffectWerk from SubWerk on 2026-10-06, before the first release. Worth sending upstream.
7. `tools/release.py` and `tools/release/install.sh` (marked `SubWerk local patch 7`): the -Werk
   rename. `release.py --previous-name <old name>` tells the installer the plugin's old folder
   (`<vendor> - VST - <old name>`); the installer carries the `--user-data` out of it into the new
   folder and removes it once MPC.settings is written. The new folder's own copies win, except while
   MPC.settings still points into the old folder (an earlier run stopped before the settings were
   written, so the old plugin kept running and saving there). A folder whose `plugin-meta.xml` has
   another uid is left alone, with a warning. `release.py` refuses an old name that isn't a plain
   name (letters, digits, spaces, `. _ -`; no `/` or `..`) or that is the current name. The settings
   entry needs nothing new: the uid didn't change, and entries with the same uid were already
   replaced. `test/install_test.sh` and `test/release_test.sh` (`make test-install`) cover it.
   Expected side effect: `tools/catalog_check.py` regenerates `install.sh` from the template without
   `@PREV_SKIN@` and so warns that it differs from the current template.
   Ported from SubWerk on 2026-10-28.
