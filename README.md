# Universal Plugin Loader for Scrap Mechanic — v0.4.0

Universal Plugin Loader keeps one proxy DLL in `Scrap Mechanic/Release`, while native DLL plugins live inside the mod that owns them.

## v0.4.0 — Plugin diagnostics + compatibility gate

This build keeps the v0.2 Local/Workshop resolver and adds explicit plugin load states plus an optional minimum-loader compatibility gate.

Recognized status lines include `LOADED`, `SKIPPED_DUPLICATE`, `INVALID_MANIFEST`, `DLL_NOT_FOUND`, `LOAD_FAILED`, and `INCOMPATIBLE_LOADER`.

A shared bridge may be shipped by multiple active mods using the same `id`; only the first successfully loaded copy stays resident.

## v0.2.0 — Local + Workshop resolver

This build is intended to make native-plugin development testable **without publishing unfinished mods to Steam Workshop**.

The loader follows Scrap Mechanic's current `Logs/game-*.log` and reacts only to the game's `Active UGC Content` block.

Each active line is parsed for:

- `Content ID`
- `Steam File ID`
- `Local: true/false`

Resolution rules:

1. `Local: true` → resolve the mod below `%APPDATA%\\Axolot Games\\Scrap Mechanic\\User\\User_*\\Mods` by matching `description.json.localId`.
2. `Local: false` → resolve the Steam Workshop item in the user's Steam Libraries.
3. If the preferred source is unavailable, a defensive fallback checks the other source, but the `Content ID` must still match `description.json.localId`.
4. Only after the owning active mod is resolved does UPL inspect `Native/plugin.json`.

A local development copy therefore wins over a subscribed Workshop copy when Scrap Mechanic itself marks that active UGC as local.

## Steam Library discovery

No drive letter, Steam install folder, username, or Steam user id is hardcoded.

UPL:

- finds the `steamapps` ancestor of the running `ScrapMechanic.exe`;
- includes the Steam Library containing the game;
- parses that library's `steamapps/libraryfolders.vdf`;
- adds `<library>/steamapps/workshop/content/387990` for every discovered Steam Library.

Examples that are all valid without code changes:

```text
C:\\Program Files (x86)\\Steam\\steamapps\\...
D:\\SteamLibrary\\steamapps\\...
E:\\Steam\\steamapps\\...
F:\\Games\\SteamLibrary\\steamapps\\...
```

## Local mod discovery

UPL asks Windows for `%APPDATA%` at runtime and scans all `User_*` profiles below:

```text
%APPDATA%\\Axolot Games\\Scrap Mechanic\\User\\User_*\\Mods
```

Folder names do not matter. UPL identifies the mod through:

```json
{
  "localId": "8a4041db-121a-4b77-adc8-35689c7c58f3"
}
```

in `description.json`.

## Plugin layout — identical for Local and Workshop

```text
<My Mod>/
  description.json
  Native/
    plugin.json
    my_plugin.dll
    optional_dependency.dll
```

Example `Native/plugin.json`:

```json
{
  "schemaVersion": 1,
  "id": "oxydrive99.creation-shield-bridge",
  "name": "CreationShieldBridge",
  "version": "0.7.0",
  "minLoaderVersion": "0.4.0",
  "dll": "creation_shield_bridge.dll"
}
```

The DLL path must be relative and remain inside the owning mod. `..` traversal and absolute paths are rejected.

Plugin `id` is a process-wide deduplication key. Two active mods may ship the same shared bridge; UPL loads that plugin ID only once.

## Install UPL

```text
Scrap Mechanic/
  Release/
    ScrapMechanic.exe
    vcruntime140_1.dll      <- Universal Plugin Loader
    vcruntime140_1_.dll     <- renamed original runtime
```

`Release/DLLModules` is not used for UPL plugins.

Command-line escape hatches:

- `-noupl`
- `-noinject` (compatibility alias)

Log:

```text
Scrap Mechanic/Release/UniversalPluginLoader.log
```

Expected v0.2 diagnostics include lines like:

```text
INFO: Local Mods root = C:\\Users\\...\\AppData\\Roaming\\Axolot Games\\Scrap Mechanic\\User\\User_...\\Mods
INFO: Workshop root = E:\\Steam\\steamapps\\workshop\\content\\387990
ACTIVE UGC: content=... steam=... local=true
RESOLVED: Local mod ... -> C:\\Users\\...\\Mods\\Creation Shield
```

If that mod has no `Native/plugin.json`, UPL simply logs that fact and loads nothing.

## Lifecycle

UPL does not call `FreeLibrary` when a mod becomes inactive. Hook-based native plugins are unsafe to unload arbitrarily. A plugin remains resident until Scrap Mechanic exits after its first load. A later plugin API can add explicit activation/deactivation callbacks without unloading the module.

## Build

Requirements:

- Visual Studio 2022
- MSVC v143
- Windows 10/11 SDK
- `Release | x64`

The proxy uses the dynamic MSVC runtime (`/MD`; `/MDd` for Debug). `/MT` must not be used because it collides with the proxy's exported VC runtime functions.

Output:

```text
bin/x64/Release/vcruntime140_1.dll
```

## Upstream / license

The proxy/forwarding mechanism is derived from QuestionableM's SM-DLL-Injector. See `LICENSE` and `CREDITS.md`.
