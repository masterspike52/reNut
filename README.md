<img width="1920" height="1080" alt="renut logo" src="https://github.com/user-attachments/assets/273bee28-755f-4494-920f-9333af72091e" />



Created using the <a href="https://github.com/rexglue/rexglue-sdk">Rexglue-SDK</a>


Discord
----------------------------------------------
https://discord.gg/D5Bz2ZsHdY - you can join the discord here


About
--------------------------------------------
* Renut is a Recompilation of Banjo-Kazooie: Nuts and Bolts using the Rexglue-SDK

* the code has heavy ai useage, your opinion on ai useage means very little to me, if you have a problem with it you're free to fork the repo and redo it your own way (this is the purpose of open source projects)

* Renut does have a native renderer in which the games D3D functions are natively hooked to run through plume as well as compiles its recompiled shaders from xenosrecomp into its executables which makes the latest release different from previous releases which used the xenos emulation pipeline that rexglue provides for debugging



What's currently supported?
-----------------------------------------
currently the only devices supported are linux devices and windows devices, i have not gotten around to adding metal support for mac nor do i know if itll be worth it (if you use a mac like i said above, its open source, you're free to fork it)




Android
--------------------------------------
* arm64, Android 11 or newer, Vulkan. Get the apk from <a href="https://github.com/etonedemid/reNut-android/releases">reNut-android releases</a>

* copy your iso to /sdcard/reNut or /sdcard/Download, use Extract on the Set Up Game page, then Play. Saves, the workshop and custom Adreno drivers (zips in /sdcard/reNut/drivers) are in the same menu

* to build it: android/configure.sh, then android/build.sh (Android SDK, NDK 29 and a host build of the rexglue CLI for codegen). The rexglue SDK is used as is



 How to play
 ---------------------------------------------------
 * acquire your banjo-kazooie: nuts and bolts (usa) iso (you do not need the update for its not supported)

* download <a href="https://github.com/etonedemid/NutStaller/releases">nutstaller</a>

* open nutstaller, and click do everything and select your iso from the first step

* click play

Online play
--------------------------------------
* online play uses Epic Online Services (for windows this includes a dll for linux i havent tested but should be part of the appimage)

* if you wish to play online you must press F5 and check the signed into live box (this spoofs your xbox live to be Gold so you can get into the xbox live option) as well as set your name itll look like this
  <img width="562" height="129" alt="image" src="https://github.com/user-attachments/assets/76be801c-a6e2-4ff9-9629-860ee72b5f01" />

* pressing F6 opens the friends list and displays your online id (also known as a machine id)

* to add friends you press F6 and send them your friend id or they can send you theirs in which you can copy paste it into the online id box and click send friend request
<br>
  <img width="640" height="147" alt="image" src="https://github.com/user-attachments/assets/c19c2008-c676-4e24-a7ae-90945446aafd" />


* the friend will see an invite in the F6 imgui that they can accept their new friend

* go into the game and select multiplayer > Xbox Live

* hit RB then select your friend to join them


things i havent tested for online
-------------------------------------
* matchmaking, reasoning is it may require a server, if it does i will not be implementing this

Things that dont work
----------------------------------
leaderboards, this is because it does require a server unless i wanna set up 100+ things in the EOS dev portal for every mission and other things that use the leaderboard (thanks rare)

Mods
--------------------------------------------
* mods made with https://github.com/weighta/NB-Studio-Banjo-Kazooie-Nuts-and-Bolts-World-Editor- made by XephyrCraft, they make .nbpatch files.

* to use the nbpatch files all you have to do is make a mods folder and place them in (its that simple)

* depending on the size of the mod the game could take a while to open (this only happens once) and not all mods work (this will be trial and error)

* we can now add custom parts following <a href="https://github.com/masterspike52/reNut/blob/main/docs/modding/custom-parts.md">custom-parts.md</a>



What's different
---------------------------------------------------
* aside from whats above, i had to scrap some things that weren't necessary, this includes the graphics options that were really in there for coping with the xenos emulation pipeline as well as mouse and keyboard support *(you can still use rexglues mnk which should work better than before)

* in return we now have a slew of new graphical options that work natively with the game like 4K resolution, widescreen, increased shadows, anisotropic filtering, and more. 



Warning
------------------------------------------------
if you have any issues please make an issue after ensuring you have covered things such as 

* updating your graphics drivers
* making sure the game is using your dedicated GPU and not your APU
* Making sure it happens more than once (by closing and reopening the game)

* if you have ensured the above the best way to go about it is to use <a href="https://renderdoc.org/">Renderdoc</a> and collect me render captures that i can then use to solve the issue

* if a crash happens please provide the crash dump and the log file associated with it 

Credits
------------------------------------------------
<a href="https://github.com/rexglue/rexglue-sdk">Rexglue team</a> for creating Rexglue-SDK
<br>
the <a href="https://discord.gg/ksm4vVbBf">Rexglue SDK discord</a> for helping with any info they have
<br>
SolarCookies for a handful of midasm hooks 
<br>
ValcomDrifty for the renut logo
<br>
OlieGamerTV for the information and documentations made for nuts and bolts specifically as well as a multitude of other things as well as assisting with other things about nuts and bolts
<br>
XephyrCraft for helping test mods made with his tool as well as testing the game
<br>
Leia and eli for testing renut (as well as anyone else who tested renut on windows)
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
