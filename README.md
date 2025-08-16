# KeepAudio — Headless USB Audio Keep‑Alive (Windows)

KeepAudio is a Windows program that prevents certain audio interfaces/DACs from power‑gating (and "popping") between sounds.  
Itcontinuously play a inaudible tone (default: **1 Hz at −100 dBFS**) to keep the device’s output path alive.  
The app is **headless** when run at logon and uses negligible CPU.

---

## Why this exists

Many low‑cost USB audio devices cut power to their analog stages when the OS stops streaming audio, causing an audible pop when audio resumes. By keeping a constant, ultra‑low‑level stream open, the device stays awake and avoids that pop.
This also works for preventing bluetooth devices from entering low power mode and avoids the small cut in the audio when it comes back to operation.

---

## Features

- **Headless autostart** (per‑user, no admin): `--install` registers a Run‑key entry and runs invisibly at logon.
- **Device selection and audio controls**: frequency, level (dBFS), sample rate, channels, buffer sizing.
- **Format selection**: `auto | pcm16 | float32` (auto tries float for ≤ −96 dBFS).
- **Device listing**: `--list-devices` (MessageBox in headless mode, or console output with `--console`).
- **Optional console** for interactive tests and logs: `--console`.
- **Install/uninstall helpers**:
  - `--install` adds a per-user autostart entry.
  - `--install-copy` copies the EXE to `%LOCALAPPDATA%\KeepAudio\keepaudio.exe` before registering.
  - `--startup-name` customizes the Run‑key value name.
  - `--uninstall` removes the autostart entry (and cleans up the copied EXE).
- **Random early‑exit (`--chance`)** for blind tests.  
    - This was added after a friend claimed the low tone was causing headaches. In order to prove it was a **nocebo effect** rather than an actual audible/physiological cause, I've added this to prove they where unable to tell when the program was running or not (other than the fix to their headphone)

---

## Quick start (use the prebuilt EXE)

You can run KeepAudio without building anything. The repository includes a compiled `keepaudio.exe`.

### 1) Clone to any folder

```powershell
git clone https://github.com/your-account/keepaudio.git
cd keepaudio
```

### 2) Install to run headlessly at logon

```powershell
.\keepaudio.exe --install
```
- The program records your current flags (excluding install/uninstall helpers) and uses them at logon.
- The process will start automatically on the next logon.  

```powershell
.\keepaudio.exe --console
```

### Optional: Install to LocalAppData instead of having it run from the cloned directory.

```powershell
.\keepaudio.exe --install --install-copy
```

This copies to `%LOCALAPPDATA%\KeepAudio\keepaudio.exe` and registers that path (good for keeping a stable location independent of your Git clone).

### 4) Uninstall autostart

```powershell
.\keepaudio.exe --uninstall
```

---

## Running manually (without autostart)

```powershell
# Default: 1 Hz at -100 dBFS, float32 if available, headless unless --console is passed
.\keepaudio.exe

# Show devices in a console (for interactive testing)
.\keepaudio.exe --console --list-devices

# Pick a specific device index at 44.1 kHz
.\keepaudio.exe --device 0 --rate 44100

# Nudge the level up if your interface still sleeps (e.g., -90 dBFS)
.\keepaudio.exe --db -90

# Explicitly use float32 or PCM16
.\keepaudio.exe --format float32
.\keepaudio.exe --format pcm16
```

---

## Blind test (`--chance`)

```powershell
.\keepaudio.exe --console --chance 50
```

---

## Command-line reference

```
--freq F_HZ                 Tone frequency (default: 1)
--db NEG_DBFS               Tone level in dBFS (default: -100)
--rate SR                   Sample rate in Hz (default: 48000)
--device N                  Output device index; omit for default (WAVE_MAPPER)
--channels 1|2              Mono or stereo (default: 1)
--frames N                  Frames per buffer (default: 1024; clamp: 128..8192)
--buffers K                 Number of queued buffers (default: 8; clamp: 2..32)
--format auto|pcm16|float32 Audio format (default: auto; prefers float for <= -96 dBFS)
--chance P                  1..100; P% chance to exit immediately (blind test)
--list-devices              Show available devices (MessageBox or console if --console)
--console                   Allocate a console for logs/interaction
--install                   Register per-user autostart (HKCU Run); uses current EXE and flags
--install-copy              Copy EXE to %LOCALAPPDATA%\KeepAudio before registering
--startup-name Name         Custom Run-key name (default: KeepAudio)
--uninstall                 Remove the autostart entry (and LocalAppData copy if present)
--help / -h / /?            Show brief usage
```

## Test autostart without reboot

- **Run the exact command from the Run key:**
  - Open `regedit` → `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` → copy the command line for your entry → paste into `Win + R` and press Enter.
---

## Build from source

```bash
gcc -O2 keepaudio_headless.c -lwinmm -o keepaudio.exe -mwindows
```