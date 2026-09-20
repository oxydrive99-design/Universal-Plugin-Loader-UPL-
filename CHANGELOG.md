# Changelog

## 0.4.0
- Added native query API: `UPL_GetApiVersion`, `UPL_IsContentActive`, `UPL_IsPluginActive`.
- Active UGC state is now tracked transactionally while Scrap Mechanic changes world/mod content.
- Native plugins can stop touching a mod immediately when it is removed from the current world.
- Empty Active UGC blocks now commit an empty active state correctly.

# Changelog

## 0.3.0

- Added explicit plugin status log lines: `LOADED`, `SKIPPED_DUPLICATE`, `INVALID_MANIFEST`, `DLL_NOT_FOUND`, `LOAD_FAILED`, `INCOMPATIBLE_LOADER`.
- Added optional `minLoaderVersion` manifest field.
- Duplicate plugin logs now show both the owning mod and skipped duplicate mod.
- Keeps v0.2 Local + Workshop resolution behavior.

# Changelog

## 0.2.0
- Added Local mod resolver using APPDATA/User_*/Mods and description.json.localId.
- Active UGC parser now honors Local: true/false.
- Added dynamic Steam Library discovery through libraryfolders.vdf.
- Local active content has priority over a Workshop copy during development.
- Workshop active content has Workshop priority in release use.
- Added validated cross-source fallback.
- Improved resolver diagnostics.

## 0.1.1
- Switched proxy build from static CRT to /MD to fix duplicate vcruntime exports.
