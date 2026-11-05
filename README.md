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

Prerequisites:
 * Visual Studio 2022 Build Tools: v143 C++ toolset + Windows 10/11 SDK
 * CMake >= 3.28 and Ninja, expected on PATH.
 * .NET SDK 7+
 * [vcpkg](https://github.com/microsoft/vcpkg), on path or set via VCPKG_ROOT env var.

After cloning, initialize submodules with `git submodule update --init --recursive`

The native C++ projects (HCMInterproc, HCMSpeedhack, HCMInternal) build with CMake.
Open through CMakeLists.txt in root (Rider, CLion) or as a folder (Visual Studio).
Requires MSVC environment (IDEs will autodiscover, or run from "x64 Native Tools Command Prompt"):

    cmake --preset default
    cmake --build build --config Debug    # or: Release

DLLs + InternalPointerData.xml land in `HCMExternal/bin/x64/<config>/net7.0-windows`.

The managed C# app uses MSBuild.
Open through HaloCheckpointManager.sln in root:

    dotnet build HCMExternal/HCMExternal.csproj

This project also makes heavy use of the amazing SafetyHook by cursey. You can think of it as like "Microsoft Detours but if it didn't suck".
