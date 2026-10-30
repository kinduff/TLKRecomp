The Lion King (SNES) - native PC build (TLKRecomp)
==================================================

This is a static recompilation of The Lion King for the Super Nintendo,
built on snesrecomp. It does NOT include the game: you need your own
copy of the cartridge ROM.

Required ROM
------------
  Lion King, The (USA).sfc
  CRC32  C8FBFAA8
  SHA-256 457abe634e0a8be03e29513a3dca8f3e9d0ddc6bf97d8931f2316094260f3712
  (3 MiB; a copy with a 512-byte copier header is also accepted)
Any other region or revision is rejected.

Running
-------
  ./tlk_sdl --rom "/path/to/Lion King, The (USA).sfc"

or put the .sfc file next to tlk_sdl (or in the current directory) and run
./tlk_sdl with no arguments: it checks every .sfc/.smc there and uses the
first one that matches. If none matches, it asks for the ROM with a file
dialog and remembers the choice in tlk_sdl.ini. You can also put
"rom = /path/to/file.sfc" in tlk_sdl.ini yourself.

Linux x86_64 (glibc). SDL3 is built into tlk_sdl: nothing needs to be
installed. It uses the desktop's own libraries when present: Wayland or
X11 for the window, PipeWire, PulseAudio or ALSA for sound, udev/hidapi
for gamepads. To force one: SDL_VIDEO_DRIVER=x11 (or wayland),
SDL_AUDIO_DRIVER=pulseaudio (or pipewire, alsa). ./tlk_sdl --list-drivers
prints what is compiled in.

Windows x86_64 (the .zip): the same program as tlk_sdl.exe, with SDL3 and
the C runtime built in, so it needs no DLLs. Put the .sfc next to
tlk_sdl.exe and double-click it, or run it from a terminal with --rom as
above. tlk_sdl.ini, the save-state slots and last_session.rec go in the
folder it is started from.

The single .exe (TLKRecomp-windows-x86_64.exe) is the same program
without the folder: put the ROM next to it and double-click it. Its
shader presets and font unpack to %APPDATA%\TLKRecomp on first start.

Linux AppImage (TLKRecomp-x86_64.AppImage): make it executable
(chmod +x) and run it. tlk_sdl.ini, saves and recordings go next to
the .AppImage file, and a ROM put there is found.

macOS on Apple Silicon: the .dmg holds TLKRecomp.app; drag it to
Applications. It is not notarized, so macOS refuses the first start:
then open System Settings > Privacy & Security and click Open Anyway
next to TLKRecomp. Its settings, saves and recordings go to
~/Library/Application Support/TLKRecomp, and a ROM put there is found;
otherwise it asks for the ROM and remembers it. The -macos-arm64 .zip
has the bare tlk_sdl for Terminal use (files go in the current
directory; clear the download flag first with
xattr -dr com.apple.quarantine TLKRecomp-macos-arm64). The macOS build
has been run on one Mac only.

Options: --scale N, --fullscreen, --no-audio, --config file.ini, --help.

Keys (defaults)
---------------
  D-pad           arrow keys
  B / A           Z / X
  Y / X           A / S
  L / R           Q / W
  Start / Select  Enter / Right Shift
  F11, Alt+Enter  fullscreen
  F1-F4           save state, slot 1-4
  F5-F8           load state, slot 1-4 (also Shift+F1-F4)
  ` (hold)        turbo: game runs 4x faster, sound muted
                  (pad: right stick click; rebind it on the Controls
                  page, speed is turbo_speed in tlk_sdl.ini)
  Tab or Esc      settings menu (the game pauses); in the menu Esc goes
                  back, System > Quit (pressed twice) quits

Gamepad: any controller SDL knows. Buttons by position: bottom = B,
right = A, left = Y, top = X, shoulders/triggers = L/R, d-pad or left
stick to move, Start/Back = Start/Select. Guide (or Back+Start) opens the
menu; d-pad moves, A selects, B goes back, Start closes. Pads can be
plugged in while playing.

Settings menu: Up/Down pick a section, Enter opens it, Left/Right
change a value, Esc goes back. Display, Graphics (shader, smoothing,
integer scaling), Audio, Practice, Controls and System (Resume, Quit).
Every change is applied and saved at once. Shaders
(CRT, LCD grid, Sharpen, Composite) are the files in assets/shaders and
need OpenGL 3.3; keep that folder next to the program. Aspect 8:7 shows
square pixels, 4:3 the shape a TV showed. In Controls pick a SNES
button and press Enter (or pad A), then press the new key or the new
gamepad button. Esc / Guide cancels. "Reset defaults" restores the
table above. Settings go to tlk_sdl.ini in the current directory (or
$TLK_SDL_INI). The menu's font is in assets/fonts; keep that folder
next to the program too.

Save states are tlk_slot1.state .. tlk_slot4.state next to the ini. They
only load in the same build that wrote them.

See LICENSE-NOTES.txt before sharing this build.
