/**
 * @file docs/mpris-api.js
 * @summary Documentation and examples for the `bro.mpris` JavaScript API.
 */

/**
 * `bro.mpris` provides both sides of the FreeDesktop MPRIS2
 * (`org.mpris.MediaPlayer2`) D-Bus specification:
 * - **Controller / Client**: discover active media players on the session bus,
 *   inspect playback status, track metadata, position, volume, loop/shuffle
 *   state, and capabilities, send transport commands (`play`, `pause`,
 *   `playPause`, `stop`, `next`, `previous`, `seek`, `setPosition`,
 *   `setVolume`, `setLoopStatus`, `setShuffle`, `setShuffleStatus`, `setRate`,
 *   `openUri`, `raise`, `quit`), and subscribe to live player events.
 * - **Player Service (`createService` / `publish` / `createPlayerService`)**:
 *   export an MPRIS2 media player bus name (`org.mpris.MediaPlayer2.<name>`)
 *   from a bro app so desktop shells, lock screens, media keys, and `playerctl`
 *   can inspect and control playback.
 *
 * Mounted automatically in Bronze when `BRO_WITH_MPRIS` is enabled.
 */

// ============================================================================
// 1. Listing & Inspecting Media Players (`getPlayers`, `getActivePlayer`, `findPlayer`)
// ============================================================================

const players = bro.mpris.getPlayers();
for (const p of players) {
    // {
    //   id, name, busName, identity, desktopEntry,
    //   playbackStatus, loopStatus, shuffle, shuffleStatus, rate, volume, position,
    //   canControl, canPlay, canPause, canSeek, canGoNext, canGoPrevious, canRaise, canQuit,
    //   capabilities: { canControl, canPlay, canPause, canSeek, canGoNext, canGoPrevious, canRaise, canQuit, canSetFullscreen },
    //   metadata: {
    //     trackId, title, artist, artists, album, albumArtist, albumArtists,
    //     artUrl, albumArtUrl, url, length, lengthUs, duration,
    //     trackNumber, discNumber, genre, genres, userRating
    //   }
    // }
    console.log(`${p.identity} (${p.id}): ${p.playbackStatus}`);
    console.log(`  Track: ${p.metadata.title} — ${p.metadata.artist} (${p.metadata.album})`);
    console.log(`  Position: ${p.position} us / ${p.metadata.length} us`);
}

// Active player (prioritizes Playing, then Paused, then first available):
const active = bro.mpris.getActivePlayer();
if (active) {
    console.log(`Active player: ${active.identity} [${active.playbackStatus}]`);
}

// Look up a specific player by ID or bus name:
const vlc = bro.mpris.findPlayer('vlc');
const meta = bro.mpris.getMetadata('vlc'); // or bro.mpris.getMetadata() for active player

// ============================================================================
// 2. Controlling Playback (Top-Level or Per-Player Handle)
// ============================================================================

// Top-level commands target the active player when playerId is omitted,
// or a specific player when playerId is passed first:
bro.mpris.playPause();                  // toggle active player
bro.mpris.play('vlc');                  // play specific player
bro.mpris.pause('vlc');
bro.mpris.stop('vlc');
bro.mpris.next();                       // next track on active player
bro.mpris.previous();                   // previous track on active player
bro.mpris.seek(5_000_000);              // seek +5s (in microseconds) on active player
bro.mpris.seek('vlc', -10_000_000);     // seek -10s on specific player
bro.mpris.setPosition(30_000_000);      // jump to 30s (in microseconds)
bro.mpris.setVolume(0.8);               // set volume (0.0 .. 1.0+)
bro.mpris.setLoopStatus('Playlist');    // 'None' | 'Track' | 'Playlist'
bro.mpris.setShuffle(true);             // boolean or 'On' | 'Off' (alias: setShuffleStatus)
bro.mpris.setRate(1.25);                // playback rate multiplier

// Player objects returned by getPlayers() / getActivePlayer() / findPlayer()
// also carry bound convenience methods:
if (active) {
    active.playPause();
    active.seek(5_000_000);
    active.setVolume(0.75);
}

// ============================================================================
// 3. Subscribing to Controller Events (`on`, `off`)
// ============================================================================
//
// Events:
// - 'playerAdded'         ({ type, id, identity, playbackStatus, player, ... })
// - 'playerRemoved'       ({ type, id, playerId })
// - 'activePlayerChanged' ({ type, id, player, ... })
// - 'playbackStatus'      ({ type, id, playerId, playbackStatus, status })
// - 'metadata'            ({ type, id, playerId, title, artist, album, albumArtUrl, duration, trackId, metadata })
// - 'seeked'              ({ type, id, playerId, position, positionUsec })
// - 'volume'              ({ type, id, playerId, volume })
// - 'loopStatus'          ({ type, id, playerId, loopStatus, status })
// - 'shuffle'             ({ type, id, playerId, shuffle, shuffleStatus })
// - 'rate'                ({ type, id, playerId, rate })
// - 'capabilities'        ({ type, id, playerId, capabilities })
// - 'change' / '*'        (fires on any event above)

const onStatus = (ev) => {
    console.log(`Player ${ev.id} status -> ${ev.playbackStatus}`);
};
bro.mpris.on('playbackStatus', onStatus);
bro.mpris.on('metadata', (ev) => {
    console.log(`Now playing on ${ev.id}: ${ev.title} by ${ev.artist}`);
});

// Remove listener:
// bro.mpris.off('playbackStatus', onStatus);

// ============================================================================
// 4. Publishing an MPRIS2 Player Service (`createService` / `publish`)
// ============================================================================

const service = bro.mpris.createService({
    name: 'helm_music',                 // claims org.mpris.MediaPlayer2.helm_music
    identity: 'Helm Music',
    desktopEntry: 'helm-music',
    canControl: true,
    canPlay: true,
    canPause: true,
    canGoNext: true,
    canGoPrevious: true,
    canSeek: true,
    canRaise: true,
    canQuit: false,
    volume: 1.0,
    playbackStatus: 'Stopped'
});

// Listen for remote control commands from desktop shells / media keys:
service.on('play', () => {
    service.setPlaybackStatus('Playing');
});
service.on('pause', () => {
    service.setPlaybackStatus('Paused');
});
service.on('playPause', () => {
    const next = service.getPlaybackStatus() === 'Playing' ? 'Paused' : 'Playing';
    service.setPlaybackStatus(next);
});
service.on('next', () => {
    // advance playlist
});
service.on('previous', () => {
    // previous track
});
service.on('seek', (ev, offsetUsec) => {
    const nextPos = Math.max(0, service.getPosition() + offsetUsec);
    service.setPosition(nextPos);
    service.emitSeeked(nextPos); // alias: service.seeked(nextPos)
});
service.on('setPosition', (ev, posUsec, trackId) => {
    service.setPosition(posUsec);
    service.emitSeeked(posUsec);
});
service.on('volume', (ev, vol) => {
    service.setVolume(vol);
});
service.on('loopStatus', (ev, loopStatus) => {
    service.setLoopStatus(loopStatus);
});
service.on('shuffle', (ev, shuffle) => {
    service.setShuffle(shuffle);
});

// Publish track metadata and state updates:
service.setMetadata({
    trackId: '/org/mpris/MediaPlayer2/Track/1',
    title: 'Solaris',
    artist: 'Archive',
    album: 'Patterns',
    artUrl: 'file:///home/j/Music/cover.jpg',
    length: 215_000_000 // microseconds
});
service.setPlaybackStatus('Playing');
service.setPosition(12_000_000);

// Unpublish and release D-Bus name when done (aliases: stop(), destroy()):
// service.close();
