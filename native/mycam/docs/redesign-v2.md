# MyCam redesign v2: matching the designer's mockup

An analysis of the mockup the owner's designer supplied (`design/previews/designer-mockup.png`), compared
with what MyCam has today on branch `xp-redesign` (1.4.0), and the work needed to match it.

Companion documents: [redesign.md](redesign.md) (the XP spec we built) and
[design/STYLE.md](design/STYLE.md) (the token rulebook). Where this file and those disagree, **this file
wins** for anything the mockup actually shows; redesign.md still governs everything the mockup is silent
about.

---

## 1. What the mockup is

Ten screens on a Bliss-style hills background: four Windows windows and six phone screens.

**Windows:** 1. Home (connect a device) · 2. Device discovery · 3. Pairing · 4. Camera control with a live
preview.

**Phone:** 1. Splash · 2. Connection · 3. Scan for devices · 4. Pairing · 5. Live preview ·
6. Camera settings.

### 1.1 The single most important observation

The mockup is **XP on the outside, modern flat on the inside**.

- **Kept from XP/Luna:** the blue gradient title bar, the three square caption buttons with the red close
  box, the hills-and-sky imagery, the overall friendliness.
- **Not XP at all:** the window interior. It uses a left **sidebar nav**, white **rounded cards** with soft
  shadows, flat line icons, **dropdowns** (combo boxes), **toggle switches**, a **slider**, and a single
  blue accent (`#1E6FE8`-ish) for every action. There are no beige `#ECE9D8` surfaces, no task-pane
  gradient, no green check marks, no glossy bevelled buttons, no green progress bar.

So this is not "more XP". It is **XP chrome wrapped around a clean modern app**. That is a deliberate and,
in my view, good choice: the XP shell carries the personality, while the controls stay legible and modern.
But it means a large part of what we built in the last pass changes.

### 1.2 What this costs

| Built in this pass | Mockup's position |
|---|---|
| XP task-pane groups (collapsible, lavender) | Replaced by sidebar nav + white cards |
| Green check boxes and radio dots | Replaced by toggle switches and dropdowns |
| Glossy bevelled XP push buttons | Replaced by flat blue/white rounded buttons |
| Beige `#ECE9D8` window body | Replaced by white / very light grey |
| Green segmented progress bar | Not shown anywhere; a radar animation appears instead |
| Zoom presets (0.6× / 1× / 2× / 5×) | Replaced by a continuous slider |
| Phone Now/Settings tabs | Replaced by a 3-item bottom nav (Connect / Camera / Settings) |
| **No preview on the phone** (owner's earlier decision) | **The phone shows a live preview** (screen 5) |

The last row is a direct reversal of a decision the owner made earlier in this project, so it needs a
decision rather than an assumption. See §6.

---

## 2. Windows app: what changes

### 2.1 Navigation: sidebar replaces the task pane

The mockup's window is **two columns**: a ~150 px white sidebar and a content area.

- Sidebar items: **Home · Devices · Camera · Settings · About**, each a line icon plus a label, with the
  selected row as a filled blue rounded pill with white text.
- The content area shows one page at a time, with a page title and a subtitle
  ("Connect a Device / Find and connect to your phone").

Today we have one scrolling task pane with every group stacked in it. The change is structural: the window
becomes a small **page router**, with the element model and the UI Automation tree rebuilt per page.

**What this fixes:** our window is dense. Five groups plus a preview plus controls in 720 × 500 is a lot.
Pages make each screen calm.

**What this costs:** more clicks to reach a setting, and the UIA tree changes shape per page (the provider
already rebuilds from `ui_model.h`, so this is mostly layout work).

### 2.2 Page 1, Home

Two large white cards, each with an icon, a title, a one-line description and a primary button:
- **USB Connection** — "Connect your phone via USB for best performance." → `Scan for USB Devices`
- **Wi-Fi Connection** — "Both devices must be on the same Wi-Fi network." → `Scan for Devices`

This is a genuinely better first-run surface than ours, which drops the user straight into status plus every
control. New work: a USB scan action (we only ever scan Wi-Fi; on USB we wait for a plug-in, so "Scan for
USB Devices" maps to a re-enumerate plus the existing `Reconnect phone`).

### 2.3 Page 2, Devices

- Header "Devices" with a **Refresh** button at the right.
- Two sections, each a heading over its own card: **Connected Device** and **Available Devices**
  (owner's note, 2026-10-11: the phone in use must not be mixed into the list of ones to pick from). The
  connected section always shows, saying "No device is connected yet." when there is none, so the state is
  stated rather than left to be inferred.
- Rows: a phone glyph, the device name in bold, the IP beneath it, a **transport icon** (Wi-Fi arcs or the
  USB trident) and a blue **Connect** button.
- "Scan for Devices" on Home lands here, because this is where its results are.

We built almost exactly this in "Scan for phones" (name, IP, status, Connect link). Changes needed:
a dedicated page, the transport icon, Connect as a **button** rather than a link, and USB devices listed in
the same list.

### 2.4 Page 3, Pairing

A centred column: a phone illustration, "A pairing request has been sent to your phone. Make sure the code
below matches on both devices.", the **code in a large bordered box**, and a **Cancel** button.

Ours is a separate modal dialog window. The mockup makes it a **page inside the main window**. The page
form is calmer and avoids the owner-window problems we already had to fix. I recommend keeping a dialog
**only** when the main window is closed (the pairing request must still be reachable from the tray), and
otherwise showing this page.

Our countdown bar is not in the mockup. Keep it: a 60-second window with no visible timer is worse. Render
it as a thin flat blue determinate bar under the code, not the XP green chunks.

### 2.5 Page 4, Camera

- **Left:** the live preview, large, with three dark pill chips along the bottom: `1920 × 1080`, `60 FPS`,
  `Back Camera`.
- **Right:** a settings column — **Camera** (dropdown), **Resolution** (dropdown), **Frame Rate**
  (dropdown), **Zoom** (slider with `1.0x` to the right), **Auto Focus** (toggle), **Torch / Flash**
  (toggle).

Changes from ours: radio groups → dropdowns; the zoom −/+/preset row → a slider; check boxes → toggles; our
frosted LIVE chip → three flat dark chips.

**Keep from ours, because the mockup cannot show it:** every option must still come only from what the phone
reports. A dropdown must **omit or disable** entries the phone cannot do (4K on a phone without it, 120 fps
at a quality that cannot do it) and must carry the one-line reason we already compute
("120 fps works at 720p on this camera"). A dropdown hides that reason, so it goes under the control.

The mockup has no Pause control on this page. **Pause must stay** — it is the privacy promise of this
product. Put it under the preview, or in the sidebar as a persistent item.

### 2.6 Window chrome

The mockup shows **minimise, maximise and close**. Ours has no maximise and is fixed at 720 × 500. The
mockup's window is wider (roughly 4:3) and must stay resizable if maximise exists. That means a real
resizable layout, which the layout helper already supports but the fixed `kW`/`kH` constants do not.

---

## 3. Phone app: what changes

### 3.1 Navigation: a 3-item bottom bar

**Connect · Camera · Settings**, with the active item in blue. Ours has two top tabs (Now / Settings) and a
bottom utility bar. The mockup's split is clearer: *connecting* and *using* are different jobs.

### 3.2 Screen 1, Splash

Full-bleed hills, the app icon, "MyCam", "Turn your phone into a webcam", and a blue **Get Started** button.
We have no splash. This replaces our 3-card first-run guide as the first impression; the cards can remain
behind Get Started, or disappear entirely in favour of the Connect screen, which is self-explanatory.

Note: the owner has just asked that first-run never repeat. A splash that shows **every** launch is fine
(it is 1 s of branding), but it must not block, and the setup cards must still appear only once.

### 3.3 Screen 2, Connect

"Connect to PC / Choose how you want to connect.", then three tappable rows:
- **Scan QR Code** — "Pair using code from PC"
- **Find Devices** — "Search on the same Wi-Fi"
- **USB Connection** — "Connect via USB cable"

**Scan QR Code is a new feature.** It is the single biggest functional addition in the mockup: the PC shows
a QR code, the phone's camera reads it, and pairing completes without typing or comparing six digits. See
§5.

### 3.4 Screen 3, Scan for devices

A **radar animation**: concentric rings with a sweep and dots for found devices, "Scanning for devices…",
the hint "Make sure your PC app is open and both devices are on the same Wi-Fi network.", then result rows
(`DESKTOP-SAIF`, `192.168.1.12`).

Ours shows the green marquee instead. The radar is friendlier and reads as "looking around", which is what
discovery is. It also honours reduced motion poorly, so keep a static fallback.

### 3.5 Screen 4, Pairing

A monitor illustration, "Pair with this device?", the device name, "Make sure the code below matches on your
PC.", the code, then **Reject** (plain) and **Accept** (blue filled).

Ours is close. Differences: an illustration instead of a shield, "Reject/Accept" instead of
"Don't allow/Allow", and no countdown shown. Keep the countdown.

**Wording:** "Don't allow / Allow" is the better pair. "Reject" sounds like the device is at fault, and
Android's own permission language is allow/don't allow. I would keep ours and adopt everything else.

### 3.6 Screen 5, Live preview

The camera preview fills the screen, with a **Front Camera | Back Camera** segmented control and three stat
tiles beneath it: `1080p / Resolution`, `60 / FPS`, `1.0x / Zoom`.

This reverses the "no preview on the phone" decision. See §6.

### 3.7 Screen 6, Camera settings

Rows with the control on the right: **Resolution** (dropdown), **Frame Rate** (dropdown), **Zoom** (slider +
`1.0x`), **Auto Focus** (toggle), **Torch / Flash** (toggle). Same mapping as the PC page, so the two apps
finally use identical controls for identical settings — which is what §4 of redesign.md asked for and we
only half-achieved.

---

## 4. Visual system: the deltas to apply

| Token / component | Today | Mockup |
|---|---|---|
| Window body | `#ECE9D8` beige | White `#FFFFFF`, page background near-white `#F5F7FA` |
| Card | flat lavender group body | White, radius ~10, 1 px `#E3E8EF` border, soft shadow |
| Primary action | glossy XP button | Flat blue `#1E6FE8` → `#1760D0`, white text, radius 6 |
| Secondary action | glossy XP button | White fill, `#D0D7E2` border, dark text |
| Selection in nav | n/a | Blue filled pill, white text + icon |
| Choice of value | green radio group | **Dropdown** (combo box) |
| On/off | green check box | **Toggle switch** (blue when on) |
| Continuous value | −/+ buttons and presets | **Slider**, value label to the right |
| Progress | green XP chunks | **Keep the green XP chunks** (owner's decision, 2026-10-10). The mockup shows no progress bar; ours stays as it is, including the marquee and the pairing countdown. The phone's scan screen still gets the radar. |
| Preview overlay | frosted glass chips | Flat dark pills at ~70 % black, white text |
| Icons | glossy XP badges | Flat line icons, 1.5 px stroke, in the blue accent or grey |
| Title bar | Luna gradient | **Unchanged** (keep ours) |
| Caption buttons | ours | **Unchanged** (keep ours, but add maximise) |

**Typography:** the mockup uses a plain modern sans (Segoe-like). Our Tahoma/Trebuchet choice is the XP
tell. Recommendation: keep **Trebuchet MS Bold for the title bar only**, and move body text to **Segoe UI**
on the PC. On the phone, DejaVu Sans stays (it is already neutral enough and is licence-safe).

**Accessibility, unchanged and non-negotiable:** every pair must stay at WCAG AA. White on `#1E6FE8` is
4.6 : 1, which passes. Toggle switches need a visible off state (grey track with a border), not just colour.
Dropdowns and sliders must be keyboard operable and exposed through UI Automation
(`ExpandCollapse` + `Selection` for a dropdown, `RangeValue` for a slider, `Toggle` for a switch).

---

## 5. New features the mockup implies

| Feature | Where | Effort | Notes |
|---|---|---|---|
| **QR code pairing** | Phone screen 2, PC pairing page | **L** | The PC renders a QR of its address, port and a pairing token; the phone's camera decodes it. Needs a QR encoder on the PC and a decoder on the phone (ML Kit barcode, or ZXing to avoid a Play dependency). It also needs a protocol note, since the QR must carry enough to skip the six-digit compare safely: the PC's public-key fingerprint, not just the IP. **This is a security-relevant change and must be designed in PROTOCOL.md before it is built.** |
| **USB device scan** | PC Home + Devices | S | Re-enumerate rather than waiting passively. |
| **Sidebar page router** | PC | M | Including the UIA tree per page. |
| **Dropdowns, toggles, sliders** | Both apps | M | New components in both kits, with keyboard and UIA support. |
| **Splash screen** | Phone | S | |
| **Radar scan animation** | Phone | S | With a static reduced-motion fallback. |
| **Stat tiles** | Phone preview | S | |
| **Phone live preview** | Phone screen 5 | M | Pending the §6 decision. |
| **Resizable / maximisable PC window** | PC | M | Only if maximise is kept. |

---

## 6. Decisions (owner, 2026-10-10)

| # | Question | Decision |
|---|---|---|
| 1 | Supplied images | **Generated by the owner with ChatGPT**, so MyCam owns the output and may ship it. No attribution needed. Recorded in `design/assets/source/README.md`. |
| 2 | Live preview on the phone | **Yes, with an on/off toggle**, and **always off while the screen is dimmed**. This replaces the earlier "no preview on the phone" decision. |
| 3 | Progress bars | **Keep the green XP chunks.** Don't switch to a flat blue bar. |
| 4 | Logo | **Keep the current "Snap" logo.** The job is to use it *consistently and correctly* everywhere (§11). |
| 5 | Motion | **Polish every animation**, and give both apps **smooth page transitions** (§12). |

### 6.1 The phone preview, in detail

- A **Preview** toggle lives on the phone's Camera screen and in Settings, saved across launches.
- **Default: on.** The designer made it a main screen, and a webcam app that cannot show you the shot is
  hard to defend. The thermal case is handled by the two rules below rather than by hiding the feature.
- **Always off when the screen is dimmed.** The dim screen exists to keep the phone cool and private, so no
  preview surface runs behind it, and waking the screen brings the preview back if the toggle is on.
- **Off when the app is in the background**, as today.
- The preview is a second output of the existing capture session, so it never changes what the PC receives
  and never restarts the camera. If the phone reports a **severe** thermal state, the preview turns itself
  off and says why; the stream is never sacrificed for it.

---

## 7. New artwork supplied

Two images the owner supplied and states are free to use. Both are now in the repo:

| File | Use |
|---|---|
| `design/assets/source/hills-photo.png` (1536 × 1024) | The new background: splash, first run, mockup backdrop, and the base for the camera pictures |
| `design/assets/source/camera-paused-reference.png` (1536 × 1024) | The new "Camera paused" picture: the same hills with a frosted glass card, a crossed-out camcorder glyph and "CAMERA PAUSED" |

**How these change the art pipeline.** Today `make_status_frames.ps1` **paints** the hills procedurally,
because the earlier reference photo had no known licence. With a photo that is free to use, the scripts
should composite over the photo instead, which looks far better than anything drawn with GDI+ gradients.

Work needed:
1. Record the source and licence of both images in `design/assets/source/README.md`. The owner states they
   are free to use; the actual licence name and origin should be written down before release, because the
   repo is MIT and the images ship inside the APK and the installer.
2. Rework `make_status_frames.ps1` to composite: downscale the photo to each target size, blur it slightly,
   draw the frosted card, then the glyph and the text.
3. Render the frames at the sizes the apps need (1280 × 720 today; 1920 × 1080 is worth adding).
4. Match the supplied paused screen: a **crossed-out camcorder** glyph rather than our round amber pause
   badge, "CAMERA PAUSED" in caps, letter-spaced, centred. Note the supplied image has **no** subtitle and
   no wordmark; I recommend keeping a small "MyCam" wordmark bottom-left so a viewer on a call knows which
   app paused.
5. The waiting picture keeps the same treatment with its own text and the progress bar.
6. Use the photo for the phone splash and first-run background, replacing the Compose-drawn hills. The
   night Dim screen stays drawn (a dark photo would be wasted bytes).

**Size:** the photo is 2.7 MB as PNG. Ship **JPEG** at the sizes needed (a 1920 × 1080 JPEG at quality 85 is
roughly 250 KB) and keep the PNG as the source only. The APK is currently ~2.8 MB, so this matters.

---

## 8. Fix: the transparent flash when the PC window opens

**Owner-reported, and now fixed on `xp-redesign`.**

**Cause.** The window is created with `WS_EX_NOREDIRECTIONBITMAP` and draws through DirectComposition, so it
has no surface of its own: until the swap chain holds a frame, there is nothing to show. The code called
`ShowWindow` immediately after creating the window and only painted on the first `WM_PAINT`, so for one or
two frames an empty, see-through window was on screen. On top of that, the opening animation faded the
content in from **zero** opacity over 150 ms, which stretched the empty look into something clearly visible.

**Fix** (`settings_window.cpp`):
1. Draw and present one complete frame **before** `ShowWindow`.
2. Remove the 150 ms fade-from-transparent open animation. The DWM already animates a window opening, so
   the extra fade only delayed the first readable frame.

The window now appears fully drawn on its first frame. The same pattern applies to the pairing dialog if it
is ever seen to flash.

---

## 9. Recommended order

| Phase | Work | Size |
|---|---|---|
| **0. Done** | The open-flash fix (§8); the supplied images added to the repo | — |
| **1. Art** | Licence note; composite the camera pictures over the photo; new paused design; splash and first-run background; JPEG sizing | M |
| **2. Component kit** | Dropdown, toggle switch, slider, flat buttons, white cards, dark preview chips, flat blue progress — on **both** platforms, with keyboard and UIA support | M |
| **3. Windows shell** | Sidebar page router; Home, Devices, Camera pages; pairing as a page; maximise/resize if kept | L |
| **4. Phone shell** | Bottom nav; splash; Connect screen; radar scan; stat tiles; settings rows | M |
| **5. Preview on phone** | Only after the §6 decision | M |
| **6. QR pairing** | PROTOCOL.md design first, then PC encoder and phone decoder | L |

Phases 1 and 2 are worth doing regardless of how much of the shell changes, because they are what makes the
app *look* like the mockup. Phase 3 is the largest single piece.

---

## 10. What I would push back on

Honest notes, since the mockup is a design and not a specification:

1. **Pause is missing** from every mockup screen. It is the product's privacy promise and must be visible on
   both apps, not buried.
2. **No status surface.** The mockup never shows "Streaming / Paused / Not connected". Our status model is
   the one thing users read at a glance, and it should stay on the PC Home page and the phone Connect
   screen.
3. **Dropdowns hide capability.** Radio groups made "this phone cannot do 4K" visible at a glance. With
   dropdowns, keep the unavailable entries visible but disabled, with the reason underneath. The dropdown
   itself therefore stays **openable even with no phone connected** (every entry greyed, and a line saying
   "Connect a phone to choose. Each phone reports which of these it can do."): a control that refuses to
   open shows nothing at all, which is worse than showing a list that cannot be used yet.
4. **"Reject"** should be "Don't allow", to match Android's own language.
5. **The radar animation** must have a reduced-motion fallback.
6. **Maximise** on a fixed-layout window is a trap. Either make the layout genuinely resizable or drop the
   maximise button.

---

## 11. Logo: one mark, used correctly everywhere

The "Snap" logo stays as it is. The work is consistency: today the same mark is drawn by three different
pipelines, and the mockup shows a fourth treatment (a rounded-square app tile with a thick lens).

**Where the logo appears, and what each place must use:**

| Surface | Source today | Must be |
|---|---|---|
| Windows tray (5 states) | `tray_*.ico` | The mark + its state badge, hinted at 16 / 20 / 24 / 32 px |
| Windows window caption | `app.ico` at 16 px | The same 16 px artwork as the tray's base state |
| Windows window status area | `status_*` icon at 48 px | The same mark, same proportions |
| Windows installer / Add-Remove | `app.ico` | Same |
| Camera pictures (paused, waiting) | drawn into the frame | Same mark, not a redrawn variant |
| Android launcher (adaptive, legacy, round) | `mipmap-*` | Same mark; the adaptive foreground must sit inside the 66 dp safe zone so launcher masks never clip the lens |
| Android notification | `ic_stat_webcam.xml` | A white silhouette of the *same* shape |
| Android in-app header | `app_icon` drawable | The same mark, not the launcher PNG scaled down |
| Android splash (new) | — | The same mark, large |
| Play Store icon and feature graphic | `design/assets/store/` | Same |

**Rules:**
1. **One generator.** Every raster above comes from one function in `design/tools/luna_draw.ps1`. No
   hand-edited PNGs, and no second drawing of the mark anywhere in app or companion code.
2. **Same silhouette at every size.** The small sizes simplify (drop the shutter, then the light), but the
   body proportions, corner radius and lens position never shift, so the icon does not appear to "jump"
   between the tray and the window.
3. **Badges are consistent too:** always bottom-right, always the same proportion of the icon, always with
   the 2 px white outline.
4. **The in-app headers must use the mark, not a screenshot of it.** Check that the phone header, the PC
   caption and the status area all pull from the generated assets.
5. **Audit before release:** put every surface side by side in `design/previews/icons.png` and look at it.

---

## 12. Motion: polish and page transitions

The owner asked for every animation to feel smooth, and for page transitions on both apps.

**Principles**
- **Nothing janky is better than something fast.** Animate only compositor-cheap properties: opacity and
  transform. Never animate layout.
- **One timing system.** Short 120 ms, standard 200 ms, page 250 ms, all on the same ease-out curve
  (`cubic-bezier(0.2, 0, 0, 1)`-ish). No animation runs longer than 300 ms.
- **Interruptible.** A transition that is interrupted retargets from where it is, and never snaps.
- **Frame-rate aware.** The PC drives animation from the real frame clock, not a fixed timer tick, and
  stops the timer the moment nothing is moving. The phone uses Compose's animation clock.
  Concretely, on the PC (both are needed; either alone still judders):
  - the clock is `QueryPerformanceCounter` (`xp::NowMs`), never `GetTickCount64`, whose ~15.6 ms step is
    coarser than a frame, so neighbouring frames would read the same time and the tween would stand still;
  - while anything is moving the window repaints as soon as the last frame was presented, because
    `Present(1, 0)` has already waited for the vertical blank. A 16 ms `WM_TIMER` beats against the
    16.67 ms blank and drops or doubles frames. The timer is only the fallback for an occluded window.
  - text layouts are built once and cached (`Painter::Text`), because a page transition draws two pages
    every frame and building a `IDWriteTextLayout` per string per frame is what makes it miss them.
- **Reduced motion still wins.** Every transition becomes instant; nothing below is exempt.

**Page transitions**

| App | Transition |
|---|---|
| Windows: sidebar page change | The outgoing page fades out and slides 8 px toward the nav; the incoming fades in and slides 8 px from the far side. 250 ms. The sidebar pill slides to the new item in 200 ms. |
| Phone: bottom-nav tab change | A shared-axis horizontal slide: outgoing out 16 dp and fades, incoming in from 16 dp. 250 ms. The direction follows the tab order. |
| Phone: splash → Connect | The splash content fades and lifts 12 dp; the first screen fades in. The background photo is shared and does not move, so it reads as one continuous scene. 300 ms. |
| Phone: screen → dialog | The dialog rises 16 dp and fades in while the backdrop blur fades up. 220 ms. |
| Both: status change | Crossfade 200 ms, with the badge popping 0.6 → 1.0 with a slight overshoot. |

**Polish items (what "smooth" means concretely)**
1. **No first-frame flash anywhere.** Fixed for the PC window (§8); apply the same rule to the pairing
   dialog and any new page: draw before showing.
2. **No animation on first paint.** A page that is already on screen when the window opens is simply there;
   only later changes animate.
3. **Preview start is a crossfade, not a pop** (both apps).
4. **The green marquee is continuous.** Its cycle is driven by elapsed time, so it never jumps when a
   repaint is late.
5. **Buttons respond instantly.** The pressed state is immediate; only the release animates.
6. **The radar sweep** rotates at a constant angular speed and is driven by elapsed time.
7. **Toggle switches** move the thumb and cross-fade the track together, 150 ms.
8. **Dropdowns** open with a 120 ms fade and a small scale from the control edge, never a slide from
   off-screen.
9. **The phone preview** fades in over 200 ms when the camera starts, and out when it stops, so the camera
   starting never shows a black rectangle.
10. **Scroll is never animated by us.** The platform owns scrolling.
