# 86Box MCP server

86Box can act as a [Model Context Protocol](https://modelcontextprotocol.io) server, so an
agent (Claude, an IDE assistant, or any other MCP client) can inspect and drive the virtual
machines the VM manager knows about.

The server lives inside the VM manager process and speaks the **Streamable HTTP** transport on
the loopback interface. It is not an external helper program: it reads the manager's own state,
so the machine list, the running state and the configuration files it reports are exactly the
ones the manager window shows.

## Endpoint

```
http://127.0.0.1:8674/mcp
```

The port is configured in the VM manager's **Preferences** dialog, in the *MCP Server* group:
*Enable MCP server for AI agents* switches the server on and off, *Port* picks the port, and the
line below them shows where the server is currently listening. Accepted changes take effect
immediately, without restarting the manager.

The dialog stores the settings in `vmm.ini`, in 86Box's global configuration directory:

| Key           | Default | Meaning                                                        |
| ------------- | ------- | -------------------------------------------------------------- |
| `mcp_enabled` | `1`     | Set to `0` to switch the server off.                            |
| `mcp_port`    | `8674`  | Port to listen on, loopback only (1024-65535).                  |

The server is enabled by default. If the port is already in use, the manager reports it in the
preferences dialog and stays without an MCP server; pick a free port and accept again.

## Tools

| Tool              | Purpose                                                                                   |
| ----------------- | ----------------------------------------------------------------------------------------- |
| `vm_list`         | List the machines known to the manager, with their display name and running state.        |
| `vm_create`       | Create a machine directory and register it (`copy_from`, `config`, or empty).             |
| `vm_config_get`   | Read a machine's `86box.cfg`: all sections, one section, or one value.                    |
| `vm_config_set`   | Change one value in a machine's `86box.cfg`.                                              |
| `vm_power`        | `start`, `pause`, `reset`, `shutdown`, `force_shutdown`, `ctrl_alt_del`.                  |
| `vm_screenshot`   | Capture a running machine's screen and return it as an image.                             |
| `vm_screen_text`  | Read the same screen as text with OCR, for agents that cannot look at images.             |
| `vm_keyboard`     | Type text or press keys in a running machine.                                             |
| `vm_media`        | List the floppy and optical drives, load media into them, and eject it.                   |
| `vm_performance`  | Live statistics of a running machine, as shown by **Tools > Performance**.                |

Machines are named by their directory name (the VM manager's `config_name`), or by their
display name; `vm` also accepts an absolute path.

### `vm_screenshot`

Returns the frame the emulated display currently holds, as an `image/png` content block plus a
small JSON description (`name`, `monitor`, `width`, `height`, `bytes`). Use `monitor: 2` for a
machine with two screens. Nothing is written to disk, and a paused machine can be captured too
- the frame buffer keeps its last picture.

The capture reads the emulated frame buffer directly (through
`video_capture_frame_monitor()`), so it does not depend on the machine's window being visible,
unlike the screenshot menu entries. Text modes come back at their native resolution, for
example 640x200 for an Amstrad PC1512 BIOS screen, which keeps them readable.

### `vm_screen_text`

The second step after `vm_screenshot`: it captures the very same screen and returns the text
recognized on it, which is what an agent whose model has no image input can work with. It is
also handy when exact text matters (an error message, a directory listing) or when the screen
should be searched.

```json
{
  "name": "8086", "monitor": 1, "width": 640, "height": 200, "scale": 3,
  "engine": "tesseract", "characters": 146,
  "text": "Please set time and date\nPlease set user options (if required)\n...",
  "lines": [ { "text": "Please set time and date", "confidence": 95.0, "x": 0, "y": 24, "width": 191, "height": 7 } ],
  "alternative_engine": "apple-vision",
  "alternative_text": "..."
}
```

Parameters: `vm` and `monitor` as for `vm_screenshot`, plus

| Parameter | Meaning                                                                              |
| --------- | ------------------------------------------------------------------------------------ |
| `engine`  | `auto` (default), `apple-vision` or `tesseract`.                                      |
| `scale`   | Upscale factor (1-6) applied before recognition; `0` picks one from the capture size. |

Engines:

* **apple-vision** uses the macOS Vision framework (10.15+). It is weak-linked, so builds still
  start on older system versions, and no extra software is needed.
* **tesseract** shells out to an installed `tesseract`. It is the better reader for the 8x8
  bitmap fonts of a text mode screen, which is why `auto` runs every available engine and keeps
  the one that read more characters. The other reading comes back as `alternative_text`.

Both engines need the picture enlarged: the capture is upscaled with nearest-neighbour
interpolation to about 2048 pixels on the long side (`scale: 0`, the default), because a 640x200
screen gives each character roughly eight pixels, far less than any OCR engine expects.

Accuracy is imperfect by nature - expect misread characters such as `¢` for `(`, and note that
`vm_screenshot` remains the reliable way to see a screen if the model can render images.

### `vm_keyboard`

Drives the machine the way a person at its keyboard would, which is what closes the loop with
`vm_screenshot`/`vm_screen_text`: answer a BIOS prompt, type a DOS command, confirm a dialog,
then look again.

```json
{ "vm": "8086", "text": "dir", "keys": ["enter"] }
```

| Parameter  | Meaning                                                                                   |
| ---------- | ----------------------------------------------------------------------------------------- |
| `text`     | Text to type, up to 160 characters, on a **US layout**. A newline types Enter.             |
| `keys`     | Keys to press after the text, each a name (`enter`, `esc`, `f1`, `up`, `tab`, `kp5`, ...) or a combination (`ctrl+c`, `ctrl+alt+del`, `shift+tab`). |
| `delay_ms` | Delay between keystrokes; 30 ms by default, raise it for a slow guest.                     |

Every keystroke is a press followed by a release, each spaced by `delay_ms`, so that the emulated
keyboard controller is never handed two codes at once - a BIOS or DOS guest that is busy cannot
lose input that way. The call returns once the whole sequence has been sent, reporting
`keystrokes` and `duration_ms`; a request whose total would exceed 20 seconds is refused, so
long input has to be split. Sending keys to a paused machine is refused as well, because the
guest would not be running to read them.

Unknown key names come back as an error listing everything that is accepted.

### `vm_media`

`action: "list"` reports the machine's drives, what is in them, the images that machine has used
before, and the image files lying next to it:

```json
{
  "name": "98SE", "running": true,
  "floppy": [
    { "drive": "a", "type": "3.5\" 1.44M", "capacity_kb": 1440, "empty": true,
      "candidates": [ { "path": "/Users/me/media/boot.img", "size_kb": 1440, "fits_drive": true } ],
      "recent_images": [ "/Users/me/media/old.img" ] },
    { "drive": "b", "type": "5.25\" 360k", "capacity_kb": 360, "empty": true }
  ],
  "optical": [
    { "drive": "cd1", "type": "ASUS CD-S520/A4 1.6K (52x)", "empty": false,
      "image": "/Users/me/Downloads/NT", "folder": true,
      "candidates": [ { "path": "/Users/me/iso/game.iso", "folder": false } ] }
  ]
}
```

The list is built from the machine's `86box.cfg`, but a running machine is asked directly, because
the configuration leaves out drive types that equal the built-in default (both `a` and `b` are
360k unless changed) and it lists optical slots that are not attached at all. `fits_drive` says
whether a candidate's size matches the capacity of that floppy drive - an image that does not fit
will be refused by the drive.

`action: "load"` takes a `drive` and a `path`:

| Drive            | Meaning                                                                       |
| ---------------- | ----------------------------------------------------------------------------- |
| `a`, `b`         | Floppy drives; also `floppy1`..`floppy4`. `path` is an image file.             |
| `cd1`..`cd8`     | Optical drives; also `cdrom1`..`cdrom8`. `path` is an image **or a folder**.   |

A folder is mounted as a virtual ISO image, the same way the media menu's "load folder" entry
does it, so a directory of files can be presented to a guest that expects a disc. `write_protected`
mounts a floppy read-only. `action: "eject"` takes the media back out.

Loading and ejecting require the machine to be running, exactly like the media menu; the
emulator saves the new media into the machine's configuration, so it survives the session. For a
machine that is stopped, `vm_config_set` can set `fdd_01_fn` or `cdrom_01_image_path` instead.

### `vm_performance`

Numbers come from the running emulator, sampled over a 400 ms window:

* `cpu.speed_percent` – emulated time over real time, as a percentage (`100` = real time);
* `cpu.guest_ms`, `cpu.real_ms` – the two counters behind that ratio;
* `l1` / `l2` – geometry, totals, windowed hits/misses and hit rate of the modelled caches
  (`available: false` when the emulated CPU has no such cache);
* `working_set` – guest memory pages touched during the window (populated by operating systems
  that use paging, so a real-mode DOS machine reports zero, exactly like the window does);
* `paused` – whether the machine is paused.

The sampling counters are only switched on while a sample is taken, and the Performance window
itself is left untouched when it is open.

## Configuration files

`vm_config_set` edits `86box.cfg` line by line, so comments, ordering and line endings survive.
Changing a machine that is currently running is refused unless `"force": true` is passed,
because a running machine rewrites its whole configuration when it exits.

## Connecting a client

The preferences dialog generates the snippet for the port above it, with a **Copy** button that
puts the following on the clipboard:

```json
{
  "mcpServers": {
    "86box": {
      "url": "http://127.0.0.1:8674/mcp"
    }
  }
}
```

Paste it into the configuration file of a client that accepts a remote MCP server over HTTP
(Claude Desktop, Cursor, Cline, ...). Some clients also expect a transport hint, for example
`"type": "http"` next to `url`.

DeepSeek Harness (`mcp-client` plugin row):

```yaml
- name: mcp-client
  config:
    transport: streamable-http
    serverName: 86box
    url: http://127.0.0.1:8674/mcp
```

Claude Code:

```sh
claude mcp add --transport http 86box http://127.0.0.1:8674/mcp
```

## Limits

* The server runs in the **VM manager** process only. A machine started directly
  (`86Box --vmpath <dir>`, without the manager) does not expose it.
* Performance statistics only exist for a machine the manager started, because the emulator
  reports them over the manager's own IPC socket.
* The transport is stateless: one JSON-RPC message per HTTP request, no server-initiated SSE
  stream (`GET` answers `405`, which the specification allows).
