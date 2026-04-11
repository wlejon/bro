// audio.js — Audio engine, SFX, music system, song data
var T = T || {};

T.Audio = {
    ctx: null,
    musicBus: -1,
    sfxBus: -1,
    melodyAlloc: null, bassAlloc: null, percAlloc: null,
    melodySeq: null, bassSeq: null, percSeq: null,
    currentSongIndex: -1,
    musicPlaying: false,

    SONGS: [
        // Song A: "Block March" — E minor, driving 4/4
        {
            name: "Block March", baseBPM: 140, loopBeats: 32,
            melodyWave: "square", bassWave: "triangle",
            melody: [
                [0,76,0.9,1],[1,71,0.8,0.5],[1.5,72,0.8,0.5],[2,74,0.9,1],
                [3,72,0.8,0.5],[3.5,71,0.8,0.5],[4,69,0.9,1],[5,69,0.7,0.5],
                [5.5,72,0.8,0.5],[6,76,0.9,1],[7,74,0.8,0.5],[7.5,72,0.8,0.5],
                [8,71,0.9,1.5],[9.5,72,0.7,0.5],[10,74,0.9,1],[11,76,0.9,1],
                [12,72,0.9,1],[13,69,0.9,1],[14,69,0.9,1.75],
                [16,74,0.9,1],[17,76,0.9,0.5],[17.5,74,0.8,0.5],[18,72,0.9,1],
                [19,71,0.7,0.5],[19.5,69,0.8,0.5],[20,71,0.9,1],[21,72,0.8,0.5],
                [21.5,74,0.8,0.5],[22,76,0.9,1.5],[23.5,74,0.7,0.5],[24,72,0.9,1],
                [25,69,0.8,0.5],[25.5,69,0.7,0.5],[26,71,0.9,1],[27,67,0.8,0.5],
                [27.5,69,0.8,0.5],[28,71,0.9,1],[29,72,0.8,0.5],[29.5,71,0.7,0.5],
                [30,76,0.9,1.75]
            ],
            bass: [
                [0,40,0.7,1],[1,40,0.5,1],[2,38,0.7,1],[3,38,0.5,1],
                [4,45,0.7,1],[5,45,0.5,1],[6,40,0.7,1],[7,40,0.5,1],
                [8,47,0.7,1],[9,47,0.5,1],[10,38,0.7,1],[11,38,0.5,1],
                [12,45,0.7,1],[13,45,0.5,1],[14,40,0.7,1.75],
                [16,38,0.7,1],[17,38,0.5,1],[18,36,0.7,1],[19,36,0.5,1],
                [20,47,0.7,1],[21,47,0.5,1],[22,40,0.7,1],[23,40,0.5,1],
                [24,36,0.7,1],[25,36,0.5,1],[26,47,0.7,1],[27,47,0.5,1],
                [28,47,0.7,1],[29,47,0.5,1],[30,40,0.7,1.75]
            ],
            perc: [
                [0,36,0.9,0.25],[2,36,0.9,0.25],[4,36,0.9,0.25],[6,36,0.9,0.25],
                [8,36,0.9,0.25],[10,36,0.9,0.25],[12,36,0.9,0.25],[14,36,0.9,0.25],
                [1,42,0.6,0.15],[3,42,0.6,0.15],[5,42,0.6,0.15],[7,42,0.6,0.15],
                [9,42,0.6,0.15],[11,42,0.6,0.15],[13,42,0.6,0.15],[15,42,0.6,0.15],
                [16,36,0.9,0.25],[18,36,0.9,0.25],[20,36,0.9,0.25],[22,36,0.9,0.25],
                [24,36,0.9,0.25],[26,36,0.9,0.25],[28,36,0.9,0.25],[30,36,0.9,0.25],
                [17,42,0.6,0.15],[19,42,0.6,0.15],[21,42,0.6,0.15],[23,42,0.6,0.15],
                [25,42,0.6,0.15],[27,42,0.6,0.15],[29,42,0.6,0.15],[31,42,0.6,0.15]
            ]
        },
        // Song B: "Crystal Stack" — C major, bouncy
        {
            name: "Crystal Stack", baseBPM: 128, loopBeats: 16,
            melodyWave: "square", bassWave: "triangle",
            melody: [
                [0,72,0.8,0.75],[0.75,74,0.7,0.25],[1,76,0.9,1],[2,72,0.8,0.5],
                [2.5,74,0.7,0.5],[3,76,0.8,0.5],[3.5,79,0.9,0.5],[4,81,0.9,1.5],
                [5.5,79,0.7,0.5],[6,76,0.8,1],[7,74,0.7,0.5],[7.5,72,0.7,0.5],
                [8,74,0.9,1],[9,72,0.7,0.5],[9.5,69,0.8,0.5],[10,72,0.9,1],
                [11,74,0.8,0.5],[11.5,76,0.8,0.5],[12,79,0.9,1],[13,76,0.8,1],
                [14,72,0.8,1.75]
            ],
            bass: [
                [0,48,0.7,1],[1,48,0.5,1],[2,48,0.7,1],[3,52,0.5,1],
                [4,45,0.7,1],[5,45,0.5,1],[6,48,0.7,1],[7,43,0.5,1],
                [8,50,0.7,1],[9,50,0.5,1],[10,48,0.7,1],[11,48,0.5,1],
                [12,43,0.7,1],[13,43,0.5,1],[14,48,0.7,1.75]
            ],
            perc: [
                [0,36,0.8,0.25],[2,36,0.6,0.25],[4,36,0.8,0.25],[6,36,0.6,0.25],
                [8,36,0.8,0.25],[10,36,0.6,0.25],[12,36,0.8,0.25],[14,36,0.6,0.25],
                [0.5,42,0.4,0.1],[1.5,42,0.4,0.1],[2.5,42,0.4,0.1],[3.5,42,0.4,0.1],
                [4.5,42,0.4,0.1],[5.5,42,0.4,0.1],[6.5,42,0.4,0.1],[7.5,42,0.4,0.1]
            ]
        },
        // Song C: "Neon Rush" — D minor, intense
        {
            name: "Neon Rush", baseBPM: 155, loopBeats: 16,
            melodyWave: "square", bassWave: "sawtooth",
            melody: [
                [0,74,0.9,0.5],[0.5,74,0.8,0.5],[1,77,0.9,0.5],[1.5,74,0.8,0.5],
                [2,72,0.9,0.5],[2.5,69,0.8,0.5],[3,72,0.9,1],[4,74,0.9,0.5],
                [4.5,77,0.9,0.5],[5,79,0.9,0.5],[5.5,77,0.8,0.5],[6,74,0.9,1],
                [7,72,0.8,0.5],[7.5,69,0.7,0.5],[8,70,0.9,0.5],[8.5,70,0.8,0.5],
                [9,72,0.9,0.5],[9.5,74,0.9,0.5],[10,77,0.9,1],[11,79,0.9,0.5],
                [11.5,77,0.8,0.5],[12,74,0.9,1],[13,69,0.8,0.5],[13.5,72,0.8,0.5],
                [14,74,0.9,1.75]
            ],
            bass: [
                [0,38,0.8,0.5],[0.5,38,0.6,0.5],[1,38,0.8,0.5],[1.5,38,0.6,0.5],
                [2,36,0.8,0.5],[2.5,36,0.6,0.5],[3,36,0.8,1],[4,38,0.8,0.5],
                [4.5,38,0.6,0.5],[5,41,0.8,0.5],[5.5,41,0.6,0.5],[6,38,0.8,1],
                [7,36,0.8,1],[8,34,0.8,0.5],[8.5,34,0.6,0.5],[9,36,0.8,0.5],
                [9.5,36,0.6,0.5],[10,41,0.8,1],[11,43,0.8,1],[12,38,0.8,1],
                [13,33,0.8,1],[14,38,0.8,1.75]
            ],
            perc: [
                [0,36,0.9,0.2],[1,36,0.7,0.2],[2,36,0.9,0.2],[3,36,0.7,0.2],
                [4,36,0.9,0.2],[5,36,0.7,0.2],[6,36,0.9,0.2],[7,36,0.7,0.2],
                [8,36,0.9,0.2],[9,36,0.7,0.2],[10,36,0.9,0.2],[11,36,0.7,0.2],
                [12,36,0.9,0.2],[13,36,0.7,0.2],[14,36,0.9,0.2],[15,36,0.7,0.2]
            ]
        }
    ],

    init: function() {
        try { this.ctx = new AudioContext(); } catch(e) { this.ctx = null; return; }
        try {
            this.musicBus = this.ctx.createBus();
            this.sfxBus = this.ctx.createBus();
            this.ctx.setBusGain(this.musicBus, T.Storage.settings.musicVol / 100);
            this.ctx.setBusGain(this.sfxBus, T.Storage.settings.sfxVol / 100);
            this.ctx.setBusReverbEnabled(this.musicBus, true);
            this.ctx.setBusReverbRoomSize(this.musicBus, 0.3);
            this.ctx.setBusReverbDamping(this.musicBus, 0.6);
            this.ctx.setBusReverbMix(this.musicBus, 0.15);
            this.ctx.setBusChorusEnabled(this.musicBus, true);
            this.ctx.setBusChorusRate(this.musicBus, 0.8);
            this.ctx.setBusChorusDepth(this.musicBus, 0.3);
            this.ctx.setBusChorusMix(this.musicBus, 0.1);
            this.ctx.setCompressorEnabled(true);
            this.ctx.setCompressorThreshold(-12);
            this.ctx.setCompressorRatio(3);
            this.ctx.setCompressorAttack(0.01);
            this.ctx.setCompressorRelease(0.1);
        } catch(e) {
            this.musicBus = -1;
            this.sfxBus = -1;
        }
    },

    playTone: function(freq, duration, type, vol) {
        if (!this.ctx) return;
        var v = (vol !== undefined ? vol : 1.0) * (T.Storage.settings.sfxVol / 100);
        if (v <= 0) return;
        try {
            var id = this.ctx.createVoice();
            this.ctx.setVoiceWaveform(id, type || "square");
            this.ctx.setVoiceFrequency(id, freq);
            this.ctx.setVoiceGain(id, v * 15.0);
            this.ctx.setVoiceAttack(id, 0.003);
            this.ctx.setVoiceDecay(id, duration * 0.8);
            this.ctx.setVoiceSustain(id, 0.0);
            this.ctx.setVoiceRelease(id, 0.02);
            if (this.sfxBus !== -1) this.ctx.setVoiceBus(id, this.sfxBus);
            var t = this.ctx.currentTime;
            this.ctx.startVoice(id, t);
            this.ctx.stopVoice(id, t + duration);
        } catch(e) {}
    },

    // SFX
    sfxMove: function() { this.playTone(200, 0.05, "square", 0.4); },
    sfxRotate: function() { this.playTone(300, 0.06, "square", 0.5); },
    sfxDrop: function() { this.playTone(120, 0.12, "triangle", 0.8); },
    sfxLock: function() { this.playTone(160, 0.08, "triangle", 0.5); },
    sfxHold: function() { this.playTone(250, 0.06, "sine", 0.4); },
    sfxClear1: function() { this.playTone(523, 0.15, "square", 0.6); },
    sfxClear2: function() { this.playTone(659, 0.15, "square", 0.7); },
    sfxClear3: function() { this.playTone(784, 0.18, "square", 0.8); },
    sfxMenuMove: function() { this.playTone(400, 0.03, "sine", 0.3); },
    sfxMenuSelect: function() { this.playTone(600, 0.08, "square", 0.4); },
    sfxCountdown: function() { this.playTone(440, 0.15, "sine", 0.6); },
    sfxGo: function() { this.playTone(880, 0.2, "square", 0.8); },

    sfxTetris: function() {
        T.Audio.playTone(523, 0.1, "square", 0.8);
        setTimeout(function() { T.Audio.playTone(659, 0.1, "square", 0.8); }, 80);
        setTimeout(function() { T.Audio.playTone(784, 0.12, "square", 0.9); }, 160);
        setTimeout(function() { T.Audio.playTone(1047, 0.2, "square", 1.0); }, 240);
    },
    sfxLevelUp: function() {
        T.Audio.playTone(440, 0.08, "sine", 0.6);
        setTimeout(function() { T.Audio.playTone(554, 0.08, "sine", 0.7); }, 80);
        setTimeout(function() { T.Audio.playTone(659, 0.12, "sine", 0.8); }, 160);
    },
    sfxGameOver: function() {
        T.Audio.playTone(300, 0.2, "sawtooth", 0.5);
        setTimeout(function() { T.Audio.playTone(250, 0.2, "sawtooth", 0.5); }, 200);
        setTimeout(function() { T.Audio.playTone(200, 0.4, "sawtooth", 0.5); }, 400);
    },
    sfxCombo: function(n) {
        var f = 400 + n * 80;
        if (f > 1200) f = 1200;
        this.playTone(f, 0.1, "square", 0.6);
    },

    // Music
    getSongForLevel: function(lvl) {
        if (lvl <= 7) return 0;
        if (lvl <= 14) return 1;
        return 2;
    },

    getMusicBPM: function(songIdx, lvl) {
        return this.SONGS[songIdx].baseBPM + (lvl - 1) * 4;
    },

    buildSequences: function(songIdx) {
        if (!this.ctx) return;
        var song = this.SONGS[songIdx];
        this.stopMusic();

        var ac = this.ctx;
        var bus = this.musicBus;

        this.melodyAlloc = ac.createVoiceAllocator(8);
        this.melodyAlloc.setStealPolicy("oldest");
        this.melodyAlloc.setVoiceSetup(function(voiceId, note, velocity) {
            ac.setVoiceNote(voiceId, note, velocity);
            ac.setVoiceWaveform(voiceId, song.melodyWave);
            ac.setVoiceFrequency(voiceId, 440 * Math.pow(2, (note - 69) / 12));
            ac.setVoiceGain(voiceId, 15.0);
            ac.setVoicePan(voiceId, 0);
            ac.setVoiceAttack(voiceId, 0.008);
            ac.setVoiceDecay(voiceId, 0.08);
            ac.setVoiceSustain(voiceId, 0.7);
            ac.setVoiceRelease(voiceId, 0.06);
            if (bus !== -1) ac.setVoiceBus(voiceId, bus);
        });

        this.bassAlloc = ac.createVoiceAllocator(4);
        this.bassAlloc.setStealPolicy("oldest");
        this.bassAlloc.setVoiceSetup(function(voiceId, note, velocity) {
            ac.setVoiceNote(voiceId, note, velocity);
            ac.setVoiceWaveform(voiceId, song.bassWave);
            ac.setVoiceFrequency(voiceId, 440 * Math.pow(2, (note - 69) / 12));
            ac.setVoiceGain(voiceId, 15.0);
            ac.setVoicePan(voiceId, 0);
            ac.setVoiceAttack(voiceId, 0.01);
            ac.setVoiceDecay(voiceId, 0.1);
            ac.setVoiceSustain(voiceId, 0.8);
            ac.setVoiceRelease(voiceId, 0.08);
            if (bus !== -1) ac.setVoiceBus(voiceId, bus);
        });

        this.percAlloc = ac.createVoiceAllocator(4);
        this.percAlloc.setStealPolicy("oldest");
        this.percAlloc.setVoiceSetup(function(voiceId, note, velocity) {
            ac.setVoiceNote(voiceId, note, velocity);
            if (note >= 40) {
                ac.setVoiceWaveform(voiceId, "whitenoise");
                ac.setVoiceGain(voiceId, 10.0);
                ac.setVoiceAttack(voiceId, 0.001);
                ac.setVoiceDecay(voiceId, 0.04);
                ac.setVoiceSustain(voiceId, 0.0);
                ac.setVoiceRelease(voiceId, 0.02);
                ac.setVoiceFilterEnabled(voiceId, true);
                ac.setVoiceFilterType(voiceId, "highpass");
                ac.setVoiceFilterFrequency(voiceId, 8000);
            } else {
                ac.setVoiceWaveform(voiceId, "triangle");
                ac.setVoiceFrequency(voiceId, 55);
                ac.setVoiceGain(voiceId, 15.0);
                ac.setVoiceAttack(voiceId, 0.002);
                ac.setVoiceDecay(voiceId, 0.12);
                ac.setVoiceSustain(voiceId, 0.0);
                ac.setVoiceRelease(voiceId, 0.05);
            }
            if (bus !== -1) ac.setVoiceBus(voiceId, bus);
        });

        function buildSeq(alloc, notes) {
            var seq = ac.createSequence(alloc);
            for (var i = 0; i < notes.length; i++) {
                seq.addNote(notes[i][0], notes[i][1], notes[i][2], notes[i][3]);
            }
            seq.setLoopEnabled(true);
            seq.setLoopRange(0, song.loopBeats);
            return seq;
        }

        this.melodySeq = buildSeq(this.melodyAlloc, song.melody);
        this.bassSeq = buildSeq(this.bassAlloc, song.bass);
        this.percSeq = buildSeq(this.percAlloc, song.perc);
        this.currentSongIndex = songIdx;
    },

    startMusic: function(level) {
        if (!this.ctx || !this.melodySeq) return;
        var bpm = this.getMusicBPM(this.currentSongIndex, level);
        this.melodySeq.setBPM(bpm);
        this.bassSeq.setBPM(bpm);
        this.percSeq.setBPM(bpm);
        var t = this.ctx.currentTime;
        this.melodySeq.play(t);
        this.bassSeq.play(t);
        this.percSeq.play(t);
        this.musicPlaying = true;
    },

    stopMusic: function() {
        this.musicPlaying = false;
        var seqs = [this.melodySeq, this.bassSeq, this.percSeq];
        var allocs = [this.melodyAlloc, this.bassAlloc, this.percAlloc];
        for (var i = 0; i < seqs.length; i++) {
            if (seqs[i]) try { seqs[i].stop(); } catch(e) {}
            if (allocs[i]) try { allocs[i].allNotesOff(); } catch(e) {}
        }
        this.melodySeq = this.bassSeq = this.percSeq = null;
        this.melodyAlloc = this.bassAlloc = this.percAlloc = null;
        this.currentSongIndex = -1;
    },

    pauseMusic: function() {
        if (!this.musicPlaying) return;
        var t = this.ctx.currentTime;
        var seqs = [this.melodySeq, this.bassSeq, this.percSeq];
        var allocs = [this.melodyAlloc, this.bassAlloc, this.percAlloc];
        for (var i = 0; i < seqs.length; i++) {
            if (seqs[i]) try { seqs[i].pause(t); } catch(e) {}
            if (allocs[i]) try { allocs[i].allNotesOff(); } catch(e) {}
        }
    },

    resumeMusic: function() {
        if (!this.musicPlaying) return;
        var t = this.ctx.currentTime;
        if (this.melodySeq) try { this.melodySeq.resume(t); } catch(e) {}
        if (this.bassSeq) try { this.bassSeq.resume(t); } catch(e) {}
        if (this.percSeq) try { this.percSeq.resume(t); } catch(e) {}
    },

    updateMusicBPM: function(level) {
        if (!this.musicPlaying || !this.melodySeq) return;
        var bpm = this.getMusicBPM(this.currentSongIndex, level);
        this.melodySeq.setBPM(bpm);
        this.bassSeq.setBPM(bpm);
        this.percSeq.setBPM(bpm);
    },

    updateMusicVolume: function() {
        if (!this.ctx || this.musicBus === -1) return;
        try { this.ctx.setBusGain(this.musicBus, T.Storage.settings.musicVol / 100); } catch(e) {}
    },

    updateSfxVolume: function() {
        if (!this.ctx || this.sfxBus === -1) return;
        try { this.ctx.setBusGain(this.sfxBus, T.Storage.settings.sfxVol / 100); } catch(e) {}
    },

    checkSongChange: function(level) {
        var want = this.getSongForLevel(level);
        if (want !== this.currentSongIndex) {
            this.buildSequences(want);
            this.startMusic(level);
        }
    },

    updateSequences: function() {
        if (!this.musicPlaying || !this.ctx) return;
        var t = this.ctx.currentTime;
        if (this.melodySeq) try { this.melodySeq.update(t); } catch(e) {}
        if (this.bassSeq) try { this.bassSeq.update(t); } catch(e) {}
        if (this.percSeq) try { this.percSeq.update(t); } catch(e) {}
    }
};
