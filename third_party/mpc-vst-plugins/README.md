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

## Local patches (PolyForce)

Copied into SubForce from [PolyForce](https://github.com/Devko/PolyForce) with its patches as they
are (SubForce uses patches 1-4; it has no meters). EffectForce copied SubForce's copy unchanged.

`tools/shadow_skin.py` carries five small, marked (`PolyForce local patch`) changes:
1. `theme_title=` / `title_size=`: frame title colour and size (were fixed ACCENT_HI / 26 px).
2. Popup list options are drawn with the real title font (`SHADOW_TITLE_FONT`) like enum
   segments, instead of the 9x9 bitmap font.
3. `theme_tile_on=`: a filled lit state for `list` tiles (was a 3 px outline only).
4. `slider_v`/`slider_h` accept `cw=` (component width), so a tight row of sliders doesn't
   overlap hit areas and value labels.
5. A `meter` without `strip=` builds with the C renderer: its strip is drawn as a vertical
   slider's (`sh_meter_<w>x<h>.png`) for a post-step to redraw (PolyForce's wave view:
   `surface/skin_polish.py`). With the browser renderer a meter still needs `strip=`.
Re-apply them after re-copying upstream.
