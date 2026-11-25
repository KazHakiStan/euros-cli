# euros-cli

The terminal client for [Euros](https://github.com/KazHakiStan/euros): live chat, letters that burn
after reading, the forum and pictures, in a plain IRC-style terminal chat. It talks to the same
backend as the web client and does everything it does.

C11 with libcurl (HTTPS and WebSocket) and libsodium; cJSON and stb_image / stb_image_write are
vendored in `third_party/`. No curses: the input line is pinned under the output by a small line editor.

## Install

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/KazHakiStan/euros-cli/master/install.ps1 | iex
```
```sh
# Linux x86_64, macOS (Apple Silicon)
curl -fsSL https://raw.githubusercontent.com/KazHakiStan/euros-cli/master/install.sh | sh
```

The installer downloads the latest release, checks it against `SHA256SUMS`, puts it in
`%LOCALAPPDATA%\Programs\euros` (Windows) or `~/.local/bin` and adds that folder to PATH.
Open a new terminal afterwards.

Options (environment): `EUROS_VERSION=v0.2.0`, `EUROS_INSTALL_DIR`, `EUROS_NO_MODIFY_PATH=1`,
`EUROS_DOWNLOAD_BASE` (a mirror). Uninstall: `... | sh -s -- --uninstall`, or on Windows
`& ([scriptblock]::Create((irm .../install.ps1))) -Uninstall`.

Once a day euros checks for a new release (silently, 3 s timeout) and says so if there is one.
`euros update` installs it, `euros version` just checks. Turn the check off with `EUROS_NO_UPDATE_CHECK=1`.

## Use

```
euros login [--not-my-device]   personal password, or an invite code to join
euros whoami
euros invite                    create an invite
euros invites                   list your invites
euros chat [group]              live room: #everyone or one of your groups (/who, /letters, /img <file>, /fs, /n, /quit)
euros rooms                     #everyone and your groups
euros letters                   unread letters, oldest first
euros read [n]                  show letter n; Enter burns it and tells the sender
euros write <nick> [--ttl D]    the text from the keyboard (end with a single "."), or a pipe;
                                unread, it burns after D = 1, 7 (default) or 30 days
euros forum                     boards; `euros forum <board>`: its threads
euros forum read <id>           a thread with replies (id: the 6 characters shown next to it)
euros forum post <board> [--title T] [--ttl 1|7|30]
euros forum reply <id>
euros forum delete <id> [n]     the thread, or its reply n (yours; the founder: any)
euros forum pin|unpin|lock|unlock <id> · new-board <name> [description]   founder
euros notifs [--all] [--keep]   what the chat noticed while you weren't looking; then it is all read
euros notifs read [room | n]    mark read: all, one room (`room:<id>`) or item n · `euros notifs clear`
euros show <file>               draw a picture the way the chat would: checks your terminal
euros update [--yes]            install the latest release
euros logout
```

The session lives in `%APPDATA%\euros\session.json` (Windows) or `~/.config/euros/session.json`,
readable only by you. With `--not-my-device` the letter key is not kept there: `read` and `write`
ask for the personal password instead.

In the chat a line that starts with `:` is a vim-like command line: `:q` leaves, `:fs` toggles full
screen, `:who`, `:letters`, `:img <file>` work like their `/` versions, Esc drops the line, and `::)`
sends `:)`. `/fs` (or `:fs`) switches to a full screen: the alternate screen with the messages above a
rule and the input on the last line; it opens with the recent messages, and closing it prints
whatever came meanwhile into the normal scrollback.

### Notifications

While `euros chat` runs it also listens to your other rooms (#everyone and every group of yours) on
the same connection, without presence: nobody sees you there and you don't answer for their memory.
What you haven't seen becomes a notification: mentions (`@you`) with their text, other messages only
counted per room (their words stay in the room), new letters and read receipts. Messages in the room
you are in count only while the terminal window is not focused (focus reporting: WezTerm, iTerm2,
kitty; in tmux `set -g focus-events on`), and coming back to the window marks that room read.

`/n` or `:n` in the chat (or `euros notifs` outside) lists them and marks them read; `:n all` shows
the recent read ones too. They are kept in `~/.config/euros/notifications.json` (`%APPDATA%\euros\`
on Windows), readable only by you, for the terminal to show:

```json
{ "unread": 3, "mentions": 1,
  "items": [ { "n": 16, "ts": 1764079200000, "kind": "mention", "room": "devs", "topic": "room:<id>",
               "from": "Nick", "text": "@you look", "pane": "12", "read": false }, ... ] }
```

`kind` is `mention`, `message`, `letter` or `receipt`; `pane` is the `WEZTERM_PANE` of the chat that
noticed it. A status bar reads `unread` / `mentions`; changes go through `euros notifs read …`, which
takes the file's lock. Two chats running at once don't count a message twice.

A letter opens on the terminal's alternate screen, so once it burns nothing is left in the scrollback.

### Pictures

`/img <file>` in the chat sends a JPEG, PNG, GIF or BMP, shrunk to a JPEG that fits one message.
Pictures are drawn in terminals that speak a picture protocol, chosen automatically:

| Terminal | Protocol |
|---|---|
| kitty, Ghostty | kitty graphics |
| WezTerm, iTerm2 | iTerm2 inline images (kitty graphics in WezTerm needs `config.enable_kitty_graphics = true`) |
| WezTerm, iTerm2 inside tmux | sixel |
| others | `[picture 640x480]` |

Override with `EUROS_IMAGES=kitty|iterm|sixel|off`.

Inside tmux (≥ 3.4, built with sixel, as Homebrew's is) pictures are sent as sixel: tmux keeps them in
the pane, so they survive switching windows and arrive in a window you are not looking at. If tmux
does not draw them, tell it the terminal can: `set -as terminal-features ',xterm-256color:sixel'`.
kitty graphics and `EUROS_IMAGES=iterm` go through tmux passthrough instead, which needs
`set -g allow-passthrough on` and is lost on the next redraw. On Windows pictures show up only if ConPTY passes the
escape sequences through; `euros show <file>` tells quickly whether yours does.

## Build

Needs CMake ≥ 3.20, a C11 compiler, pkg-config, libcurl ≥ 8.11 (WebSocket), libsodium, pthreads.

```bash
# Windows (MSYS2, mingw64 shell)
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,curl,libsodium}
# macOS
brew install cmake ninja pkg-config curl libsodium
#   Homebrew's curl is keg-only: export PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig"
# Debian/Ubuntu (curl ≥ 8.11 is in Debian 13 / Ubuntu 25.04+)
sudo apt install build-essential cmake ninja-build pkg-config libcurl4-openssl-dev libsodium-dev

cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The backend defaults to the Euros project; point it elsewhere with `EUROS_URL` / `EUROS_ANON_KEY`.

There is no shared code with the web client, only a shared contract in the
[euros](https://github.com/KazHakiStan/euros) repository: `docs/crypto.md` (personal key, login,
E2E letters), `docs/protocol.md` (live rooms) and `docs/crypto-vectors.json`, copied to `tests/`.
The tests check the same vectors, so both clients derive byte-identical keys.

## Layout

```
src/main.c       commands
src/chat.*       `chat` / `rooms`: the live room, line-mode output
src/letters.*    `letters` / `read` / `write`
src/forum.*      `forum`: boards, threads, replies, founder tools, "new" marks
src/picture.*    pictures: terminal protocol, decode / shrink / JPEG, kitty and iTerm2 output
src/notify.*     notifications: the file, `notifs`, the lines shown by :n
src/update.*     release check, `version` / `update`
src/room.*       room protocol: message validation, room memory (no I/O)
src/realtime.*   Phoenix Channels over one WebSocket: many channels, join, heartbeat, broadcast, presence, postgres_changes
src/ws.*         WebSocket over libcurl
src/api.*        Supabase Auth and PostgREST calls
src/keys.*       personal key: normalize, generate, derive; letter encryption
src/markup.*     forum markup (**bold**, > quote, links as OSC 8 hyperlinks)
src/lineedit.*   raw keyboard with the input line pinned under the output
src/input.*      stdin reader thread for pipes
src/platform.*   OS differences: config dir, hidden input, owner-only files, UTF-8 console and arguments
src/term.*, cli.*, http.*, session.*, config.*, version.*   shared helpers
tests/           crypto vectors, room protocol, letters, markup, pictures, versions, notifications
third_party/     cJSON (MIT), stb_image and stb_image_write (MIT / public domain)
install.sh, install.ps1, scripts/, .github/workflows/release.yml   installers and release builds
```
