# FGC Keyboard Splitter

> **Credit:** this project builds on [KeyboardSplitterXbox](https://github.com/djlastnight/KeyboardSplitterXbox)
> by djlastnight. That application showed how to combine the Interception keyboard driver with the ScpVBus virtual
> Xbox controller driver, and its architecture is the basis for this one. Thank you to its developer for that work.
> FGC Keyboard Splitter is a separate C++ implementation written from scratch; the bundled ScpVBus driver files
> come from that project.

Turns one or more physical keyboards into virtual Xbox 360 controllers on Windows, built for fighting games:
each keyboard is read on its own high-priority thread, its keys are hidden from every other program while it
runs, and each key change goes straight to a virtual controller.

## Why this exists

Fighting games punish any delay or stuck input, and keyboard-to-controller mappers can add noticeable delay or
leave inputs stuck. This project was created to try to bring input latency down significantly so a keyboard is
a practical way to play them. The design choices all serve that goal:

- Every keyboard is handled on its own high-priority thread, separate from the interface.
- The input path blocks until a key arrives instead of polling, and does no memory allocation, locking or file
  access while handling a key.
- The Interception driver is driven directly, with one handle per keyboard, so keyboards never wait on each other.
- Each key change becomes one report to the virtual controller, sent immediately.
- Key state is tracked exactly, so remapping while a key is held, opposing directions and unplugging a keyboard do
  not leave inputs stuck.
- Mappings can be edited while a keyboard is running without interrupting input.

The Latency table in the app shows how long the app itself takes from a key event to the virtual controller
accepting it. Delays outside the app, such as the keyboard's own scan rate and how often a game reads its
controller, are not something it can change.

## Requirements

- Windows 10 or 11, x64
- Two kernel drivers (see [Installing the drivers](#installing-the-drivers))
  - **Interception** captures one specific keyboard and hides it from other programs.
  - **ScpVBus** provides the virtual Xbox 360 controllers.
- To build: Visual Studio 2022 with the "Desktop development with C++" workload (includes CMake)

## Building

From a "Developer PowerShell for VS 2022" in this folder:

```powershell
cmake --preset default
cmake --build --preset release     # or: debug
```

Output is in `build\bin\<Config>\`:

| File | What it is |
|---|---|
| `FgcApp.exe` | The Dear ImGui application |
| `FgcCore.dll` | The engine; both front ends use it through `core/include/fgc_core.h` |
| `FgcCli.exe` | A small command-line front end |
| `FgcDriverSetup.exe` | Installs and removes the two drivers (see [Installing the drivers](#installing-the-drivers)) |
| `drivers\ScpVBus\` | The ScpVBus driver files, copied next to the app and the setup tool |

The C++ runtime is linked statically, so the binaries run on a PC without the Visual C++ Redistributable.
Dependencies (Dear ImGui, nlohmann/json) are downloaded by CMake on the first configure.

If `cmake` is not recognised in a normal PowerShell window, use the Developer PowerShell, or add
`C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`
to your `PATH`.

## Installing the drivers

The easy way is `FgcDriverSetup.exe`, which sits next to `FgcApp.exe`. Keep `FgcCore.dll` and the `drivers` folder
next to it. Double-click it (it asks for administrator rights) and choose **1) Install missing drivers**, or use
the command line:

```powershell
FgcDriverSetup status                 # which drivers are installed
FgcDriverSetup install                # install whatever is missing
FgcDriverSetup install scpvbus        # or interception
FgcDriverSetup uninstall scpvbus      # or interception
FgcDriverSetup install --dry-run      # show what would happen, change nothing
```

`--yes` skips the questions. Interception needs a restart of Windows; the tool offers one (default No). Exit codes:
0 done, 1 failed, 2 wrong arguments, 3 done but a restart is pending. Everything it does is written to
`FgcDriverSetup.log` next to it, including your Windows version, Secure Boot and memory integrity state; send that
file if something fails.

Interception is downloaded from the official `oblitum/Interception` v1.0.1 release on GitHub and checked against a
built-in SHA-256 checksum before anything from it runs. On a PC without internet access, download `Interception.zip`
from that release yourself and put it next to `FgcDriverSetup.exe`; it is checked the same way.

The manual steps below do the same thing and are the fallback if the tool does not work for you. Both need an
administrator prompt.

**Interception**

1. Download the release from the official project (`oblitum/Interception` on GitHub) and extract it.
2. In the `command line installer` folder run `install-interception.exe /install`.
3. Restart Windows. Until you do, the app reports "Reboot required".

To remove it: `install-interception.exe /uninstall`, then restart.

**ScpVBus**

From the `drivers\ScpVBus` folder (next to `FgcApp.exe`, or `drivers\ScpVBus` in the source tree):

1. **Windows 11 (and any PC where step 2 fails):** first add the driver to Windows' driver store:
   `pnputil /add-driver ScpVBus.inf /install`
   On its own this does not create the device, so the app still reports ScpVBus as not installed. Continue with step 2.
2. Create the device and attach the driver: `devcon.exe install ScpVBus.inf Root\ScpVBus`
3. Check it: `devcon.exe status Root\ScpVBus` should say "Driver is running". Then click "Re-check drivers" in the app.

On Windows 11, running `devcon.exe install` without step 1 can end with only "devcon.exe failed". If a failed attempt
left a stray device behind, remove it first with `devcon.exe remove Root\ScpVBus` (or in Device Manager with View >
Show hidden devices), then repeat steps 1 and 2.

To remove it: `devcon.exe remove Root\ScpVBus`.

Both drivers are old. If Windows blocks one (Secure Boot, memory integrity, driver signing), the app will keep
showing it as not installed; `devcon.exe status` shows the problem code.

## Using the app

1. Start `FgcApp.exe`. The Drivers section should show both drivers as Installed.
2. Pick a keyboard in the list, or click **Identify by key press** and press a key on the one you want. One
   physical keyboard can appear several times (one entry per USB interface); the entry that reacts to its normal
   typing keys is usually the one ending in `MI_00`.
3. Edit the mapping if you like. Click a key button and press the new key, or use `...` to pick from a list.
   Changes apply immediately, even while running.
4. Click **Start**. The keyboard now controls a virtual Xbox controller and is hidden from other programs.
   Start more keyboards the same way; each gets its own controller.
5. Click **Stop** (or **Stop all**) to give the keyboard back.

While a keyboard is running, none of its keys reach Windows. If it is your only keyboard, click Stop with the
mouse. Closing the app also releases everything.

Windows supports at most 4 XInput controllers in total. The app counts controllers that are already connected
plus the ones it holds, and refuses to start a keyboard that would be the fifth.

The default mapping: WASD is the D-pad, the arrow keys are the left stick, J K U I are A B X Y, O and P are the
bumpers, L and `;` are the triggers, Enter is Start, Tab is Back. Opposing directions are resolved the way
leverless controllers do: left+right cancels to neutral, up+down gives up.

### Profiles

A profile is a named mapping saved for one keyboard. Use the Profile section to load, save, save as, rename and
delete. A keyboard starts from the profile it used last. Files are plain JSON in
`%APPDATA%\FgcKeyboardSplitter`; their format is documented in `core/include/fgc_core.h`.

### Latency

While a keyboard runs, the Latency table shows the time from the mapping thread waking on a key event to the
virtual controller accepting the report, in **microseconds** (1,000 us = 1 ms). Read is the keyboard driver read
and Report is the virtual controller call; the remainder is the app's own work. It does not include keyboard or USB
delay, or how often the game reads its controller.

## Command line

```powershell
FgcCli list                       # drivers and keyboards
FgcCli run <index|hardware-id> 30 # run one keyboard for 30 seconds (omit the number to run until Ctrl+C)
```

`samples\csharp\FgcSample.cs` shows the same API called from C# with plain P/Invoke.

## Writing another front end

The engine has no UI dependency, so any program can be a front end. The contract is the C API in
`core/include/fgc_core.h` plus the files in the release zip:

- **`FgcCore.dll`** is x64 with a static runtime and no other dependencies. Load it from the program's own folder.
  Check `fgc_get_api_version()` against the version you were built for and show `fgc_get_version()` as the engine
  version. API version 1 is stable: later releases only add functions and append struct fields, and every struct
  starts with `struct_size`.
- **`FgcDriverSetup.exe` and `drivers\`** install the two drivers. A front end can start the tool elevated and
  then re-read `fgc_get_driver_status()`; the tool's exit codes are listed in
  [Installing the drivers](#installing-the-drivers).
- **`FgcKeyboardSplitter-<version>-win-x64.zip.sha256`**, attached to each release, is the SHA-256 of the zip, so a
  build script can download a pinned version and verify it.
- Profiles are plain JSON files that every front end shares (see [Profiles](#profiles)).
- On exit call `fgc_destroy` (or at least `fgc_stop_all`), also from crash handlers: a controller left plugged at
  ScpVBus keeps holding its slot.

## When something goes wrong

- **A keyboard unplugs or the driver stops answering:** its controller is neutralized and released, the keyboard
  goes back to Windows, and the app shows a notice. Click Start to resume.
- **The app crashes:** the keyboard returns to Windows when the process ends, and a crash handler neutralizes and
  unplugs the virtual controllers first. A process that is killed outright (Task Manager, a stopped debugger)
  cannot run that handler and can leave a controller plugged in at ScpVBus.
- **Start says "No free controller slot" with fewer than 4 controllers in use:** ScpVBus may be holding controllers
  left by a killed process. Restart Windows, or restart the bus driver from an administrator prompt with
  `pnputil /restart-device "ROOT\SYSTEM\0001"`.
- **"Keyboard is in use by another program":** another program (for example the old KeyboardSplitter) holds that
  keyboard. Close it first.
- **A key seems stuck in other programs after Stop:** the app sends key-up events for any key still held when a
  keyboard stops; pressing and releasing the key once also clears it.

## Versions and releases

`name-version.txt` holds the product name on line 1 and the version (`MAJOR.MINOR.PATCH`) on line 2. CMake reads
it and uses it for the version shown in the app and `FgcCli version`, the version resource of every binary
(Properties > Details), the package file name, and the GitHub release.

To make a package locally: `cmake --build build --config Release --target package` produces
`build\FgcKeyboardSplitter-<version>-win-x64.zip`.

The release workflow also attaches `FgcKeyboardSplitter-<version>-win-x64.zip.sha256`, the SHA-256 of the zip.

To publish a release:

1. Edit the version in `name-version.txt` and commit it.
2. Tag that commit and push the tag:
   ```powershell
   git tag v1.0.0
   git push origin v1.0.0
   ```
3. The **Release** workflow (`.github/workflows/release.yml`) builds on GitHub, checks the tag matches
   `name-version.txt`, and publishes the release with the zip attached. You can also start it from the repository's
   Actions tab with "Run workflow", which creates the tag for you.

## Project layout

```
name-version.txt  Product name and version
cmake/            Templates for the generated version header and resources
.github/          Release workflow
core/      FgcCore.dll: keyboard and virtual-pad backends, mapping threads, profiles, public C API
app/       FgcApp.exe: Dear ImGui front end, uses only core/include/fgc_core.h
cli/       FgcCli.exe: command-line front end
driver_setup/  FgcDriverSetup.exe: installs and removes the Interception and ScpVBus drivers
samples/   Calling the C API from C#
drivers/   ScpVBus driver files
```

The keyboard and controller backends sit behind small interfaces (`core/src/backends`), so the engine does not
depend on Interception or ScpVBus directly. The Interception driver is driven through its device interface
rather than `interception.dll`: the library opens every keyboard exclusively per context, which would stop
keyboards from having independent threads, and it allocates memory on every call.
