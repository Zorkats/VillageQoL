# VillageQoL

Quality of life features for the Steam version of Village in the Shade: quick save, faster walking, event fast forward, mouse wheel zoom and more. VillageQoL is a mod for [Canopy](https://github.com/Zorkats/Canopy), the Village in the Shade mod loader, and is configured inside the game on Canopy's **Mod Settings** page.

## Install

You need Canopy first. [Canopy Studio](https://github.com/Zorkats/CanopyStudio) installs both for you; to do it by hand:

1. Install Canopy as its [README](https://github.com/Zorkats/Canopy#install) says: `dinput8.dll` next to `village.exe`, and a `mods` folder beside it. On Linux and Steam Deck, also set the Steam launch option it gives.
2. Download `VillageQoL-<version>.zip` from the [releases page](https://github.com/Zorkats/VillageQoL/releases/latest).
3. Extract it into the game folder, so `VillageQoL.dll` lands in `mods`.
4. Start the game. Open **Options**, then **Mod Settings**: a `+ VillageQoL` row means it loaded.

```text
Village in the Shade\
├── village.exe
├── dinput8.dll              Canopy
└── mods\
    ├── VillageQoL.dll
    └── VillageQoL.ini       your VillageQoL settings, created on first start
```

To uninstall, delete `mods\VillageQoL.dll` and `mods\VillageQoL.ini`.

## Features

| Feature | What it does | Default |
|---|---|---|
| Show BGM Information | Shows each song's title in the game's popup log the first time it plays. | On |
| Quick Save | Press the key in the field to save and return to the title screen. The save loads from an extra row in the load menu, and is deleted once you load it. | F5, no controller button |
| Settings Hotkey | Press the key in the field to open the options. | F10, no controller button |
| Walking Speed | How fast you walk and run in the field: Normal, 1.25x, 1.5x, 1.75x or 2x. Movement during events does not change. | 1.5x |
| Event Fast Forward | Hold the key or button during an event the game lets you skip, to play it faster. Events that cannot be skipped are never sped up. | Left Ctrl or RT, 4x |
| Advance Text | While fast forwarding, talk advances on its own. | On |
| Beta Event Skip | Press Cancel and the fast forward button together (B + RT, or Circle + R2) during an event the game lets you skip, to run it at 32x like the beta build's skip. It stops at every choice and popup and hands control back, so it never answers for you. | Off |
| Mouse Wheel Zoom | Scroll the mouse wheel in the field to step the camera through its three zoom levels. | On |
| Back Up Key Bindings | Keeps a copy of your game key bindings in `mods\VillageQoL.ini` and restores them if they ever load back at their defaults. | On |

Each feature checks the game code it needs before it hooks anything. After a game update, a feature whose code moved stays off and `Canopy.log` says so, while the others keep working. Quick Save writes a save, so it also stays off until a Canopy release checks the new game build. Canopy's [troubleshooting table](https://github.com/Zorkats/Canopy#when-something-goes-wrong) explains each log line.

## Building from source

You need Visual Studio 2022 with the **Desktop development with C++** workload, which includes CMake and Ninja. vcpkg is not needed: VillageQoL hooks through Canopy, and only Canopy's loader links MinHook.

Canopy comes in as a git submodule in `external/Canopy`. It brings the SDK header and `src/common`, the game addresses and the lists of what each feature needs. Clone with `--recursive`, or run `git submodule update --init` in an existing clone.

From an **x64 Native Tools Command Prompt for VS 2022**, in the repository folder:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --component villageqol --prefix dist\villageqol
```

`build\src\VillageQoL.dll` is the mod, and `dist\villageqol` holds the same layout as the release zip.

| Option | What it does |
|---|---|
| `-DCANOPY_DIR=<folder>` | Build against another Canopy checkout instead of the submodule, such as one with changes to the game addresses. Give the full path. |
| `-DVILLAGEQOL_RELEASE=1.2.0` | The version the DLL's version resource and the mod's manifest carry. The release workflow sets it from the tag. |
| `-DVILLAGEQOL_BUILD_TESTS=OFF` | Skip Canopy's core tests. |

The tests are Canopy's core tests. Their address-list check also reads `src`: each game address a source file uses must be in a list that file checks before it hooks, so a feature never reads an address the game build was not checked for. The lists live in Canopy's `src/common/game_retail.h`, so a feature that needs a new address changes Canopy first, then moves the submodule here.

## Releases

GitHub Actions builds and tests every push and pull request. Pushing a tag such as `v1.2.0` publishes `VillageQoL-1.2.0.zip`, which Canopy Studio installs from.

| Folder | Contents |
|---|---|
| `src\` | The mod: one file per feature, and `plugin.cpp`, which registers them with Canopy. |
| `external\Canopy\` | Canopy, as a git submodule. |

## License

MIT. See [LICENSE](LICENSE).
