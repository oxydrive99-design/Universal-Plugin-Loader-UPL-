# Packaging helper

After building a bridge DLL, use `Install-PluginIntoWorkshopMod.ps1` against the *unpacked/mod-development* Workshop folder before publishing it.

Keybind Core example:

```powershell
.\Install-PluginIntoWorkshopMod.ps1 `
  -ModDirectory '...\3800264910' `
  -PluginDll '.\keybind_bridge.dll' `
  -PluginId 'oxydrive99.keybindbridge' `
  -PluginName 'KeybindBridge' `
  -PluginVersion '1.0.4-upl'
```

Standalone Flashlight can ship the same DLL with the same plugin ID. Add `-RuntimeLua <path-to-KeybindRuntime.lua>` so it does not need Core for the shared runtime. Universal Plugin Loader deduplicates the ID if Core and Flashlight are both active.

Creation Shield example:

```powershell
.\Install-PluginIntoWorkshopMod.ps1 `
  -ModDirectory '...\3802255754' `
  -PluginDll '.\creation_shield_bridge.dll' `
  -PluginId 'oxydrive99.creation-shield-bridge' `
  -PluginName 'CreationShieldBridge' `
  -PluginVersion '0.7.0'
```
