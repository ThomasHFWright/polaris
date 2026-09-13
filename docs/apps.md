# Add and edit apps

The **Library** page holds every entry Polaris publishes to clients: the Desktop entry, Steam Big
Picture, and the games and launchers you add. This guide covers the page itself and every field
in the app editor. Artwork sources and fixes are on
[Library sources and artwork](https://papi-ux.com/docs/library-and-artwork/).

## The Library page

**Quick Launch** ranks favourite, recent, and launch-ready apps first so the list feels familiar
from the couch, while still surfacing entries that need attention.

**Published apps** is the list clients see. Drag rows to reorder what appears first. Reordering,
like any change to the published list, makes Polaris rebuild it, which can interrupt a running
session. Open an entry to edit it or to export its `.art` launcher file for front ends that want a
direct launch.

**Import games** scans Steam, Lutris, and Heroic for installed titles, keeps entries that are
already published visible so you can spot what is new, and lets you stage several candidates
before one import pass. Imported Steam titles keep their app id and take the Linux launch mode
you have selected when they start.

Importing a Lutris or Heroic title also publishes an entry for the launcher itself, once, using the command that exists on this host. Those imports launch straight into a game, so without it there is no way to reach the launcher from a stream to install something or fix a login. An entry you added by hand is recognised and not duplicated.

**Library health** shows import coverage and the host context the library depends on. Keep
entries short and recognisable on a handheld screen, use per-app overrides only where a launcher,
tool, or game needs them, and export `.art` entries when you want favourite launches in another
front end.

## The app editor

Saving writes the launcher profile immediately; there is no separate apply step.

### Client entry

| Field | What it does |
| --- | --- |
| **Application Name** | The name shown on Moonlight and Nova. |
| **Image** | The icon, picture, or box image sent to clients. PNG only; when unset, Polaris sends its default box image. |
| **Game Category** | A classification hint for Auto Quality, detected from Steam genres on import. |
| **Platform and runtime** | Filled in for titles imported from Heroic. Says what the title installs as and what will execute it, such as Windows through Proton-GE. Left blank when Heroic did not record it. |
| **Emulated Gamepad Type** | Which gamepad to emulate for this app, overriding the Input tab's default. |
| **MangoHud Overlay** | Shows GPU, CPU, temperature, and frametime in the stream from the host side. |

### Command path

| Field | What it does |
| --- | --- |
| **Command** | The main application to start. Leave it blank to publish an entry that starts nothing, such as the Desktop. |
| **Working Directory** | Passed to the process; some applications look for their configuration there. Defaults to the parent directory of the command. |
| **Output** | A file that receives the command's output. Ignored when unset. |
| **Detached Commands** | Commands run in the background alongside the app. |

### Prep and state commands

| Field | What it does |
| --- | --- |
| **Command Preparations** | Commands run before the app starts and undone after the session ends. Ordinary preparation failures are logged and launch continues; required session infrastructure and game settings profiles abort on failure. |
| **Resume/Pause Commands** | The do command runs when the first client connects to an idle app; the undo command runs when the last client disconnects. Clean up in undo what do sets up. |
| **Global prep and state commands** | Per-app switches that include or exclude the host-wide commands from the General tab for this app. |
| **Allow client prepare commands** | Whether the commands a paired device carries may run when this app starts. |

### Game settings profiles

For a directly published game, **Game settings profiles** selects one set of native
settings edits on a new authenticated paired-client launch, before ordinary prep commands
and the game starter. This works with standard Moonlight; no client changes are needed.
Web launches, input-only sessions and viewers do not apply profiles. Desktop and Big Picture
cannot identify games opened inside them.

Initial support is Cyberpunk's `UserSettings.json`. In the editor, enter its host path in
**Settings file** and a JSON object in **Named option edits (JSON)**. Paths support existing
launch-environment expansion, such as `$(HOME)`. Use the exact option names from your file;
each must identify exactly one option within an `options` array. Only existing `value` and
`index` fields can be edited. Keep each value's existing JSON type (string, number or boolean);
indices must be nonnegative integers. Check the values and indices in your own file; Polaris
does not translate graphics presets between games.

For example, the optional `apps.json` field can contain:

```json
"game-profiles": [
  {
    "name": "TV 4K120",
    "width": 3840, "height": 2160, "fps": 120,
    "file": "/absolute/path/to/Cyberpunk/UserSettings.json",
    "settings": {
      "DLSS": { "value": "Balanced", "index": 3 },
      "DLSSFrameGen": { "value": true }
    }
  }
]
```

Names, `file` and `settings` are required. Empty `settings: {}` with `file: ""` is a no-op,
useful for a specific client or mode that should override a broader profile without edits.
Nonempty settings require a nonempty file path. Missing profiles or no matching profile
leave settings alone. Ordinary app prep and state commands remain separate.

All supplied conditions must match. Client-specific rows (`client-uuid`) outrank any-client
rows; within each group, resolution + FPS outranks resolution alone, then no mode. A row
without conditions is the default. Selectors use the **final host launch mode**, including
render resolution after optimization and app/client scaling. Width and height must be
supplied together as integers from 1 to 32768; FPS requires both and accepts numbers from
1 to 1000 with up to three decimals. These limits do not expand display or encoder support.
Profile and launch FPS round to the nearest whole number, with halves rounded up:
`59.94` → `60`, `119.88` → `120`, `23.976` → `24`, `59.5` → `60`.
Duplicate selectors after rounding are rejected. UUIDs compare case-insensitively; renaming
a client preserves assignments, while unpairing/re-pairing requires reassignment. Saved
unknown UUIDs remain visible as unavailable clients.

Polaris saves a full original backup in a `.polaris-profile.json` sidecar before editing.
After writer shutdown and existing undo commands, it restores **only the fields it edited**
into the latest settings file, preserving other in-game changes. The backup persists after
restoration. Use a regular file owned by the user running Polaris; its file mode is preserved.
Missing or ambiguous options, missing fields and apply failures abort launch. Failed writer
cleanup or restoration blocks another apply rather than overwriting unresolved state.
Outside a private runtime, combining main and detached commands is rejected because writer
ownership cannot be verified. Pause, reconnect and resume do not switch profiles; restart
the game to select another one.

An active crash journal blocks apply until manual recovery. After a daemon crash, reboot or
power loss, stop all game/settings writers and use the saved backup to recover the affected
fields before resolving that journal; automatic crash recovery is not provided. Polaris
cannot exclude externally launched games or cloud-sync writers, so avoid concurrent changes
to the same file while a profile is active.

### Runtime behavior

| Field | What it does |
| --- | --- |
| **Exit Timeout** | Seconds to wait for every app process to exit gracefully when quitting; five by default. Zero or below terminates immediately. |
| **Resolution Scale Factor** | Scales the client-requested resolution: 2000x1000 at 120 percent becomes 2400x1200. Only a value other than 100 percent overrides the client's own factor; the stream mode itself is not affected. |
| **Continue streaming until all app processes exit** | Keeps streaming until every process the app started has ended, instead of stopping when the first one does. |
| **Continue streaming if the application exits quickly** | Detects launcher-type apps that close right after starting something else and treats them as detached. |
| **Terminate on Pause** | Ends the app when the last client disconnects instead of keeping it paused for the resume window. |
| **Close desktop Steam for private launches** | When desktop Steam is running as a private stream starts, quits it and waits for it to exit instead of refusing the launch. Unsaved state in that Steam session is lost. |
| **Per Client App Identity** | Gives the app a separate identity per client, so one app can carry different virtual display configurations for different devices. |
| **Use App Identity** | Creates virtual displays under the app's own identity instead of the client's, so each app gets its own display configuration. |
| **Always create Virtual Display** | Creates a virtual display whenever this app starts, regardless of what the client asked for. Needs the virtual display driver on Windows hosts. |
| **Enforce Virtual Display Primary** | Makes the virtual display primary when the app starts. Kept on by default; known broken on Windows 11 24H2. |

### Environment variables

Every command the app runs receives a set of environment variables describing the session:
client, resolution, frame rate, and the like. The editor lists them under the Reference section.
Variables starting with `SUNSHINE_` are kept for compatibility with tools written for that host;
their `POLARIS_` twins are the current names. `SUNSHINE_CLIENT_FPS` carries a fractional value for
fractional refresh rates; if a script cannot read a floating-point number there, enable **ENVVAR
compatibility mode** on the Advanced tab. `POLARIS_CLIENT_FPS` is always fractional.
