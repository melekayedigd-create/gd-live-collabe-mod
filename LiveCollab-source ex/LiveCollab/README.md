# Live Collab — Geode mod for Geometry Dash (2.2081)

Build levels with a friend **live**, from two different devices.

- **Host** button (main menu, left side) → creates a room, shows a 6-letter code and copies it.
- **Join** button → type (or paste) your friend's code.
- Both open a level in the editor → the host's level is sent to the guest, then every
  object placed / moved / edited / deleted by either player appears for both.

Works over Wi-Fi or mobile data, different houses/cities are fine. It uses a free public
relay server (broker.hivemq.com, with broker.emqx.io and test.mosquitto.org as backups),
so nobody has to open ports or know an IP address.

## 1. Build the .geode file (GitHub Actions)

1. Create a new GitHub repository (e.g. `gd-live-collab`).
2. Upload **everything in this folder**, keeping the structure:
   ```
   .github/workflows/build.yml
   src/main.cpp  src/Net.cpp  src/Net.hpp  src/Session.cpp  src/Session.hpp  src/SyncLogic.hpp
   CMakeLists.txt  mod.json  about.md  README.md
   ```
   ⚠️ `.github/workflows/build.yml` must be at the repository root, not inside a subfolder.
3. Go to the **Actions** tab → *Build Live Collab* → it starts automatically after the upload
   (or press **Run workflow**). It takes ~5–10 minutes.
4. When it is green, open the run and download the artifact **LiveCollab-mod** (a `.zip`).
   Inside is `myriam.live-collab.geode`.

If a step is red, open it and send me the error lines.

## 2. Send it with Messenger

Send the **`LiveCollab-mod.zip` exactly as downloaded** (a few MB, far under Messenger's 25 MB limit).
Messenger accepts `.zip` files; a bare `.geode` file may be refused or renamed, so keep it zipped.

## 3. Install on the tablet / the friend's phone

1. Open the `.zip` from Messenger (or from Downloads) with the **Files** app and extract it.
2. Move `myriam.live-collab.geode` into:
   `Android/media/com.geode.launcher/game/geode/mods/`
3. Restart Geometry Dash. Both players need Geode **v5.11.2 or newer** (Geode updates itself).

## 4. Play

| Host | Guest |
|---|---|
| Main menu → **Host** → send the code in Messenger | Main menu → **Join** → type the code → **Join** |
| Open the level to build in the editor | Create a **new empty level** and open it in the editor |
| | The host's level appears ("Level received from the host!") |

A small label at the top of the editor shows the room. Press **Host** or **Join** again to leave.

### Good to know
- The guest's level is **replaced** by the host's level — always use a new empty level as guest.
- Synced: objects and all their properties (position, rotation, colors, groups, triggers…).
  Not synced: level settings (song, background, start options).
- Nothing is synced during play-testing; it catches up when you stop.
- Very big levels (10 000+ objects) may make the sync heavier on older tablets.
- The relay server is public: anyone who knows your room code could see the level. Codes are
  random, so just don't post them publicly.
