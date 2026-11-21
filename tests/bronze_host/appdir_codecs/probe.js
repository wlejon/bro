// The native codecs, from COMPILED code: bro.mesh's Draco round trip and
// bro.image's KTX2 transcode (host_codecs.cpp). The web's answer to both
// formats is a Worker spinning a WASM decoder; this stack's answer is
// google/draco in bromesh and the basis_universal transcoder in broimage,
// reached as one synchronous call — and this check is what proves the
// compiled realm actually has them, with typed-array payloads whose element
// access compiles natively.
//
// What only this check catches: a build that compiled the codecs out (the
// BROMESH_HAS_DRACO / BROIMAGE_HAS_KTX2 guards leaving empty objects), or a
// binding whose typed arrays arrive sized wrong for generated code's ABI
// element access.

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// --- Draco: encode, decode, and the payload is real -------------------------

const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0]);
const indices = new Uint32Array([0, 1, 2, 1, 3, 2]);
const bytes = bro.mesh.encodeDraco({ positions: positions, indices: indices });
say('dracoEncoded', bytes.length > 0);

const dec = bro.mesh.decodeDraco(bytes);
say('dracoVerts', dec.positions.length / 3);
say('dracoTris', dec.indices.length / 3);
say('dracoAttrs', dec.attributes.length);
say('dracoAttrType', dec.attributes[0].type);

// Element access on the decoded array is generated-code ABI access, not a
// property read; a mis-sized view fails HERE.
dec.positions[0] = 5;
say('dracoElemWrite', dec.positions[0]);

let refused = 'no';
try {
    bro.mesh.decodeDraco(new Uint8Array([1, 2, 3, 4]));
} catch (e) {
    refused = 'yes';
}
say('dracoRefusesJunk', refused);

// --- KTX2: a real file, transcoded to pixels ---------------------------------

fetch('2d_etc1s.ktx2')
    .then(function (res) { return res.arrayBuffer(); })
    .then(function (buf) {
        const tex = bro.image.transcodeKTX2(new Uint8Array(buf));
        say('ktxSize', tex.width + 'x' + tex.height);
        say('ktxFormat', tex.format);
        say('ktxMips', tex.mips.length);
        say('ktxBytes', tex.mips[0].data.length === tex.width * tex.height * 4);
        say('done', true);
    })
    .catch(function (e) {
        say('ktxError', e && e.message ? e.message : e);
    });
