Welcome to reNut (Banjo-Kazooie: Nuts & Bolts Recomp)

Created with <a href="https://github.com/rexglue/rexglue-sdk">Rexglue-SDK</a>





Credits
------------------------------------------------
Rexglue team for creating Rexglue-SDK
<br>
the Rexglue SDK discord for helping with any info they have
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




HOW TO BUILD
-----------------------------------------------------------------------------------
1. Install Rexglue-SDk following the <a href="https://github.com/rexglue/rexglue-sdk/wiki/Guide-%E2%80%90-Getting-Started">wiki</a>
2. install Visual Studio Community edition and ensure you install the desktop development with C++ and make sure you check the box that says C++ clang compiler for windows (note: if you are using Mac or linux you can skip this for you will have to follow the wiki linked in step 1 to build the game)
3. clone/download the repository
4. dump your copy of Banjo-Kazooie: Nuts&bolts and use a tool like ISO-Extract to dump the contents of the iso (INCLUDING THE DEFAULT.XEX FILE)
5. place the contents of the iso in the assets folder you created in step 4 (INCLUDING THE DEFAULT.XEX FILE)
6. run rexglue codegen renut_config.toml to generate the files for compiling in the generated folder
7. open the folder in visual studio, go into cmake targets view
8. change the configuration to win-amd64-relwithdebinfo
9. right click reNut project and select build all
10. copy the assets folder with the dumped contents of the iso in out/build/win-amd64-relwithdebinfo

PREFFERED METHOD OF RUNNING THE GAME
-----------------------------------------------------------
open a terminal and do .\renut --gpu_allow_invalid_fetch_constants=true to prevent some hangs in game

if your using something like cmd or similar you only need to run renut --gpu_allow_invalid_fetch_constants=true 


CURRENT ISSUE!!!!
------------------------------------------------------------------------------
THE GAME CRASHES WHEN YOU LEAVE AN AREA BACK INTO SHOWDOWN TOWN, THE CURRENT WORKAROUND IS TO CLOSE THE GAME INSTEAD AFTER IT AUTO SAVES THEN REOPEN, THIS WILL SPAWN BANJO BACK INTO SHOWDOWN TOWN WITH THE JIGGIES YOU'VE COLLECTED

BANJOS WRISTS LIKE TO SHATTER AMONGST OTHER CHARACTERS, IT IS NOT A HORRENDOUS THING JUST FUNNILY NOTICEABLE BUT DOES NOT INHIBIT GAMEPLAY




MAKING ISSUES
--------------------------------------------------------------------------
Remember that this game and rexglue are in an experimental state, there are some issues i myself cannot fix and there may even be some that rexglue cant fix
if you make an issue you will ned to be a bit specific 



