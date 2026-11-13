# GD Discord Presence (Geode mod)

Shows the level you're playing in Geometry Dash on your Discord profile, with GD Gaming List ranks. Windows only.

## Download
- **GD 2.2091** (Geode 6): https://github.com/pester33/Geode-DIscord/releases/download/latest-build-2.2091/pester.gd-discord-presence.geode
- **GD 2.2081** (Geode 5.11.2): https://github.com/pester33/Geode-DIscord/releases/download/latest-build/pester.gd-discord-presence.geode

Put the `.geode` file in `Geometry Dash/geode/mods`, restart GD, and keep the Discord app open. The mod updates itself after that.

## Discord Application
Discord shows the game name and picture from a Discord Application:
1. Go to https://discord.com/developers/applications and press **New Application**. Its name is what Discord shows ("Playing <name>").
2. Copy the **Application ID** into `DEFAULT_CLIENT_ID` in `src/main.cpp` (or into the mod's settings).
3. Optional: under **Rich Presence -> Art Assets**, upload a picture named `logo` for the big image.
