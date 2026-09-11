# cmpd — C Music Player Daemon

## Build

    make            # build ./cmpd
    make test       # run the unit tests
    make asan       # address + undefined behaviour build
    make tsan       # data race build
    make deps       # check that the dev packages are installed

Dependencies: FFmpeg (libavformat, libavcodec, libavutil, libswresample),
PortAudio, ncursesw, panelw and TagLib.

Set `CMPD_LOG` to capture the stderr that is otherwise discarded while the
TUI owns the terminal.

## Todo

### Phase 1 — Core Engine
- [x] Project scaffold — Makefile, directory structure, build test
- [x] Audio decoder — FFmpeg-based, decode audio files to PCM
- [x] Audio output — PortAudio ring buffer, play/pause/stop/seek
- [x] Playlist engine — Play queue, next/prev/current track
- [x] Tag reader — TagLib wrapper for title/artist/album/metadata
- [ ] Config/Keybinds — INI-style config + keybinding system
      (keys are currently hardcoded in `ui_handle_key`)
- [x] Basic TUI — ncurses panels, now-playing bar, playlist view

### Phase 2 — Library & Album Art
- [ ] Album art — stb_image decode + half-block ANSI renderer
      (TagLib side is done: `tags_read` already extracts cover bytes for
      MP3, FLAC, Ogg Vorbis/Opus and MP4)
- [ ] Kitty/Sixel protocol support for pixel-perfect art
- [ ] Filesystem browser — browse dirs, add to playlist
- [ ] Library scanner + flat-file DB — persist song cache
- [ ] Search/filter — filter library by title/artist/album

### Phase 3 — Polish
- [x] Shuffle + repeat modes (none/one/all)
- [ ] M3U playlist save/load
- [x] Volume control (software, applied in the output callback)
- [ ] Visualizer — FFT spectrum, simple bars or scope
- [x] Progress bar + seeking with arrow keys
- [ ] Mouse support in ncurses

### Phase 4 — Extras
- [ ] Gapless playback + crossfade
      (the output device is now reused between same-format tracks, which is
      the groundwork; the ring still drains fully between tracks)
- [ ] MPRIS D-Bus integration for media keys

## Keys

    space    play / pause          j / k, arrows   move cursor
    enter    play selected         < / >, arrows   seek back / forward
    n / p    next / previous       + / -           volume
    s        shuffle               r               repeat mode
    g / G    first / last          page up/down    page
    ?        help                  q               quit
