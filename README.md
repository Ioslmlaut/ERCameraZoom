# Elden Ring Camera Zoom Mod

Lets you control the camera zoom distance in Elden Ring using your scroll wheel

---

## Requirements

- [Elden Ring Mod Loader](https://www.nexusmods.com/eldenring/mods/117) by techiew
- or [Mod Engine 2](https://github.com/soulsmods/ModEngine2/releases) by katalash
- Elden Ring version **1.16.1**
- Windows 11
- Mouse (no controller support)

---

## Installation

You need to know how to use one of the mod loaders listed above, figure that out first, there are plenty of guides on Nexus and YouTube. 
Once you have that set up, just drop `ERCameraZoom.dll` into your mods folder and you're done. The `config.ini` will be generated automatically the first time the game loads.

---

## How to Use

Hold **Alt** and scroll your mouse wheel to zoom in or out

The zoom level is **saved automatically** when you quit

---

## Configuration

Open `config.ini` (generated on first run) in any text editor to adjust things:

```ini
camera_distance = 10   ; starting zoom level
step            = 1    ; how much each scroll click changes the zoom
min             = 1    ; closest zoom allowed
max             = 30   ; furthest zoom allowed
smooth_factor   = 0.08 ; easing speed (0.01 = slow glide, 1.0 = instant snap)
invert_scroll   = 0    ; set to 1 to flip scroll direction
```

---

## Notes

- Only affects camera distance, nothing else
- Safe for offline play. Use online at your own risk
- If it doesn't work for you, the repo is public, feel free to dig in and fix it yourself. I'm barely a C++ dev myself, I'm not in a position to provide support
- ~95% of this mod was vibe coded with Claude in a week with the help of the Hexinton cheat sheet :)
