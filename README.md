# KOG 2.5D Map Editor

A lightweight RPG-Maker-style map editor rendered in a true 3D environment.
Built with **Raylib 5.x + Dear ImGui 1.91 + rlImGui**.

---

## Quick Start

### Requirements
- Windows 10/11
- Visual Studio 2022 Build Tools
  (Install via `vs_BuildTools.exe` → "Desktop development with C++")
- Git (for FetchContent to clone dependencies automatically)
- Internet connection on first build (CMake downloads raylib, imgui, rlImGui)

### Build & Run

```bat
build.bat
```

The first build takes a few minutes (downloads + compiles ~3 dependencies).
Subsequent builds are fast (incremental).

The executable lands at:
```
build_cmake\KOG25DEditor.exe
```

---

## Editor Layout

```
┌──────────────────────────────────────────────────┐
│  Menu Bar : File | View                          │
├──────────┬──────────────────────────┬────────────┤
│ FLOOR    │                          │  3D PROPS  │
│ TEXTURES │    3D VIEWPORT           │            │
│ (click   │    (Raylib renders here) │  (click a  │
│  to pick │                          │   model)   │
│  brush)  │                          │            │
├──────────┴──────────────────────────┴────────────┤
│ Status bar : hovered tile info                   │
├──────────────────────────────────────────────────┤
│ Layer: [Floor 2D] [Decor 3D] | path | Save | Load│
└──────────────────────────────────────────────────┘
```

---

## Controls

| Action              | Input                        |
|---------------------|------------------------------|
| Place asset         | Left Mouse Button (hold)     |
| Erase asset         | Right Mouse Button (hold)    |
| Pan camera          | W A S D  or  Arrow Keys      |
| Zoom                | Mouse Scroll Wheel           |
| Reset camera        | View → Reset Camera          |
| Save map            | Ctrl+S  or  Save button      |
| Load map            | Ctrl+O  or  Load button      |

---

## Adding Your Own Assets

### Floor Textures
Drop `.png` or `.jpg` files into:
```
assets/textures/
```
They appear as thumbnail buttons in the **Floor Textures** panel.

### 3D Props
Drop `.glb` or `.obj` files into:
```
assets/models/
```
They appear as labelled buttons in the **3D Props** panel.
Models are auto-scaled to fit within one grid cell.

---

## Map File Format (`.map`)

Plain text, human-readable:

```
# KOG 2.5D Map
version 1
grid_w 50
grid_h 50
tile 3  5  grass  tree.glb
tile 10 7  stone  -
tile 11 7  stone  tent.glb
```

- Column 3 = floor texture name (`-` = none)
- Column 4 = prop model name (`-` = none)

---

## Project Structure

```
KOG-2.5D-Editor-de-mapa/
├── CMakeLists.txt        ← build system (FetchContent deps)
├── build.bat             ← one-click build script
├── src/
│   └── main.cpp          ← all editor source (~600 lines)
├── assets/
│   ├── textures/         ← drop .png/.jpg here
│   └── models/           ← drop .glb/.obj here
└── maps/                 ← saved .map files go here
```
