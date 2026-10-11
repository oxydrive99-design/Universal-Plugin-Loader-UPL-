# UPL 0.4.2 installer

Native Windows x64 installer with the release DLL embedded as a resource. No separate runtime or network download is required.

Run `BUILD_INSTALLER.bat` from an x64 Visual Studio Developer Command Prompt. Build the loader first: the resource uses `../bin/x64/Release/vcruntime140_1.dll`. The output is `../build/installer/UniversalPluginLoader-Installer-0.4.2.exe`.

The installer searches Steam libraries, preserves the original runtime, and replaces only the UPL proxy. If discovery finds several installations or none, select the game's `Release/ScrapMechanic.exe`. It refuses to modify the selected installation while its game process is running. Missing or invalid original runtimes, and ambiguous runtime/backup pairs, stop installation. A failed first installation restores the original file.

Optional command-line use:

```text
UniversalPluginLoader-Installer-0.4.2.exe /dir "D:\SteamLibrary\steamapps\common\Scrap Mechanic\Release"
UniversalPluginLoader-Installer-0.4.2.exe /dir "D:\SteamLibrary\steamapps\common\Scrap Mechanic\Release" /silent
```

Exit codes: 0 success, 1 failure, 2 user cancellation. Silent mode requires an explicit directory and prints failures to stderr.

Validation used temporary game-directory fixtures: first installation, update from 0.4.0, repeated installation, exact preservation of the original runtime, byte-for-byte agreement with the release DLL, missing/corrupt backups, conflicting runtimes, and locked-file cleanup.
