# Far Cry 2: Jackal Fix
<img width="2000" height="877" alt="Far Cry 2: Jackal Fix" src="https://github.com/user-attachments/assets/fd15de0e-e7d8-43eb-af63-174af4cd4bd7"/>

## Installation
The latest version of [Far Cry 2: Jackal Fix](https://github.com/Joshhhuaaa/FC2JackalFix/releases) can be found in the Releases page.

### Game Setup
- After downloading Far Cry 2: Jackal Fix, extract the contents to your Far Cry 2 directory and overwrite all existing files when prompted.
- You can adjust additional settings in `FC2JackalFix.ini` located in the `bin\Plugins` folder.

## Features

### Display
- **Display Type** - Allows you to change between Fullscreen, Borderless and Windowed modes.
- **FPS Limiter** - Caps the game's frame rate using the built-in engine limiter, either at your monitor's refresh rate automatically or at a value you pick.
- **internal Resolution** - Added the option to change the internal resolution to a custom value, to allow for supersampling and downscaling

#### Graphics
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures instead of using the game's inconsistent defaults.
- **Console Gamma** - Restores the Xbox 360's darker gamma look with deeper blacks and stronger contrast.
- **Enhanced LODs** - Improves level of detail (LOD) distances for models, terrain, trees, and object clusters, reducing the distance at which they switch to lower detail or stop rendering (Ultra High preset only).
- **Enhanced Shadow Range** - Extends shadow draw distance, allowing more vegetation to cast shadows and keeping static ambient shadows visible farther away (Ultra High preset only).
- **Shadow Resolution** - Adjusts the resolution of sun and cascaded shadow maps.

### Gameplay
- **Remove Mouse Speed Cap** - Removes the engine's mouse movement speed limit.
- **Mouse Look Sensitivity** - Adjusts mouse look sensitivity beyond the in-game slider.
- **Sprint Turn Modifier** - Controls how much horizontal look speed is reduced while sprinting.
- **Aim Toggle** - Toggles aiming down sights with a tap instead of requiring the button to be held.
- **Sprint Toggle** - Toggles sprinting with a tap instead of requiring the button to be held.
- **Limited Saving** - Enables console-style saving by removing Save Game from the pause menu and disabling F5/F9 quick save/load.
- **Console Autosaves** - Displays the save box after mission completion, matching the console versions.
- **No Blinking Items** - Prevents weapons, ammo, health boxes, diamond cases, beds, and other interactables from flashing.
- **No Colored Signs** - Prevents road and safe house signs from tinting red or blue when they belong to an active objective.
- **Lookback** - Included dedicated lookback button

### Field of View
- **Field of View** - Adjusts the base gameplay field of view.
- **Viewmodel Field of View** - Adjusts the first-person weapon and arms field of view.
- **Ironsight Field of View** - Adjusts the field of view while aiming down sights without affecting magnified scopes.
- **Vehicle Field of View** - Adjusts the field of view while driving vehicles.

### Controller
- **Controller Look Sensitivity** - Adjusts controller look sensitivity beyond the in-game slider.
- **Aim Assist** - Enables or disables controller aim assist.
- **Vibration** - Restores controller vibration support.

### Fixes
- Fixed an issue where jump height was reduced at high FPS
- Fixed an issue where NPCs would bounce at high FPS without disabling rigid characters
- Fixed an issue where the game could crash when changing the resolution at the main menu
- Fixed an issue with hang gliders falling out of the sky when shot
- Fixed an issue gliders bouncing on water
- Fixed an issue with bug truck sounds being silent
- Fixed an issue with the MAC-10 not being heard by enemies while shooting
- Fixed an issue with player not walking slower when using ironsights with the M79
- Fixed an issue where the GPS was misaligned in vehicles
- Fixed an issue with assassination targets having the same vision as snipers, allowing them to see you further than regular enemies
- Fixed an issue where small scattered objects and vegetation would draw without casting a shadow (Ultra High only)
- Fixed an issue where road textures looked abnormal with a higher LOD scale setting (Ultra High, applies with Enhanced LODs)
- Fixed a case where the player could no longer save their game after completing certain missions
- Fixed an issue where buddies were not considered missing if the player helped Father Maliya at the church

### Content Unlocks
- **Predecessor Tapes Unlock** - Unlocks the seven Predecessor Tape missions, which were originally bonus content available through Ubisoft's servers.
- **Machetes Unlock** - Unlocks the Primitive and Homemade machete skins, which were originally bonus content available through Ubisoft's servers.

### General
- **Skip Intro** - Skips the Ubisoft, Dunia, and rating screens.
- **Skip Title Screen** - Skips the "Press any key" title screen and boots straight to the main menu.
- **CPU Affinity** - Limits the game to the first logical processor (may improve performance on systems with many CPU cores).
- **High Precision Timer** - Forces 1ms timer resolution in Window, so the loading screen reaches the proper 30 FPS.
- **Skip System Detection** - Skips redundant hardware detection, making game startup up much faster.
- **Large Address Aware** - Patches the executable to increase the memory limit from 2 GB to 4 GB (may improve stability).
 
### Debug
- Added various debug, these have to be applied to FC2JackalFix.ini beneath the ** [Debug]** section:
- Invincibility = 0
- InfiniteAmmo = 0
- UnlockAllWeapons = 0
- Diamonds = 0
- Noclip = 0
- NoclipKey = F1
- Freecam = 0
- FreecamKey = F2
