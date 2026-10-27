# MyCam redesign: Windows XP edition

A UI/UX review and redesign of the Android app and the Windows companion.
Written 2026-10-10 against branch `wireless` (1.4.0-beta3). Revision 2: the design language moves from
Windows 7 Aero to **Windows XP (Luna)**.

**What changed in this revision (owner's brief):**
1. The design language is **Windows XP**, not Windows 7.
2. A camera preview **on the PC only**. The phone gets no preview.
3. XP's **green progress bar**, plus **glass, blur and motion**, used only where they help.
4. A **new logo**. Today's webcam reads as an eye and feels like surveillance. The new one is a friendly
   camera.
5. A new **pause screen** on a sunny **green hills and blue sky** background.

**Constraints that still apply:**
- **Capability-driven UI.** Every option comes from what the phone reports (`CameraStreamer.modeFor`).
  Nothing is tuned to the test phone.
- Confirmed phone behaviour: opening the phone's own camera app stops the stream.
- **No Microsoft artwork or fonts are shipped.** XP is the *inspiration*. The wallpaper, icons and chrome are
  drawn from scratch by the scripts in `design/tools/`, and the phone ships only freely licensed fonts. See
  §9.
- No protocol change.

---

## 1. Summary

The structural problems from the first review still stand, and the XP move doesn't fix them:

1. **You can't see what apps receive.** A preview belongs on the PC, where the companion already decodes
   every frame.
2. **The PC window doesn't fit common screens.** It's a fixed 420 × 780 DIP with no scroll, so it's cut off
   at 1366 × 768, or 1080p at 150 %.
3. **The phone is one long, flat scroll.** Status, pause, camera, video, controls and Wi-Fi all have the
   same weight.
4. **Pairing is security-critical but looks like ordinary content**, on both ends.
5. **Accessibility gaps.** The PC window is invisible to Narrator. Phone checkboxes and segments don't
   announce their state. The LIVE pill and the warning text fail contrast.
6. **The logo** (a blue iris with a pupil and a catchlight) looks like an eye watching you, which is the
   wrong feeling for a camera that lives in your room.

The redesign answers all six in the XP idiom:

- **XP task panes** (the blue sidebar with collapsible white groups from XP's Explorer) become the
  organising pattern for settings on both apps.
- The **PC window** becomes "task pane on the left, preview on the right", and fits a 720 DIP work area.
- The **phone** splits into **Now** and **Settings** using XP property-sheet tabs.
- **Pairing** uses a dedicated dialog with the green progress bar counting down.
- A **friendly compact-camera logo** whose lens shows a little landscape instead of a pupil.
- A **"Camera paused" picture** of sunny hills behind a frosted-glass card.

---

## 2. Audit (still valid after the style change)

Severity: **P1** breaks a task or excludes users · **P2** noticeable friction · **P3** polish.

### 2.1 Phone app

| # | Sev | Finding | Where |
|---|---|---|---|
| A2 | P1 | `AeroCheckbox` uses `clickable(role = Checkbox)` without toggleable state, so TalkBack doesn't say "checked". `AeroSegmented` items don't expose `selected`. | `AeroComponents.kt:301`, `:143` |
| A3 | P1 | The pairing request is inline content inside the scroll. It can be scrolled away, and Allow and Don't allow look the same. | `WebcamScreen.kt:91-109` |
| A4 | P2 | Seven sections of equal weight in one scroll. Camera controls take space even when nothing is streaming. | `WebcamScreen.kt` |
| A5 | P2 | Portrait only in practice. A phone used as a webcam is usually landscape, on a tripod or clip. | layout |
| A6 | P2 | The heat warning `#D68000` on white is 3.0 : 1, which fails AA. | `WebcamScreen.kt:134` |
| A7 | P2 | The LIVE pill has white 11 sp text on light green: 2.4–2.9 : 1. | `AeroComponents.kt:261` |
| A8 | P2 | The footer only repeats the connection state, in the most reachable part of the screen. | `WebcamScreen.kt:194` |
| A9 | P2 | No first-run guidance, and no pointer to the PC app. | `strings.xml:16` |
| A10 | P2 | Greyed-out frame rates don't say why. | `CameraControls.kt:64` |
| A11 | P3 | "Forget paired PCs" is all-or-nothing. | `WebcamScreen.kt:181` |
| A13 | P3 | The `−`/`+` buttons have no content descriptions. | `CameraControls.kt:99` |
| A14 | P3 | No feedback while a camera change applies (~1 s). | — |
| A15 | P3 | Wording drift: "exposure" vs "Brightness"; "Torch (light)" vs "Torch (phone light)". | `strings.xml:44-50` |

### 2.2 Windows companion

| # | Sev | Finding | Where |
|---|---|---|---|
| W1 | P1 | The fixed 420 × 780 DIP window doesn't fit work areas under ~790 DIP. Close and General end up off-screen. | `settings_window.cpp:36` |
| W2 | P1 | No UI Automation provider, so Narrator sees one blank pane. | `settings_window.cpp` |
| W3 | P1 | No preview, although the frames are already decoded. | — |
| W4 | P1 | The pairing code appears only in the headline and a balloon. The PC can't cancel, and a long phone name truncates the code. | `status_text.cpp:27`, `main.cpp:113` |
| W5 | P2 | "Controls appear once…" is drawn over the Focus and Torch rows. | `DrawControlValues` |
| W6 | P2 | "Forget Wi-Fi phones" overflows the right margin. Tab order doesn't match the visual order. | `Layout()` |
| W7 | P2 | Hand-typed absolute coordinates, which caused W5 and W6. Text doesn't reflow with Windows "Text size". | `Layout()` |
| W8 | P2 | Pause is a small corner button on the PC but the hero action on the phone. | `Layout()` |
| W9 | P2 | Arrow keys walk every element in turn instead of moving within a group. No access keys. | `WM_KEYDOWN` |
| W10 | P3 | Titled "MyCam Settings", but it's the main window. | `DrawFrame` |
| W11 | P3 | Before the camera reports, the PC shows greyed controls and the phone hides them. | — |
| W12 | P3 | The tray menu has no check mark on the active camera. | `main.cpp:156` |

### 2.3 Keep

- The **status model**: one icon, one main instruction, one grey sentence (`DescribeStatus` / `describe()`).
- The **copy voice**: short, second person, explains consequences.
- **Capability-driven greying** of quality and fps.
- The **Dim screen** idea (OLED drift, private, cool), restyled in §5.6.
- The **scripted asset pipeline** (`design/tools/*.ps1`), retargeted to XP (§8).

---

## 3. Design language: Windows XP (Luna), made friendly

XP's character: **bright, saturated, rounded, optimistic**. A sky-blue title bar, warm beige surfaces,
green "go" accents, soft gloss, chunky friendly icons with a slight 3D tilt. That suits a product that
lives in someone's home office far better than a cool, technical look.

### 3.1 Principles

1. **See it on the PC, control it from either.** The preview lives on the PC; the phone is a remote
   control and a status light.
2. **Now vs. settings.** Things you touch during a call are one tap away. Things you set once sit in
   collapsible task-pane groups.
3. **Privacy is the hero action.** Pause has the same weight and treatment on both apps.
4. **Same state, same look.** Identical labels, icons and rules for unavailable controls on both sides.
5. **Fits anywhere.** Phone in portrait and landscape; the PC window fits a 720 DIP work area at any scale.
6. **Effects earn their place.** Glass, blur and motion are used only to (a) keep text readable over
   imagery, (b) show progress, or (c) explain a change of state. Never as decoration on every surface. §6
   lists every allowed use. Anything not on that list stays flat.

### 3.2 Colour tokens (Luna Blue, original values tuned for contrast)

| Token | Value | Use |
|---|---|---|
| `TitleBar` | `#3D8AF7 → #0A5DEB (8 %) → #0053E1 (50 %) → #0047D0`, highlight line `#7FB6FF`, edge `#0831D9` | Window caption (PC), app header (phone) |
| `TitleText` | `#FFFFFF`, 1 px shadow `#0A1E78` | Caption text (5.7 : 1 on the bar) |
| `Surface` | `#ECE9D8` | XP window body (PC), Settings background (phone) |
| `Card` | `#FFFFFF` | Content cards and the task-pane group body (light variant) |
| `TaskPane` | `#7BA2E7 → #6375D6` vertical | Task-pane background |
| `GroupHeader` | `#FFFFFF → #C6D3F7` horizontal, title `#215DC6` bold | Normal task group |
| `GroupHeaderHero` | `#0055E5 → #2463D6`, title white bold | The "Now" group (status + Pause) |
| `GroupBody` | `#D6DFF7`, border `#FFFFFF` | Task group content |
| `Selection` | `#316AC5`, text white (5.3 : 1) | Selected option, focused row |
| `Link` | `#215DC6` (4.6 : 1 on `GroupBody`), hover `#428EFF` + underline | Task links |
| `Text` / `Subtle` | `#000000` / `#4D4D4D` (7.6 : 1 on `Surface`) | Body / secondary |
| `GoGreen` | `#3FAA3F → #2A7F2A`, border `#1D5E1D`, text white (5.0 : 1 on the low stop) | LIVE pill, Resume, primary "go" buttons (XP Start-button green) |
| `Progress` chunk | `#E2F8E2 → #6FD86F (45 %) → #2DB52D (55 %) → #5ACD5A`, track `#FFFFFF`, border `#ACA899` | The green progress bar (§6.1) |
| `Amber` | `#FFD86A → #E08A00`, graphics only | Pause badge |
| `WarningText` | `#8F5200` (5.6 : 1 on white) | Heat warning, "needs better light" |
| `ErrorRed` | `#FF7B6B → #C81E0F`, graphics only | Error badge, close button |
| Button normal | `#FFFFFF → #ECEBE6 (85 %) → #D6D0C5`, border `#003C74`, radius 3 | Push buttons |
| Button hover | normal + inner 2 px `#FFCF6B → #E5A01A` (XP orange hot-track) | Mouse hover (PC), press (phone) |
| Button default | normal + inner 2 px `#CEE7FF → #6982EE` | The one default button per dialog |
| Checkbox | 13 px (PC) / 20 dp (phone) white box, border `#1C5180`, **green check `#21A121`** | Checkboxes (XP checks are green) |
| Radio | border `#1C5180`, **green dot** `#7BD87B → #21A121` | Option groups |

All text pairs meet WCAG AA. Amber and red appear only in badges and icons (3 : 1 applies to graphics).

### 3.3 Type

| Role | PC (installed with Windows, not shipped) | Phone (shipped, free licence) |
|---|---|---|
| Body | Tahoma 8 pt (11 px) | **DejaVu Sans Condensed** 14 sp (Tahoma-like proportions, Bitstream Vera licence) |
| Group titles, buttons | Tahoma Bold 8 pt | DejaVu Sans Condensed Bold 14 sp |
| Window title, main instruction | **Trebuchet MS Bold** 10 pt / 13 pt | DejaVu Sans Bold 18 / 22 sp |
| Pairing code | Trebuchet MS Bold 24 pt, tabular | DejaVu Sans Mono Bold 40 sp |

Selawik and its licence file go away when the XP UI lands (it was the Segoe stand-in for the Win7 look).

### 3.4 Shape and spacing

- **4 dp grid**: 4 · 8 · 12 · 16 · 24 · 32.
- Radii: buttons 3, task groups 4 (top corners only, like XP), cards and dialogs 8, the frosted glass card on
  camera pictures 16.
- XP windows have **rounded top corners and square bottom corners**. The PC window keeps that silhouette
  (DWM corner preference off, with the rounding drawn in the caption).

---

## 4. Logo and icons (rework)

### 4.1 What's wrong today

The current mark is a round webcam whose lens is a **blue iris, a black pupil and a white catchlight**: the
anatomy of an eye. Combined with being "always connected" in the tray, it reads as *something is watching
you*. A webcam in your home should feel like a friendly tool you're in charge of.

### 4.2 New mark: "Snap", a friendly compact camera

```
          ┌──┐            ●  ← amber shutter button (warm accent)
     ╭────┴──┴──────────────╮
     │ ▭        ___         │ ← silver top band with soft XP gloss
     │       ╱  ◠◠◠  ╲      │
     │      │ sky ☁   │     │ ← lens glass shows a tiny sunny landscape:
     │      │ ⌒⌒hills │     │   blue sky, one cloud, green hill
     │       ╲_______╱      │   (no pupil, no centred catchlight)
     │                  •   │ ← small green "on" light
     ╰──────────────────────╯   Luna-blue body, rounded "squircle" corners
```

- **Body:** a rounded rectangle (corner radius 22 % of the height), 1.3 : 1 wide, **Luna blue**
  `#3A86F0 → #1A4FB8` with a silver top band `#F4F6F8 → #C9D0D8`. A slight XP 3/4 tilt (5° yaw), a soft
  drop shadow, and a 1 px darker outline, as XP icons have.
- **Lens:** a thick, friendly silver bezel (two rings). The glass shows a **miniature of the MyCam hills**:
  a sky gradient, a white cloud and a green hill. The camera looks *out at the world* with you, not at
  you. A gentle highlight arc sits along the top-left rim of the glass, never as a dot in the centre.
- **Warmth:** an amber shutter button on top and a small green status light on the front. Two warm accents
  against the blue keep it toy-like and approachable.
- **Small sizes:**
  - **16 px:** drop the viewfinder, shutter and light. Keep body, bezel, and lens as two colour blocks
    (sky over hill).
  - **24 and 32 px:** add the shutter.
  - **48 px and up:** full detail.
- **Name pairing:** "MyCam" in Trebuchet MS Bold (PC art) / DejaVu Sans Bold (phone), with a white text
  shadow on blue.

Alternatives explored and rejected:
- A phone with a camera bump: too generic, reads as "phone app".
- An instant camera: lovely, but busy at 16 px.
- A lens-only roundel: back to looking like an eye.

### 4.3 Status family (tray, settings, phone)

The badge sits bottom-right with a 2 px white outline, XP style (round, glossy, bold glyph):

| State | Treatment |
|---|---|
| Ready | Full colour, no badge |
| Streaming | **Green** badge with a white play triangle ("on air") |
| Paused | **Amber** badge with two white bars |
| Error | **Red** badge with a white × (XP's error glyph) |
| Not connected | The whole camera desaturated (grey body, grey hills in the lens), no badge |

### 4.4 Other icon surfaces

- **Android adaptive icon:**
  - Background layer: the sunny hills scene (sky, clouds, hills), drawn by script.
  - Foreground layer: the camera.
  - Monochrome (themed) layer: body silhouette, bezel ring, and the hill line inside the lens.
- **Notification icon:** a white camera outline with a hill line in the lens.
- **Play Store 512 px icon and feature graphic, installer wizard art:** regenerated with the new mark over
  the hills scene.

---

## 5. Phone redesign (no preview)

### 5.1 Information architecture

```
Header (XP title bar) ─ tabs: [ Now ] [ Settings ]      (XP property-sheet tabs)
Now       status card · Pause · camera switch · quick controls (while streaming)
Settings  task groups: Video · Wireless · Paired PCs · Phone · About
Overlays  Pairing dialog (modal) · First-run cards · Dim screen
```

### 5.2 "Now", portrait

```
┌────────────────────────────────────┐
│ ▣ MyCam                (Luna blue) │  ← title bar with gloss highlight line
│ ╭─────╮╭──────────╮                │
│ │ Now ││ Settings │                │  ← XP tabs: selected tab has the orange top line
├─┴─────┴┴──────────┴────────────────┤
│  task pane blue gradient            │
│ ╭─────────────────────────────────╮│
│ │ Streaming                (LIVE) ││  ← hero group: dark-blue header, white text
│ ├─────────────────────────────────┤│
│ │ [ big status illustration ]     ││  ← the camera, with its state badge (§4.3), 96 dp
│ │ Front camera · 1080p30 · USB    ││
│ │ ┌─────────────────────────────┐ ││
│ │ │ ▌▌ Pause the camera         │ ││  ← hero button, 56 dp, amber badge
│ │ │    Apps see "Camera paused" │ ││
│ │ └─────────────────────────────┘ ││
│ ╰─────────────────────────────────╯│
│ ╭─────────────────────────────────╮│
│ │ Camera                        ⌃ ││  ← normal group: white→lavender header
│ ├─────────────────────────────────┤│
│ │ (•) Back camera  ( ) Front      ││  ← XP radio buttons, green dot
│ │ Zoom   [−] [0.6×|1×|2×|5×] [+]  ││  ← only what this camera reports
│ │ Light  [−]  +0.3 EV  [+]        ││
│ │ Focus  (•) Auto ( ) Lock        ││
│ │ ☑ Torch (phone light)  [Reset]  ││
│ ╰─────────────────────────────────╯│
├────────────────────────────────────┤
│ ☾ Dim screen            Wi-Fi: on  │  ← bottom bar: real actions (A8)
└────────────────────────────────────┘
```

- **No preview on the phone.** The hero group shows the big status illustration instead. It's the new logo
  at 96 dp with the state badge, and it animates between states (§6.3).
- **Not streaming:** the Camera group collapses its controls to one line: "Zoom, brightness, focus and
  torch appear when an app uses the camera." The PC does the same (W11).
- **Applying a change** (quality, fps or camera switch takes ~1 s): a slim **green marquee bar** runs under
  the hero group's header until the phone reports the new mode (A14, §6.1).
- **Paused:** the hero header turns amber (`#E08A00 → #C06F00`, white bold text, 4.7 : 1) and the button
  becomes **"Resume the camera"** in **GoGreen**.

### 5.3 "Now", landscape

At medium width and up (`WindowSizeClass`), the hero group sits on the left (40 %) and the Camera group on
the right (60 %). That suits tripods, foldables and tablets.

### 5.4 Settings

Collapsible task groups. The chevron rotates and the group slides open (§6.3).

```
Video            Quality    (•) 720p  ( ) 1080p  ( ) 4K
                 Frame rate (•) 30    ( ) 60     ( ) 120 fps
                 60 fps needs 1080p or lower on this camera.   ← per-state reason (A10)
                 Now: 1920×1080 at 30 fps
Wireless (beta)  ☑ Use over Wi-Fi
                 This phone is 192.168.1.23. Turn on "Find phones on Wi-Fi" on the PC.
Paired PCs       DESKTOP-ABC  [Forget]   LAPTOP-XYZ  [Forget]   (A11)
Phone            ☑ Dim the screen automatically while streaming
About            Get MyCam for Windows (link + QR) · Version · Licences
```

The fps reason is computed from the masks of all qualities ("needs 1080p or lower", or "this camera does
30 fps"), so it stays capability-driven.

**Scan for PCs.** An XP push button in the Wireless group, and in the Now hero while Wi-Fi is on and nothing
is connected. The phone never probes (no protocol change): it lists the PCs whose "Find phones on Wi-Fi"
probes (`MYCAM?1 <name>`, every 2 s) it heard in the last 6 s, kept by `NearbyPcs` all the time wireless mode
is on. Pressing the button shows the green marquee and "Looking for PCs on this Wi-Fi…" for 6 s (three probe
intervals), then "N PCs on this Wi-Fi:" with one row per PC: a status dot, the name, its IP in subtle text and
"Connected", "Paired" (matched by name, best effort) or "Not paired yet: it will ask for a code when it
connects". With none: "No PC found. On the PC, open MyCam and turn on 'Find phones on Wi-Fi'…" plus the
phone's own IP on Now. With Wi-Fi off the button reads **Turn on and scan** and switches wireless mode on
(a user action, saved like the checkbox). The status line is a polite live region; with reduced motion the
marquee is left out and the static "Looking for PCs…" line stands in for "Working…".

### 5.5 Pairing dialog (A3)

A modal XP dialog (title bar, beige body) over a **blurred, dimmed** Now screen (§6.2):

```
┌ Pair with a PC ───────────────────────── ✕ ┐
│ 🛡  Allow DESKTOP-ABC to use this camera?   │
│                                            │
│    Check that the PC shows:                │
│              554 294                       │  ← 40 sp mono bold
│    If the codes differ, don't allow.       │
│                                            │
│    [▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░]  Waiting 0:42     │  ← green bar counts down the handshake timeout
│                                            │
│              [ Don't allow ]  [[ Allow ]]  │  ← Allow = default button (blue inner ring)
└────────────────────────────────────────────┘
```

TalkBack reads the code digit by digit. When the timer runs out, the dialog says "Pairing timed out"
instead of vanishing.

### 5.6 Dim screen

It stays nearly black for OLED and privacy, but becomes **the hills at night**: a navy sky
`#06142B → #000000`, a faint hill silhouette `#0B2414` along the bottom, and two or three dim stars. The
content still drifts every minute against burn-in. The LIVE pill is shown at 60 % opacity. Nothing
animates except the drift.

### 5.7 First run (A9)

Three cards on the hills background, shown once to a new user: Skip, Got it on the last card or the first
connection end them for good (`PREF_FIRST_RUN_DONE`), and a phone that already has a paired PC never sees them:
1. **Install MyCam on your Windows PC**: link and QR.
2. **Plug in this phone**: "Tap OK when Android asks. Tick *Always*."
3. **Pick "MyCam" as the camera in any app.**

Each card has a **Next** button in GoGreen. A step indicator shows three XP progress chunks, filled as you
go.

---

## 6. Effects: progress bar, glass, blur, motion (where, and only where)

### 6.1 The green progress bar

XP's segmented bar: rounded white track, `#ACA899` border, glossy green chunks (8 px wide with 2 px gaps on
the PC; 8 × 12 dp on the phone).

| Mode | Where | Behaviour |
|---|---|---|
| **Marquee** (indeterminate) | PC: "Phone found, connecting…", binding the driver, "Starting the camera". Phone: applying quality/fps/camera changes. Waiting picture (§7.2). | 3 chunks slide left to right, linear, 2.0 s per pass, 0.4 s pause, repeat |
| **Determinate** | Pairing timeout (both apps), first-run steps | Chunks fill in whole steps, never smoothly (that's the XP feel) |
| **Never** | Steady states (Ready, Streaming, Paused) | A bar that runs forever means "stuck" |

Implementation:
- **PC:** draw it in Direct2D. Don't use the comctl32 progress control, which renders Windows 11 style.
- **Phone:** a Compose `Canvas` with `rememberInfiniteTransition`.

### 6.2 Glass and blur

Allowed uses only:

| Where | Effect | Why | Implementation |
|---|---|---|---|
| Camera pictures (Paused, Waiting) | **Frosted glass card** over the hills: the background is blurred behind the card (σ 18 px at 1080p), with white at 55 %, a 1 px white 70 % rim, a 16 px radius and a soft shadow | Keeps text readable on a busy photo-like scene | Baked into the generated frames by `make_status_frames.ps1`; zero runtime cost |
| Camera pictures, whole background | Light blur (σ 3 px) on the hills | The scene recedes; the message comes forward | Baked |
| PC preview overlays (LIVE pill, "1080p · 30", "Mirrored") | **Frosted chips** over the video: Gaussian blur σ 8 of the area underneath, with a dark tint at 35 % | Labels stay readable on any picture | `ID2D1DeviceContext` + Gaussian blur effect (requires moving off `ID2D1HwndRenderTarget`, §8) |
| Phone pairing dialog backdrop | Blur of the screen behind it (radius 16 dp) + 40 % dim | Focuses attention on a security decision | `Modifier.blur` on API 31+; dim only on API 24–30 |
| Title bars (both apps) | The XP **gloss highlight**: a 1 px light line and a lighter top 8 % | That's what XP's chrome is | Gradient stops (§3.2) |

**Not** on body surfaces, cards, buttons, lists or the tray. Everything else stays flat XP.

### 6.3 Motion

| Where | Motion | Timing |
|---|---|---|
| Status change (icon, headline, badge) | Crossfade; the badge pops in (scale 0.6 → 1.0 with a slight overshoot) | 200 ms, ease-out; badge 250 ms |
| LIVE pill when streaming starts | A one-shot green glow sweep across the pill (XP gloss passing over it) | 600 ms, once; **no looping pulse** |
| Task group expand and collapse | Height and opacity slide; chevron rotates 180° | 180 ms, ease-in-out |
| Tabs (phone) | The orange top line slides to the selected tab; content crossfades | 150 ms |
| Option selection | Radio dot scales in; selected chip fills | 120 ms |
| Button press | XP pressed state (contents shift 1 px down and right), plus a light haptic on the phone | Instant |
| Pairing dialog | Fades in and rises 16 dp; backdrop blur fades in | 220 ms, ease-out |
| PC preview | First frame fades in from the status art; on disconnect, fades to the status art | 300 ms |
| PC window open | Fades in from 96 % scale | 150 ms |
| Camera restart (~1 s) | The green marquee bar (§6.1) | Until the phone reports |
| Dim screen | Burn-in drift only | 1 step per minute |

**Reduced motion** turns every transition into an instant change and replaces the marquee with a static
"Working…" label:
- **Phone:** when `Settings.Global.ANIMATOR_DURATION_SCALE` is 0, or TalkBack is on.
- **PC:** when `SPI_GETCLIENTAREAANIMATION` is off.

**Never:**
- Looping decorative animation in steady states.
- Parallax.
- Animated backgrounds in the apps.
- Animation in the tray icon.

---

## 7. Camera pictures (what apps see)

### 7.1 "Camera paused"

```
┌──────────────────────────────────────────────────────────────┐
│  blue sky, soft clouds                                       │
│                ╭──────────────────────────────╮              │
│                │      [camera + ▌▌ badge]     │              │  ← frosted glass card
│                │       Camera paused          │              │     (§6.2), centred slightly
│                │  Video is turned off for now.│              │     above the horizon
│                ╰──────────────────────────────╯              │
│     ⌒⌒⌒⌒   rolling green hills   ⌒⌒⌒⌒⌒⌒⌒⌒⌒⌒⌒                    │
│ ▣ MyCam                                                      │  ← small wordmark, bottom left,
└──────────────────────────────────────────────────────────────┘     white with a soft shadow
```

- **Background:** sunny rolling hills under a blue sky with cumulus clouds, as in the reference photo. The
  horizon sits at about 58 % height. Two or three overlapping hills in yellow-green to deep green
  (`#9BD640` / `#5DB82E` / `#2F8A1F`) with soft light on the crests. The sky goes from `#2E7FE0` at the top
  to `#9CC8F2` at the horizon.
- **Card:** frosted white glass. The title "Camera paused" is in Trebuchet MS Bold 64 px `#1D3F8A`, the
  subtitle in Tahoma 28 px `#33475B`, and the new logo with its amber pause badge sits at 140 px.
- **Still image:** no animation. Paused should feel calm and settled.
- **"Windows is locked"** reuses this picture, as today.
- **Sizes:** rendered at 1920 × 1080, 1280 × 720 and 3840 × 2160, so every quality gets a crisp frame.

### 7.2 "Waiting for the phone"

The same hills, at a **softer early-morning tint**: the sky is a little paler and the hills slightly less
saturated, so the two states are told apart at a glance in a call grid. The card contains the logo
(desaturated), "Waiting for the phone", "Connect your phone with a USB cable, or over Wi-Fi, and open
MyCam", and **the green marquee bar** under the text.

The bar is drawn per frame by the virtual camera. It's three small rectangles composited onto the NV12
frame at ~15 fps, which is a negligible cost. If that turns out costly in Frame Server, fall back to a
static, half-filled bar.

### 7.3 Licensing the background

The reference photo lives in `inspo/`, which is never committed, and its source isn't recorded. **Plan:**
paint an original hills-and-sky scene procedurally in `design/tools/make_status_frames.ps1`:
- Bézier hill silhouettes with vertical gradients and soft crest highlights.
- Grass texture as low-contrast noise.
- Clouds as clustered soft radial blobs with a lighter top.

The photo is used **only** as a colour and composition reference. If the owner has a licence for a specific
photo, it can go in `design/assets/` with its source and licence noted instead.

---

## 8. Windows redesign

### 8.1 Window

Retitle it **"MyCam"**. The window is **720 × 500 DIP**, which fits a 720 DIP work area with the taskbar.
It has the XP silhouette: a Luna title bar with rounded top corners, a red close button and a blue minimise
button.

```
┌ ▣ MyCam ─────────────────────────────────────────────────── _ ✕ ┐  ← Luna title bar
│╭ task pane (blue gradient) ─╮┌───────────────────────────────────┐│
││╭ Now (hero) ─────────────╮ ││                                   ││
│││ ✓ Streaming   (LIVE)    │ ││        live preview 16:9          ││
│││ Back camera · USB       │ ││                                   ││
│││ [ ▌▌ Pause the camera ] │ ││ (LIVE)              1080p · 30    ││ ← frosted chips (§6.2)
││╰─────────────────────────╯ │└───────────────────────────────────┘│
││╭ Camera ────────────── ⌃ ╮ │ Choose "MyCam" as the camera in     │
│││ (•) Back  ( ) Front     │ │ any app.                            │
││╰─────────────────────────╯ │                                     │
││╭ Video ─────────────── ⌄ ╮ │ Zoom       [−]  1.0×  [+]  [1×]     │ ← controls for the
││╭ Picture ───────────── ⌄ ╮ │ Brightness [−] +0.3 EV [+]          │   live picture sit
││╭ Wi-Fi and startup ──── ⌄ ╮│ Focus (•) Auto ( ) Lock  ☑ Torch    │   right under it
││  Reconnect phone           │                         [ Auto ]    │
││  Open log folder           │                                     │
│╰────────────────────────────╯                                     │
└───────────────────────────────────────────────────────────────────┘
```

- **Left: XP task pane** (blue gradient), 240 DIP wide, scrolls when groups are expanded.
  - The **Now** hero group holds status and **Pause** (same component and weight as the phone, fixing W8).
  - Then collapsible **Camera**, **Video**, **Picture** and **Wi-Fi and startup** groups.
  - **Task links** at the bottom ("Reconnect phone", "Open log folder", "Forget Wi-Fi phones") are stacked,
    never crowded on one row (fixes W6).
- **Right: preview and live controls** on the beige surface. Zoom, brightness, focus and torch sit right
  under the picture they change. Before the camera reports, they're replaced by one grey line (fixes W5 and
  W11).
- **Layout engine:** replace the hand-typed coordinates with a small row/column helper that measures
  DirectWrite text, so text reflows with Windows "Text size" (fixes W7).
- **Rendering:** move from `ID2D1HwndRenderTarget` to `ID2D1DeviceContext` with a swap chain, for the blur
  effect, the preview bitmaps and smooth transitions.

### 8.2 Preview (W3)

- While the window is visible, take every Nth decoded NV12 frame, scale it to 400 × 225 on the GPU (a
  D2D YCbCr effect) and draw it at **≤ 15 fps**. It stops when the window is hidden or minimised.
- The preview shows **what apps receive**: mirror and fill applied.
- With no stream, it shows the same Paused or Waiting picture that apps see (§7), so it's never an empty
  box.
- Frosted overlay chips (bottom-left): **LIVE** (GoGreen) and the mode ("1080p · 30"), plus "Mirrored" when
  mirroring is on.

### 8.3 Pairing dialog (W4)

An XP-styled modal window (custom-drawn like the main window, with UIA, see §8.4):
- **Content:** "Pair with Pixel 8?", the code in Trebuchet MS Bold 24 pt, and "Tap Allow on the phone if it
  shows the same code".
- **Countdown:** the **green determinate bar** counts down the handshake timeout.
- **Cancel pairing:** the PC just closes the socket, so no protocol change.
- **Closing:** it closes by itself when the phone answers. The phone name gets the ellipsis, never the code.
- **Notification:** the tray balloon "MyCam wants to pair with Pixel 8" opens this dialog when clicked.

### 8.4 Accessibility (W2, W9)

- A **UI Automation provider**, one fragment per element:
  - Option groups as `Selection` / `SelectionItem`.
  - Checkboxes as `Toggle`.
  - Links and buttons as `Invoke`.
  - Task groups as `ExpandCollapse`.
  - The status line as a live region, so Narrator says "Streaming" or "Paused".
- **Keyboard:**
  - Tab moves between groups; arrows move within a group.
  - Space toggles; Enter activates.
  - Access keys: Alt+P Pause, Alt+B / Alt+F camera, Alt+M Mirror. Underlines appear only with keyboard cues.
- **High contrast:** draw flat system colours with no gloss, blur or animation.

### 8.5 Tray

- Show the new status icons (§4.3).
- Put a check mark on the active camera (W12), and add a "Quality ▸" submenu greyed by capability.
- Keep the native menu: it's accessible, and Windows 11 renders it consistently.
- **Scan for phones** (§8.6) opens the window and starts a scan.

### 8.6 Scan for phones

An XP button **Scan for phones** (Alt+S) sits in the Wi-Fi and startup group, and also in Now while no
phone is connected. It runs a one-shot scan: the PC broadcasts the usual discovery probe right away and
lists every phone that answers within **6 s**, even while "Find phones on Wi-Fi" is off (the setting doesn't
change). While it runs, the green **marquee** shows with "Looking for phones on this Wi-Fi…" (reduced motion:
the sentence alone). Then each phone gets a row: its name, its address in subtle text, and "Connected",
"Paired" or "New: it will ask for a code", with a **Connect** link while no phone is connected. With search
off, nothing connects by itself: Connect runs one Wi-Fi session to that phone (pairing with the dialog as
usual). With search on, auto-connect works as before and the scan only refreshes the list sooner. A plugged-in
phone still wins. If nothing answers: "No phone found. On the phone, open MyCam and turn on “Use over
Wi-Fi”. Both must be on the same Wi-Fi." The results show in Now while no phone is connected, else in the
Wi-Fi group. Everything goes through `ui_model.h` (ids 206, 603–606, rows from 1000), so the UIA provider
exposes it: the buttons and Connect links as Invoke, the list as a Group of Text rows, and the result
sentence as a live region ("Found 2 phones", "No phone found"). "Paired" is matched by name against
`HKCU\Software\MyCam\PairedPhones` (the discovery answer carries no phone id), so it's best effort. Demos:
`--demo=scanning`, `scan-results`, `scan-none`, `scan-wifi`.

---

## 9. Licensing and assets

- **Nothing from Microsoft is shipped:** no Bliss, no Luna bitmaps, no XP icons, no Tahoma or Trebuchet
  files.
- **PC:** the PC *uses* Tahoma and Trebuchet MS, which ship with Windows 11, and falls back to Segoe UI if
  they're missing.
- **Phone:** ships **DejaVu Sans** (Condensed, Bold, Mono), under the Bitstream Vera licence plus public
  domain changes. Add its licence to `app/src/main/assets/licenses`. Selawik is removed.
- **Scripts:** all art is generated by scripts. Retarget `design/tools/aero_draw.ps1` → **`luna_draw.ps1`**
  (title bar, chunks, badges, frosted card, hills painter). Update `make_icons.ps1` (new mark),
  `make_status_frames.ps1` (hills + glass + three sizes), `make_installer_art.ps1` and `make_store_art.ps1`.
  Regenerate with them rather than hand-editing PNGs.
- **Docs:** when this lands, update CLAUDE.md "Design language", IMPROVEMENTS.md 7.1 (a new decision: XP
  replaces Aero) and 7.2, and write `design/STYLE.md` from §3 and §6.

---

## 10. Copy changes

| Where | Today | Proposed |
|---|---|---|
| Phone, disconnected | "Plug this phone into a PC running MyCam." | "Plug this phone into your PC. Need MyCam for Windows? See **Get it**." |
| Phone `controls_unknown` | "Zoom, exposure, focus and torch appear…" | "Zoom, brightness, focus and torch appear when an app uses the camera." |
| Phone torch | "Torch (light)" | "Torch (phone light)" |
| Phone fps hint | generic | per-state reason (§5.4) |
| PC title | "MyCam Settings" | "MyCam" |
| PC pairing headline | "Pair with X? Code 554 294" | "Pairing with X…" + pairing dialog (§8.3) |

---

## 11. Roadmap

| Phase | Item | Findings | Size |
|---|---|---|---|
| **1. Foundations** | XP tokens and type in `Color.kt` / `Type.kt` and the PC palette; DejaVu fonts | — | S |
| | Compose semantics (toggleable, selectable, descriptions) while rebuilding the components | A2, A13 | S |
| | XP component kit: title bar, tabs, task group, buttons, radio/checkbox, progress bar | — | M |
| **2. Brand** | New logo and status family; Android adaptive / themed / notification icons; tray icons | §4 | M |
| | Hills painter + frosted card; new Paused and Waiting frames in 3 sizes | §7 | M |
| | Waiting-frame marquee in the vcam | §7.2 | S |
| **3. Phone** | Now / Settings tabs, hero group, collapsible task groups, bottom bar | A4, A8, A12 | M |
| | Pairing dialog with blur and countdown | A3 | S |
| | Landscape two-pane, first run, per-PC forget, fps reasons, night Dim screen | A5, A9–A11 | S each |
| **4. PC** | Device-context renderer + layout helper; 720 × 500 task-pane window | W1, W5–W8, W10, W11 | L |
| | Preview with frosted chips | W3 | M |
| | Pairing dialog with Cancel and countdown | W4 | M |
| | UI Automation, grouped keyboard nav, high contrast | W2, W9 | L |
| | Tray check marks and quality submenu | W12 | S |
| **5. Polish** | Motion pass (§6.3) with reduced-motion support on both apps | A14 | M |
| | Installer and store art, STYLE.md, docs | §9 | S |

Phases 1–2 give the new look and logo everywhere quickly. Phase 4 is the largest piece (renderer and UIA)
but fixes the most serious PC issues.

---

## 12. Decisions (owner, 2026-10-10)

| # | Question | Decision |
|---|---|---|
| 1 | Pause background | **An original hills-and-sky scene painted by script.** The owner's photo is a colour and composition reference only; it is never shipped. |
| 2 | XP colour scheme | **Luna Blue only.** |
| 3 | Dark mode | **None.** The apps stay light; only the Dim screen is dark (the night hills). |
| 4 | Phone fonts | **DejaVu Sans** (Condensed for body, Bold for titles, Mono for the pairing code). Selawik is removed. |
