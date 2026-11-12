Voyeur (Philips CD-i) for Windows
=================================

This is a PC port of the 1993 Philips CD-i game Voyeur. It runs the game
from YOUR OWN copy of the disc: no game files are included. You need a
disc image of Voyeur in .cue/.bin form (raw 2352-byte sectors).

Getting started
---------------
Installer: run Voyeur-<version>-setup.exe. It asks for the folder with your
  .cue/.bin files, then adds Start menu shortcuts.
Portable zip: unzip Voyeur-<version>-portable.zip anywhere and run
  voyeur.exe. The first time, it asks you to select the .cue or .bin.

On a fresh save, the code-entry screen already holds a stored code, 3333.
Enter it on the keypad and press ACCEPT to start.

Everything stays in this folder
-------------------------------
The program writes only next to itself; nothing goes to AppData or the
registry (except the installer's own uninstall entry):
  saves\               saves (the CD-i NVRAM), settings.ini and
                       settings_hd.ini, disc_path.txt (where your disc
                       image is), recomp.log
  hd_pack\             the HD video pack, once built
To move the game, move the whole folder.

Two versions
------------
voyeur.exe      the faithful version
voyeur_hd.exe   the HD edition:
  * AI-upscaled video, once you have built the HD video pack (below);
  * the 12-15 fps video interpolated to 60 frames per second (F7);
  * run-ahead, which cuts input lag by a frame (F6 cycles 0/1/2).

Controls
--------
Move the pointer   arrow keys (Shift = faster), gamepad stick or D-pad
Button 1 (select)  Z, Space, A          gamepad A
Button 2           X, Ctrl, S           gamepad B, Y
F5                 unlimited camera battery on/off. In Voyeur the battery
                   is also the clock, so time stands still while it is on.
F6 / F7            (HD) run-ahead / 60 fps video
F8                 video upscaling on/off
F9 / F10 / F11     aspect ratio / filter / fullscreen (or Alt+Enter)
Tab (hold)         fast forward
Esc                quit

HD video pack
-------------
"Build HD video pack" (Start menu, or Build HD video pack.cmd here) upscales
every frame of the game's video from your disc with Real-ESRGAN, which is
downloaded from its official GitHub release on first use. It needs a
graphics card with Vulkan support, takes about 10 hours on a mid-range GPU
and uses about 5 GB. You can stop it at any time; running it again carries
on where it stopped. voyeur_hd.exe uses the pack automatically.

Updates
-------
When the game starts it checks GitHub for a newer release. If there is one,
it asks whether to install it; saves and settings are kept. Answer No to
skip that version. To turn the check off, start the game with
--no-update-check.

Other options
-------------
voyeur.exe "path\to\game.cue"   use a disc image without changing the saved one
--unlimited-battery              start with F5 on
--version                        print the version

Source and bug reports: https://github.com/tools81/voyeur-static-recomp
