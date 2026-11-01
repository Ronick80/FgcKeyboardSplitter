# FGC Keyboard Splitter

Turns one or more physical keyboards into virtual Xbox 360 controllers on Windows, built for fighting games:
each keyboard is read on its own high-priority thread, its keys are hidden from every other program while it
runs, and each key change goes straight to a virtual controller.

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
| `drivers\ScpVBus\` | The ScpVBus driver files, copied next to the app |

The C++ runtime is linked statically, so the binaries run on a PC without the Visual C++ Redistributable.
Dependencies (Dear ImGui, nlohmann/json) are downloaded by CMake on the first configure.

If `cmake` is not recognised in a normal PowerShell window, use the Developer PowerShell, or add
`C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`
to your `PATH`.

## Installing the drivers

Both installs need an administrator prompt. The app shows the status of each driver and these same steps.

**Interception**

1. Download the release from the official project (`oblitum/Interception` on GitHub) and extract it.
2. In the `command line installer` folder run `install-interception.exe /install`.
3. Restart Windows. Until you do, the app reports "Reboot required".

To remove it: `install-interception.exe /uninstall`, then restart.

**ScpVBus**

1. From the `drivers\ScpVBus` folder (next to `FgcApp.exe`, or `drivers\ScpVBus` in the source tree) run:
   `devcon.exe install ScpVBus.inf Root\ScpVBus`

To remove it: `devcon.exe remove Root\ScpVBus`.

Both drivers are old. If Windows blocks one (Secure Boot, memory integrity, driver signing), the app will keep
showing it as not installed.

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
virtual controller accepting the report. It does not include keyboard or USB delay, or how often the game reads
its controller.

## Command line

```powershell
FgcCli list                       # drivers and keyboards
FgcCli run <index|hardware-id> 30 # run one keyboard for 30 seconds (omit the number to run until Ctrl+C)
```

`samples\csharp\FgcSample.cs` shows the same API called from C# with plain P/Invoke.

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

## Project layout

```
core/      FgcCore.dll: keyboard and virtual-pad backends, mapping threads, profiles, public C API
app/       FgcApp.exe: Dear ImGui front end, uses only core/include/fgc_core.h
cli/       FgcCli.exe: command-line front end
samples/   Calling the C API from C#
drivers/   ScpVBus driver files
```

The keyboard and controller backends sit behind small interfaces (`core/src/backends`), so the engine does not
depend on Interception or ScpVBus directly. The Interception driver is driven through its device interface
rather than `interception.dll`: the library opens every keyboard exclusively per context, which would stop
keyboards from having independent threads, and it allocates memory on every call.
