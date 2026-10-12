Red Faction VR for Meta Quest 3 (WinlatorXR) - Beta 0.1.0
===========================================================

Alpine Faction VR running standalone on a Meta Quest 3 through WinlatorXR
(Wine + Box64 + DXVK). No PC is needed while playing.

Tested on: Meta Quest 3, WinlatorXR dawn-33, GOG Red Faction (English).

You need
--------
- Meta Quest 3 with WinlatorXR (dawn-33 or newer).
- Your own Red Faction (2001). The GOG version is converted automatically.
  Other versions: see "Other game versions" below.
- About 1.3 GB free space on the headset.

Install
-------
1. Copy your complete Red Faction folder (the one with RF.exe) to the
   headset as Download/RedFaction.
2. Copy everything from this ZIP into that folder (replace files if asked).
3. In WinlatorXR create a container for the game with these settings:
   - Screen size: custom 2388x1080 (important)
   - DX wrapper: DXVK (Async on)
   - Graphics driver: Wrapper / Turnip
   - XR API: enabled
   - Drive D: = /sdcard/Download (the default)
4. Start the container, open the file manager, go to D:\RedFaction and start
   RedFactionVR.bat. Or create a shortcut for
       wine C:\windows\system32\cmd.exe
   with the arguments
       /c D:\RedFaction\RedFactionVR.bat
5. The first start creates RF_120na.exe and compiles shaders; it takes a
   little longer.

Controls (Quest Touch)
----------------------
Left stick            Move (look up/down + forward to climb ladders / swim)
Left stick click      Holster weapon
Right stick left/right Snap turn
Right stick up/down   Previous / next weapon
Right stick click     Laser sight on/off
Right trigger         Fire
Left trigger          Alternate fire (sniper / precision rifle zoom)
Right grip + trigger  Alternate fire (same as left trigger)
Left grip             Use / grab the weapon with both hands
A                     Crouch
B                     Jump
X                     Reload (or shake the right controller down)
Y                     Headlamp
Left menu button      Pause menu (WinlatorXR's 2D screen, point and click)

Settings
--------
afvr_winlatorxr.ini in the game folder holds the Quest settings (HUD size and
position, menu screen, graphics, CPU cores). Remove the ";" in front of a line
and change the value; most changes apply within a second while playing.

Other game versions
-------------------
Alpine Faction needs the 1.20 North America executable. RedFactionVR.bat
creates RF_120na.exe from the GOG RF.exe (1.21). For the Steam or a retail
version, run the Alpine Faction VR installer for PC once on a PC
(https://github.com/CactusVRStudios/alpinefactionVR/releases) and copy the
resulting folder (it contains RF_120na.exe) to the headset.

Known issues
------------
- Beta: tested in the first levels only.
- Menus use WinlatorXR's 2D screen; set menu_2d=0 in the ini to show them
  inside VR instead.
- Frame rate is limited to 72 fps by the container (DXVK frame rate setting).
- Multiplayer is untested on Quest.

Credits
-------
Alpine Faction by GooberRF and contributors, Alpine Faction VR by Cactus VR
Studios, WinlatorXR port by SgtBilko76. Red Faction is (c) THQ Nordic.
No game files are included (the RF-1.21.exe.mbsdiff patch only converts your
own RF.exe). License: Mozilla Public License 2.0, see LICENSE.txt and
licensing-info.txt.
