# HaloCheckpointManager

A practice tool for [Halo: The Master Chief Collection](https://store.steampowered.com/app/976730/Halo_The_Master_Chief_Collection/)

# Download

From the [Releases page.](https://github.com/Burnt-o/HaloCheckpointManager/releases)

# Features

* Back up (dump) to file the games checkpoints, so you can inject them again at any later time.
* UI for managing your checkpoint collection (ordering, sorting, renaming etc).
* Single-player cheats to help you practice and route the games, such as:
  * Force custom checkpoints & back them up
  * Invincibility
  * Speedhack (both slow-down and fast-forward)
  * Block the game's natural checkpoints
  * Teleport and Launch the player
  * And more (specific games have different features)
* Support for multiple downpatched versions of MCC - Season 7 (2448), Season 8 (2645), and of course current patch.

# Building from source

To build the source yourself, you'll need:

* [Visual Studio](https://visualstudio.microsoft.com/) ([or just the build tools](https://aka.ms/vs/stable/vs_BuildTools.exe) if using another IDE) with the following:
  * Workloads -> Desktop development with C++
  * Workloads -> .NET desktop build tools
  * Components -> .NET 7.0 Runtime
  * Components -> Windows 10 SDK (or higher)
* [vcpkg](https://github.com/microsoft/vcpkg), on path or set via VCPKG_ROOT env var.

And make sure to initialize submodules with `git submodule update --init --recursive`

This project also makes heavy use of the amazing SafetyHook by cursey. You can think of it as like "Microsoft Detours but if it didn't suck". I've included the release I'm using in this repository so you shouldn't need to worry about it.
