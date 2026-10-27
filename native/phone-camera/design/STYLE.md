# MyCam style guide: Windows XP (Luna Blue)

The working reference for both apps. [redesign.md](../docs/redesign.md) explains the reasoning. This file is the
rulebook. If the code and this file disagree, fix one of them; don't let them drift.

Where the tokens live:
- Phone: `app/.../ui/theme/Color.kt` (`Xp`) and `Type.kt`.
- PC: `pc/companion/xp_draw.cpp`.
- Art: `design/tools/luna_draw.ps1`.

## Character

The look is bright, rounded, optimistic and friendly. A sky-blue title bar, warm beige surfaces, green
means "go", soft gloss. MyCam lives in people's homes, so it should feel like a friendly tool you're in
charge of, never like surveillance.

- **Luna Blue only.** No Olive or Silver schemes.
- **No dark mode.** The only dark screen is the phone's Dim screen (the hills at night).
- **XP is the inspiration only.** No Microsoft artwork, icons, wallpapers or font files are shipped.

## Colour

| Role | Value | Notes |
|---|---|---|
| Title bar | `#3D8AF7` → `#0A5DEB` → `#0053E1` → `#0047D0`, highlight `#7FB6FF`, edge `#0831D9` | White bold text with a `#0A1E78` shadow |
| Surface | `#ECE9D8` | XP window body; phone Settings |
| Card | `#FFFFFF` | |
| Task pane | `#7BA2E7` → `#6375D6` | Behind task groups |
| Group header | `#FFFFFF` → `#C6D3F7`, title `#215DC6` bold | |
| Hero header | `#0055E5` → `#2463D6`, white | The "Now" group |
| Paused hero header | `#B05F00` → `#8F4C00`, white | Darker than XP amber so white text passes AA |
| Group body | `#D6DFF7`, white border | |
| Selection | `#316AC5`, white text | |
| Link | `#215DC6` | Underlined |
| Text / subtle / disabled | `#000000` / `#4D4D4D` / `#8A877A` | |
| Warning text | `#8F5200` | Heat warning; 5.6 : 1 on white |
| Go green | gloss `#5DBE5D` (thin top band), body `#2B842B` → `#226B22`, border `#1D5E1D`, white text | LIVE pill, Resume, Next |
| Progress chunk | `#E2F8E2` → `#6FD86F` → `#2DB52D` → `#5ACD5A`; track white, border `#ACA899` | |
| Button | `#FFFFFF` → `#ECEBE6` → `#D6D0C5`, border `#003C74`, radius 3 | Hover/press adds the orange inner ring `#FFCF6B` → `#E5A01A`; the default button gets the blue ring `#CEE7FF` → `#6982EE` |
| Checkbox / radio | border `#1C5180`, mark and dot `#21A121` | XP checks are green |
| Tabs | border `#919B9C`, selected top line `#E68B2C` / `#FFC73C` | |
| Badges (graphics only) | amber `#FFD86A` → `#E08A00`, red `#FF7B6B` → `#C81E0F`, green `#7BD87B` → `#21A121` | Never behind text |

Every text pair must meet WCAG AA (4.5 : 1; 3 : 1 for bold text at 14 pt or larger). Check new pairs before
using them.

## Type

| Role | PC (from Windows) | Phone (shipped) |
|---|---|---|
| Body | Tahoma 8 pt | DejaVu Sans Condensed 14 sp |
| Group titles, buttons | Tahoma Bold 8 pt | DejaVu Sans Condensed Bold |
| Caption, main instruction | Trebuchet MS Bold 10 / 13 pt | DejaVu Sans Bold 18 / 22 sp |
| Pairing code | Trebuchet MS Bold 24 pt | DejaVu Sans Mono Bold 40 sp |

The PC falls back to Segoe UI if a font is missing. Phone body text is never smaller than 14 sp, and layouts
must survive a 1.3 font scale.

## Shape and spacing

- **Spacing:** a 4 dp grid (4, 8, 12, 16, 24, 32). Touch targets are 48 dp or more on the phone.
- **Radii:** buttons 3, task groups 4 (top corners only), dialogs and cards 8, the frosted card on camera
  pictures 16.
- **PC window:** rounded top corners, square bottom corners, like XP windows.

## Components

| Component | Phone | PC |
|---|---|---|
| Title bar | `XpTitleBar` | Custom caption with red close and blue minimise buttons |
| Tabs | Now / Settings | — |
| Task group (normal, hero, paused hero), collapsible | ✅ | ✅ |
| Push button, hero button, default button | ✅ | ✅ |
| Checkbox (green check), radio (green dot) | ✅ | ✅ |
| Green progress bar: marquee / determinate | ✅ | ✅ |
| LIVE pill | ✅ | ✅ (frosted chip over the preview) |
| Pairing dialog | Modal over a blurred backdrop | XP dialog window |

## Effects

Use effects only where they help: readability over imagery, progress, or explaining a change of state.
| Effect | Allowed places |
|---|---|
| Green marquee | Connecting, starting the camera, applying a quality/fps/camera change, the Waiting picture |
| Determinate bar | Pairing countdown, first-run steps |
| Frosted glass | The card on the camera pictures (baked in), the chips over the PC preview |
| Blur | The background of the camera pictures (baked in), behind the phone pairing dialog (API 31+) |
| Gloss | Title bars, buttons, badges, the Go green |

**Motion:**
- 120–300 ms with ease-out: status crossfade, badge pop, a one-shot LIVE sweep, group expand/collapse,
  the tab line, dialog in.
- No looping decoration in steady states. Never a progress bar in a steady state. No parallax, no animated
  app backgrounds, no animated tray icon.
- **Reduced motion:** every transition becomes instant and the marquee becomes a static "Working…".
  - Phone: when `ANIMATOR_DURATION_SCALE` is 0 or touch exploration is on.
  - PC: when `SPI_GETCLIENTAREAANIMATION` is off.
- **High contrast (PC):** flat system colours with no gloss, blur or motion.

## Logo and icons

**"Snap"** is a rounded Luna-blue compact camera:
- a silver top band, an amber shutter button and a green light;
- the lens shows a sky, a cloud and a hill. **Never a pupil or a centred catchlight**: it must not read as an
  eye.
- Sizes: 16 px simplified, the shutter from 24 px, full detail from 48 px.

**Status badges** sit bottom-right with a white outline:
- streaming: green ▶
- paused: amber ‖
- error: red ×
- disconnected: the whole camera desaturated

## Copy

- Short, second person, and say what happens next ("Apps see a 'Camera paused' picture").
- Same words on both apps: "Brightness" (not exposure), "Torch (phone light)", "Back camera / Front camera".
