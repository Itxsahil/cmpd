# cmpd — C Music Player Daemon

## Todo

### Phase 1 — Core Engine
- [ ] Project scaffold — Makefile, directory structure, build test
- [ ] Audio decoder — FFmpeg-based, decode audio files to PCM
- [ ] Audio output — PortAudio ring buffer, play/pause/stop/seek
- [ ] Playlist engine — Play queue, next/prev/current track
- [ ] Tag reader — TagLib wrapper for title/artist/album/metadata
- [ ] Config/Keybinds — INI-style config + keybinding system
- [ ] Basic TUI — ncurses panels, now-playing bar, playlist view

### Phase 2 — Library & Album Art
- [ ] Album art — stb_image decode + half-block ANSI renderer
- [ ] Kitty/Sixel protocol support for pixel-perfect art
- [ ] Filesystem browser — browse dirs, add to playlist
- [ ] Library scanner + flat-file DB — persist song cache
- [ ] Search/filter — filter library by title/artist/album

### Phase 3 — Polish
- [ ] Shuffle + repeat modes (none/one/all)
- [ ] M3U playlist save/load
- [ ] Volume control (software via swr or PortAudio)
- [ ] Visualizer — FFT spectrum, simple bars or scope
- [ ] Progress bar + seeking with arrow keys
- [ ] Mouse support in ncurses

### Phase 4 — Extras
- [ ] Gapless playback + crossfade
- [ ] MPRIS D-Bus integration for media keys
