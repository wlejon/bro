// Test bro.steam availability, structure, properties, methods, and stub behavior.
// Exercises src/js/steam_bindings.cpp

assert(typeof bro === "object", "bro namespace exists");
assert(typeof bro.steam === "object", "bro.steam namespace exists");

// 1. Core probe properties
assert(typeof bro.steam.available === "boolean", "available is boolean");
assert(typeof bro.steam.reason === "string", "reason is string");
assert(typeof bro.steam.steamId === "string", "steamId is string");
assert(typeof bro.steam.personaName === "string", "personaName is string");
assert(typeof bro.steam.appId === "number", "appId is number");
assert(typeof bro.steam.isVoiceRecording === "boolean", "isVoiceRecording is boolean");
assert(typeof bro.steam.voiceSampleRate === "number", "voiceSampleRate is number");

// In headless environment without steam running, steam is unavailable with clear reason
if (!bro.steam.available) {
    assert(bro.steam.steamId === "0", "steamId is 0 when unavailable");
    assert(bro.steam.appId === 0, "appId is 0 when unavailable");
    assert(bro.steam.personaName === "", "personaName is empty when unavailable");
}

// 2. Callback properties (getter / setter)
const callbacks = [
    "onpulse", "onfriends", "onoverlay", "onjoinrequest",
    "onlobbyentered", "onlobbyupdated", "onlobbyleft",
    "onlobbyinvite", "onlobbyjoinrequest", "onvoicecaptured"
];
for (const cb of callbacks) {
    assert(cb in bro.steam, "callback property exists: " + cb);
    const dummy = () => {};
    bro.steam[cb] = dummy;
    assert(bro.steam[cb] === dummy, cb + " getter returns assigned function");
    bro.steam[cb] = undefined;
}

// 3. Friends and Rich Presence API structure
assert(typeof bro.steam.getFriends === "function", "getFriends is function");
assert(typeof bro.steam.getAvatar === "function", "getAvatar is function");
assert(typeof bro.steam.setRichPresence === "function", "setRichPresence is function");
assert(typeof bro.steam.clearRichPresence === "function", "clearRichPresence is function");
assert(typeof bro.steam.activateOverlay === "function", "activateOverlay is function");
assert(typeof bro.steam.activateOverlayToUser === "function", "activateOverlayToUser is function");
assert(typeof bro.steam.activateInviteDialog === "function", "activateInviteDialog is function");

const friends = bro.steam.getFriends();
assert(Array.isArray(friends), "getFriends returns array");

assert(typeof bro.steam.setRichPresence("status", "Testing") === "boolean", "setRichPresence returns boolean");
assert(bro.steam.clearRichPresence() === undefined, "clearRichPresence returns undefined");

// 4. Lobby API structure
assert(typeof bro.steam.createLobby === "function", "createLobby is function");
assert(typeof bro.steam.joinLobby === "function", "joinLobby is function");
assert(typeof bro.steam.leaveLobby === "function", "leaveLobby is function");
assert(typeof bro.steam.setLobbyData === "function", "setLobbyData is function");
assert(typeof bro.steam.setLobbyMemberData === "function", "setLobbyMemberData is function");
assert(typeof bro.steam.setLobbyJoinable === "function", "setLobbyJoinable is function");
assert(typeof bro.steam.setLobbyType === "function", "setLobbyType is function");
assert(typeof bro.steam.setLobbyMemberLimit === "function", "setLobbyMemberLimit is function");
assert(typeof bro.steam.getLobbyMembers === "function", "getLobbyMembers is function");
assert(typeof bro.steam.getLobbyOwner === "function", "getLobbyOwner is function");
assert(typeof bro.steam.getLobbyData === "function", "getLobbyData is function");
assert(typeof bro.steam.requestLobbyList === "function", "requestLobbyList is function");
assert(typeof bro.steam.inviteUserToLobby === "function", "inviteUserToLobby is function");

assert(Array.isArray(bro.steam.getLobbyMembers("0")), "getLobbyMembers returns array");
assert(typeof bro.steam.getLobbyOwner("0") === "string", "getLobbyOwner returns string");
assert(typeof bro.steam.getLobbyData("0", "name") === "string", "getLobbyData returns string");

// 5. Voice API structure
assert(typeof bro.steam.startVoiceRecording === "function", "startVoiceRecording is function");
assert(typeof bro.steam.stopVoiceRecording === "function", "stopVoiceRecording is function");
assert(typeof bro.steam.decodeVoice === "function", "decodeVoice is function");

// 6. Asynchronous stub methods
const avatar = await bro.steam.getAvatar("1234567890");
assert(avatar === null, "getAvatar resolves to null when unavailable or no avatar");

const lobbyList = await bro.steam.requestLobbyList({ distance: "worldwide", maxResults: 10 });
assert(Array.isArray(lobbyList), "requestLobbyList resolves to array");

const createdLobby = await bro.steam.createLobby("public", 4);
assert(createdLobby === null, "createLobby resolves to null when unavailable");

const joinResult = await bro.steam.joinLobby("123456");
assert(typeof joinResult === "object", "joinLobby resolves to object");
assert(joinResult.success === false, "joinResult.success is false when unavailable");

const voiceDecoded = await bro.steam.decodeVoice(new Uint8Array(0));
assert(typeof voiceDecoded === "object", "decodeVoice resolves to object");
assert(voiceDecoded.pcm instanceof Float32Array, "decoded voice pcm is Float32Array");
assert(voiceDecoded.pcm.length === 0, "decoded voice pcm is empty for empty input");

console.log("test_steam: passed");
