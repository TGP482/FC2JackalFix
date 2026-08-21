# Far Cry 2: Jackal Fix
<img width="2000" height="877" alt="image (2)" src="https://github.com/user-attachments/assets/2d9357dc-5c84-432a-b452-7413448e9937" />

## Installation
The latest version of [Far Cry 2: Jackal Fix](https://github.com/TGP482/FC2JackalFix/releases) can be found in the Releases page.

### Game Setup
- After downloading Far Cry 2: Jackal Fix, extract the contents to your Far Cry 2 directory and overwrite all existing files when prompted.
- You can adjust the mod settings inside the in-game options menu whilst the game is running or alternatively inside `FC2JackalFix.ini` located in the `bin\Plugins` folder.
  <img width="3840" height="2160" alt="20C67D~1" src="https://github.com/user-attachments/assets/fe3e1621-a8eb-42c6-9b0e-fb3f7552d555" />


## Features

### Display
- **Display Type** - Allows you to change between Fullscreen, Borderless and Windowed modes.
- **FPS Limiter** - Caps the game's frame rate using the built-in engine limiter, either at your monitor's refresh rate automatically or at a value you pick.
- **Internal Resolution** - Added the option to change the internal resolution to a custom value, to allow for supersampling and downscaling
- **FPS Counter** - Shows the engine's built-in frame rate counter.

### Graphics
- **Improved Utilisation** - Improves GPU and CPU utilisation, increasing performance across the board
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/aad65d76-2e53-43bd-b389-902ebd843700"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/eed3b612-2a68-42bb-83ec-4ad895dc37e4"></td>
    </tr>
    <tr>
      <td align="center">Improved Utilisation Off</td>
      <td align="center">Improved Utilisation On</td>
    </tr>
  </table>
</div>

- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures instead of using the game's inconsistent defaults.
- **No Rim Lighting** - Removes the rim lighting glow on the edges of characters, mainly visible at night
- **Saturation** - Adjust the saturation levels from the 0.5 default, rage: 0.0 - 1.0

### Beyond Ultra
- **Beyond Ultra Geometry** - Increases past the Ultra High geometry values: (0) default, (1) 2x, (2) 4x, (3) 6x, (4) max draw distance
- **Beyond Ultra Shadows** - Increases past the Ultra High shadow values: (0) default, (1) max draw distance
- **Beyond Ultra Terrain** - Increases past the Ultra High terrain values: (0) default, (1) 2x, (2) 4x, (3) max draw distance

### Gameplay
- **Remove Mouse Speed Cap** - Removes the engine's mouse movement speed limit.
- **Mouse Look Sensitivity** - Adjusts mouse look sensitivity beyond the in-game slider.
- **Sprint Turn Modifier** - Controls how much horizontal look speed is reduced while sprinting.
- **Aim Toggle** - Toggles aiming down sights with a tap instead of requiring the button to be held.
- **Sprint Toggle** - Toggles sprinting with a tap instead of requiring the button to be held.
- **Limited Saving** - Enables console-style saving by removing Save Game from the pause menu and disabling F5/F9 quick save/load.
- **Console Autosaves** - Displays the save box after mission completion, matching the console versions.
<img width="400" height="224" alt="ConsoleAutosave" src="https://github.com/user-attachments/assets/0be95623-8b88-4a51-b09b-c5683d450228" />

- **No Blinking Items** - Prevents weapons, ammo, health boxes, diamond cases, beds, and other interactables from flashing.
- **No Hit Indicator** - Disables the directional hit indicators.

### Field of View
- **Field of View** - Adjusts the base gameplay field of view.
- **Viewmodel Field of View** - Adjusts the first-person weapon and arms field of view.
- **Ironsight Field of View** - Adjusts the field of view while aiming down sights without affecting magnified scopes.
- **Vehicle Field of View** - Adjusts the field of view while driving vehicles.

### Controller
- **Controller Look Sensitivity** - Adjusts controller look sensitivity beyond the in-game slider.
- **Aim Assist** - Enables or disables controller aim assist.
- **Vibration** - Restores controller vibration support.
- **Prompts** - Restored Xbox 360 button prompts.

### Jackal Fixes
- Fixed an issue where jump height was reduced at high FPS
- Fixed an issue where NPCs would bounce at high FPS without disabling rigid characters
- Fixed an issue where sound effects stuttered or went silent at high FPS
- Fixed an issue where the diamond counter rolled up faster at high FPS
- Fixed an issue where character's hair, glasses and water rendered over certain smoke, dust and other 2D effects
- Fixed an issue where road textures would break when increasing the LodScale config
- Fixed an issue where foliage flickered in and out of existence at higher geometry draw distances, by raising the tree renderer's fixed per frame instance budget (Ultra High)
- Fixed various HUD and menu UI elements being stretched in widescreen and ultrawide, in addition to restoring console HUD offsets
- Fixed an issue where the game could crash when changing the resolution at the main menu
- Fixed an issue where alt tabbing out of exclusive fullscreen returned to the game windowed and at a lower resolution
- Fixed an issue where borderless did not fill the screen at resolutions below the desktop, shrinking the window into the top left corner
### Community Fixes
- Fixed an issue with hang gliders falling out of the sky when shot
- Fixed an issue gliders bouncing on water
- Fixed an issue with bug truck sounds being silent
- Fixed an issue with the MAC-10 not being heard by enemies while shooting
- Fixed an issue with player not walking slower when using ironsights with the M79
- Fixed an issue where the GPS was misaligned in vehicles
- Fixed an issue with assassination targets having the same vision as snipers, allowing them to see you further than regular enemies
- Fixed an issue where small scattered objects and vegetation would draw without casting a shadow (Ultra High only)
- Fixed an issue where road textures broke up at higher geometry LOD scales (Ultra High)
- Fixed an issue where the DLC ATV and Utility Truck only ever spawned in one colour, while their multiplayer counterparts came in the full range
- Fixed a case where the player could no longer save their game after completing certain missions
- Fixed an issue where buddies were not considered missing if the player helped Father Maliya at the church
- Fixed an issue with tape 9 repeating 
- Fixed an issue where black squares flickered across the screen when using DirectX 10 with Bloom enabled

### Content Unlocks
- **Predecessor Tapes Unlock** - Unlocks the seven Predecessor Tape missions, which were originally bonus content available through Ubisoft's servers.
- **Machetes Unlock** - Unlocks the Primitive and Homemade machete skins, which were originally bonus content available through Ubisoft's servers.

### General
- **Skip Intro** - Skips the Ubisoft, Dunia, and rating screens.
- **Skip Title Screen** - Skips the "Press any key" title screen and boots straight to the main menu.
- **Skip Tutorials** - Skips tutorial messages: (0) default, (1) the pop-ups that interrupt gameplay, (2) also the floating on-screen hints
- **CPU Affinity** - Limits the game to the first logical processor (may improve performance on systems with many CPU cores).
- **High Precision Timer** - Forces 1ms timer resolution in Window, so the loading screen reaches the proper 30 FPS.
- **Skip System Detection** - Skips redundant hardware detection, making game startup up much faster.
- **Large Address Aware** - Patches the executable to increase the memory limit from 2 GB to 4 GB (may improve stability).
 
### Debug
- **Debug Options** - Apply these to FC2JackalFix.ini beneath the [Debug] section:
```
[Debug]
Invincibility = 0
InfiniteAmmo = 0
UnlockAllWeapons = 0
Diamonds = 0
Noclip = 0
NoclipKey = F1
Freecam = 0
FreecamKey = F2
```

### Credits
- [Boggalog](https://www.nexusmods.com/profile/Boggalog) - Various fixes and [An Almost Complete Guide to Far Cry 2 Modding](https://www.nexusmods.com/farcry2/mods/299)
- [scubrah](https://www.nexusmods.com/profile/scubrah) - Various fixes
- [ThirteenAG](https://github.com/ThirteenAG) - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
- [FoxAhead](https://github.com/FoxAhead) - Various [Far Cry 2 Multi Fixer](https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer) improvements
