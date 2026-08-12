

<img width="1920" height="1080" alt="renut logo" src="https://github.com/user-attachments/assets/273bee28-755f-4494-920f-9333af72091e" />




Originally created with <a href="https://github.com/rexglue/rexglue-sdk">Rexglue-SDK</a>



DISCORD
--------------------------------------------
We have a discord, please join and direct any questions there. Myself or someone else will happily answer them.

https://discord.gg/D5Bz2ZsHdY



Credits
------------------------------------------------
<a href="https://github.com/rexglue/rexglue-sdk">Rexglue team</a> for creating Rexglue-SDK
<br>
the Rexglue SDK discord for helping with any info they have
<br>
SolarCookies for midasm hooks and future use of CRT functions and the reNut Launcher
<br>
ValcomDrifty for the renut logo
<br>
OlieGamerTV for the information and documentations made for nuts and bolts specifically
<br>
.
<br>
.
<br>
.
<br>
.
<br>
.
<br>
.
<br>
you, the player?
<br>
.


Requirements
----------------------------------------------
the US version of Banjo-Kazooie: Nuts and Bolts (do not get the update, the recomp doesn't use it)

How to play
----------------------------------------------
1. Download Goopie Launcher from https://goopie.xyz/#/downloads
2. run goopie launcher
3. find banjo-kazooie: nuts and bolts in goopie launcher and click select game
4. select your iso for banjo-kazooie nuts and bolts, this will extract the contents to the default location for goopie (you can change it in goopie's settings)
5. click install (this should install the latest release which comes with the game, the dll's needed, and renut.toml)
6. click play when the game opens itll open a gui for setting your paths, you dont have to worry about it just click continue. it was placed there for me so when i deleted builds to rebuild i didnt have to run a command line.

WE HAVE LINUX SUPPORT NOW

How to play Linux
-------------------------
you have 2 ways to do this just like with windows

you can either follow the How to play section for using goopie (which does work on linux)

or you can do the following.

- go to <a href=https://github.com/masterspike52/reNut/releases>releases</a> and download the appimage file from the latest release
- open the appimage file by double clicking it.

- when opening the appimage you will be met with a dialog box for dumping your iso's assets, selecting yes will let you select your iso for Banjo-Kazooie: Nuts and Bolts (US Version) with extract-iso.
- if you already have your iso's assets dumped you can click no and use the in game gui that appears to set your assets folder. the dialog will only show up once

Where everything is in Linux
--------------------------------
- renut.toml and renut.cfg will be located in home/.config/renut/ they are written and read from here because appimages are weird about reading external stuff and renut.cfg will be specific to your locations for game_data_root and such
- saves are located wherever you choose to set the save location in the gui (by default its home/.local/shared/renut/) 


recommended settings in the f4 menu 
-----------------------------------------
- Under Nuts&Bolts > Performance
- target_refreshrate = 1 (defaults to -1 which is the default vsync action for 30fps, 1 allows it to go to 60fps)
- frame_cap = "display" (defaults to off, display means the framerate cant go above your monitors refresh rate, if your monitors refresh rate is above 60hz please use 60 instead)

- Under Nuts&Bolts > Graphics check the follow boxes
- disable_shadows
- disable_msaa
- disable_cao
- disable_motion_blur
  
uncheck the following under the same section
- sync_shader_compile

this is also recommended in windows (however the toml that comes with the windows version should already have these set)

NOTICE
--------------------
you will not have access to the texture replacement stuff due to rexglue-ostentation not working on linux at this time, you will however still have access to the extra menu for the cvars, it'll just show up as the unused town map icon.
