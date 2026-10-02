# Beach Buggy Racing — Stereoscopic Anaglyph 3D Mod 🕶️🏎️

This is a fan-made stereoscopic 3D modification for **Beach Buggy Racing** (Windows PC version). The mod injects custom graphics processing by overriding the default `sdl2.dll` library to output a real-time Red-Cyan Anaglyph 3D effect directly from the game engine.

## ✨ Features
* 🟢 **Real-Time 3D Rendering:** Smooth Red-Cyan Anaglyph processing without game lag.
* ⌨️ **Live Calibration Hotkeys:** Fine-tune depth, convergence, and change settings on the fly.
* 🛠️ **Smart Path Checker:** Clean `install.bat` script that verifies your target directory before changing files.

## ⌨️ In-Game Controls

Use these hotkeys during the race to calibrate the 3D effect perfectly for your glasses and monitor size:

| Hotkey | Action |
|--------|--------|
| **F6** | **Toggle 3D Effect ON / OFF** |
| **F8 / F9** | Decrease / Increase Separation (3D Depth Strength) |
| **F10 / F11**| Adjust Convergence Point (Focus Plane) |
| **F7** | Cycle Modes (Depth Buffer / Backup Y-Mode / Debug Visualizer) |
| **F5** | Change Anaglyph Blend Type |
| **F12** | Swap Left and Right Eyes |
| **Insert** | Invert Z-Buffer Depth Direction |

## 🚀 Installation Guide

1. Download the mod files into a separate folder on your PC (Do NOT extract directly into the game folder).
2. Open `config.txt` using Notepad or any text editor.
3. Set your actual game directory path. Example:
   ```text
   GAME_PATH=C:\Games\Beach Buggy Racing
   ```
4. Right-click `install.bat` and choose **Run as Administrator**.
5. The script will automatically verify the folder existence, create a backup of your original `sdl2.dll` (`SDL2_orig.dll`), and inject the 3D mod files.
6. Put on your 3D glasses, launch the game, and hit **F6**!

---
## ⚠️ Legal Disclaimer
**DISCLAIMER:** This is an unofficial, non-commercial, fan-made modification. All original game code, assets, textures, 3D models, characters, and music belong exclusively to **Vector Unit**. 
