// Online play bridge for the browser build: the host talks to up to five guests, each over its own peer-to-peer link, through the free PeerJS broker
// (no server of ours). The game (C++) polls window.fhNet; this file never touches game rules. Messages are opaque byte arrays.
(function () {
    'use strict';

    const PEER_SOURCES = [
        'https://unpkg.com/peerjs@1.5.4/dist/peerjs.min.js',
        'https://cdn.jsdelivr.net/npm/peerjs@1.5.4/dist/peerjs.min.js'
    ];
    const ID_PREFIX = 'fh2sync-';
    const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    const MAX_GUESTS = 5;
    const CHUNK_SIZE = 16000;
    const PING_EVERY_MS = 2000;
    const SILENCE_LIMIT_MS = 30000;
    const REPLACE_AFTER_SILENT_MS = 4000;
    const CLOSE_DELAY_MS = 800;
    const REJOIN_EVERY_MS = 3000;
    const JOIN_TIMEOUT_MS = 20000;
    const INTRO_TIMEOUT_MS = 10000;

    // Frame kinds: 0 ping, 1 data chunk, 2 guest introduction (its client id), 3 refusal sent by the host
    const FRAME_PING = 0;
    const FRAME_DATA = 1;
    const FRAME_HELLO = 2;
    const FRAME_REFUSED = 3;

    function makeClientId() {
        try {
            const saved = sessionStorage.getItem('fhClientId');
            if (saved) return saved;
        } catch (e) { /* storage may be blocked */ }
        let id = '';
        const bytes = new Uint8Array(10);
        crypto.getRandomValues(bytes);
        for (const b of bytes) id += CODE_ALPHABET[b % CODE_ALPHABET.length];
        try { sessionStorage.setItem('fhClientId', id); } catch (e) { /* ignore */ }
        return id;
    }

    const net = {
        // Host: idle | starting | waiting (room open) | error.   Guest: idle | starting | connecting | connected | lost | error
        state: 'idle',
        role: '',
        code: '',
        error: '',
        connects: 0, // guest: how often the link came up; the game re-sends its last message when this changes
        clientId: makeClientId(),
        _peer: null,
        _slots: [], // host: one entry per guest, in the order they joined
        _guestConn: null, // guest: the link to the host
        _guestLastRx: 0,
        _guestPartial: new Map(),
        _rx: [],
        _nextMsgId: 1,
        _timer: 0,
        _rejoinTimer: 0,
        _joinTimer: 0,
        _everConnected: false,
        _locked: false,
        _gen: 0
    };

    function randomCode() {
        let code = '';
        const bytes = new Uint8Array(5);
        crypto.getRandomValues(bytes);
        for (const b of bytes) code += CODE_ALPHABET[b % CODE_ALPHABET.length];
        return code;
    }

    function loadPeerLibrary(done, index) {
        if (typeof Peer !== 'undefined') return done(true);
        index = index || 0;
        if (index >= PEER_SOURCES.length) return done(false);
        const script = document.createElement('script');
        script.src = PEER_SOURCES[index];
        script.onload = () => done(typeof Peer !== 'undefined');
        script.onerror = () => loadPeerLibrary(done, index + 1);
        document.head.append(script);
    }

    function fail(message) {
        net.state = 'error';
        net.error = message;
    }

    function stopTimers() {
        clearInterval(net._timer);
        clearTimeout(net._rejoinTimer);
        clearTimeout(net._joinTimer);
        net._timer = 0;
        net._rejoinTimer = 0;
        net._joinTimer = 0;
    }

    function toBytes(data) {
        if (data instanceof Uint8Array) return data;
        if (data instanceof ArrayBuffer) return new Uint8Array(data);
        if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        return null;
    }

    // Puts chunks of one link together. Whole messages go to the receive queue tagged with the slot they came from.
    function onDataFrame(frame, partial, slot) {
        if (frame.length < 9) return;
        const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
        const id = view.getUint32(1, true);
        const index = view.getUint16(5, true);
        const count = view.getUint16(7, true);
        const payload = frame.subarray(9);
        if (count === 0 || index >= count) return;
        if (count === 1) {
            net._rx.push({ slot, bytes: payload.slice() });
            return;
        }
        let entry = partial.get(id);
        if (entry && entry.count !== count) return;
        if (!entry) {
            entry = { count, parts: new Array(count), got: 0 };
            partial.set(id, entry);
        }
        if (!entry.parts[index]) {
            entry.parts[index] = payload.slice();
            entry.got++;
        }
        if (entry.got === entry.count) {
            partial.delete(id);
            let total = 0;
            for (const part of entry.parts) total += part.length;
            const whole = new Uint8Array(total);
            let offset = 0;
            for (const part of entry.parts) {
                whole.set(part, offset);
                offset += part.length;
            }
            net._rx.push({ slot, bytes: whole });
        }
    }

    function sendChunks(conn, data) {
        const count = Math.max(1, Math.ceil(data.length / CHUNK_SIZE));
        if (count > 65535) return false;
        const id = net._nextMsgId++ >>> 0;
        for (let i = 0; i < count; i++) {
            const part = data.subarray(i * CHUNK_SIZE, Math.min(data.length, (i + 1) * CHUNK_SIZE));
            const frame = new Uint8Array(9 + part.length);
            const view = new DataView(frame.buffer);
            frame[0] = FRAME_DATA;
            view.setUint32(1, id, true);
            view.setUint16(5, i, true);
            view.setUint16(7, count, true);
            frame.set(part, 9);
            conn.send(frame);
        }
        return true;
    }

    function startHeartbeat() {
        if (net._timer) return;
        let lastTick = Date.now();
        net._timer = setInterval(() => {
            const now = Date.now();
            const wasFrozen = now - lastTick > PING_EVERY_MS * 3;
            lastTick = now;
            if (net.role === 'host') {
                net._slots.forEach((slot, index) => {
                    if (slot.state !== 'connected') return;
                    if (wasFrozen) slot.lastRx = now; // this page was busy: the silence says nothing about the guest
                    if (now - slot.lastRx > SILENCE_LIMIT_MS) { slotLost(index); return; }
                    try { slot.conn.send(new Uint8Array([FRAME_PING])); } catch (e) { slotLost(index); }
                });
            }
            else if (net.role === 'guest' && net.state === 'connected') {
                if (wasFrozen) net._guestLastRx = now;
                if (now - net._guestLastRx > SILENCE_LIMIT_MS) { guestLinkLost(); return; }
                try { net._guestConn.send(new Uint8Array([FRAME_PING])); } catch (e) { guestLinkLost(); }
            }
        }, PING_EVERY_MS);
    }

    // ---------- host ----------

    function slotLost(index) {
        const slot = net._slots[index];
        if (!slot || slot.state === 'closed') return;
        const conn = slot.conn;
        slot.conn = null;
        slot.partial.clear();
        slot.state = 'lost';
        try { if (conn) conn.close(); } catch (e) { /* already closed */ }
    }

    function hostAccept(conn) {
        const gen = net._gen;
        let slotIndex = -1;
        let introduced = false;

        const refuse = reason => {
            try { conn.send(new Uint8Array([FRAME_REFUSED, reason || 1])); } catch (e) { /* ignore */ }
            setTimeout(() => { try { conn.close(); } catch (e) { /* ignore */ } }, CLOSE_DELAY_MS);
        };

        const introduce = frame => {
            const clientId = new TextDecoder().decode(frame.subarray(1)).slice(0, 32);
            if (!clientId) return refuse();
            let index = net._slots.findIndex(slot => slot.clientId === clientId);
            if (index >= 0) {
                const slot = net._slots[index];
                if (slot.state === 'closed') return refuse();
                // The same guest coming back: take over, unless the old link is alive and in use.
                if (slot.state === 'connected' && slot.conn !== conn && Date.now() - slot.lastRx < REPLACE_AFTER_SILENT_MS) return refuse(2);
                if (slot.conn && slot.conn !== conn) { try { slot.conn.close(); } catch (e) { /* ignore */ } }
            }
            else {
                if (net._locked) return refuse(1);
                // Before the game starts a slot whose guest is gone can be given to a new guest.
                index = net._slots.length >= MAX_GUESTS ? net._slots.findIndex(entry => entry.state === 'lost') : -1;
                if (index < 0 && net._slots.length >= MAX_GUESTS) return refuse(1);
                const fresh = { clientId, conn: null, state: 'lost', connects: 0, lastRx: 0, partial: new Map() };
                if (index >= 0) net._slots[index] = fresh;
                else { index = net._slots.length; net._slots.push(fresh); }
            }
            const slot = net._slots[index];
            slot.conn = conn;
            slot.state = 'connected';
            slot.lastRx = Date.now();
            slot.connects++;
            slot.partial.clear();
            slotIndex = index;
            introduced = true;
            startHeartbeat();
        };

        const opened = () => {
            setTimeout(() => { if (gen === net._gen && !introduced) { try { conn.close(); } catch (e) { /* ignore */ } } }, INTRO_TIMEOUT_MS);
        };
        conn.on('open', opened);
        conn.on('data', raw => {
            if (gen !== net._gen) return;
            const frame = toBytes(raw);
            if (!frame || frame.length === 0) return;
            if (!introduced) {
                if (frame[0] === FRAME_HELLO) introduce(frame);
                return;
            }
            const slot = net._slots[slotIndex];
            if (!slot || slot.conn !== conn) return;
            slot.lastRx = Date.now();
            if (frame[0] === FRAME_DATA) onDataFrame(frame, slot.partial, slotIndex);
        });
        const closed = () => { if (gen === net._gen && introduced && net._slots[slotIndex] && net._slots[slotIndex].conn === conn) slotLost(slotIndex); };
        conn.on('close', closed);
        conn.on('error', closed);
        if (conn.open) opened();
    }

    function openHostPeer(attempt) {
        const gen = net._gen;
        net.code = randomCode();
        const peer = new Peer(ID_PREFIX + net.code);
        net._peer = peer;
        peer.on('open', () => { if (gen === net._gen && net.state === 'starting') net.state = 'waiting'; });
        peer.on('connection', conn => { if (gen === net._gen) hostAccept(conn); });
        peer.on('disconnected', () => { if (gen === net._gen && !peer.destroyed) { try { peer.reconnect(); } catch (e) { /* retry on next event */ } } });
        peer.on('error', err => {
            if (gen !== net._gen) return;
            if (err.type === 'unavailable-id' && attempt < 5) {
                try { peer.destroy(); } catch (e) { /* ignore */ }
                openHostPeer(attempt + 1);
                return;
            }
            if (err.type === 'network' || err.type === 'server-error' || err.type === 'socket-error' || err.type === 'socket-closed') {
                if (net.state === 'starting') fail('Could not reach the free matchmaking server (' + err.type + ').');
                return;
            }
            if (net.state === 'starting') fail('Online error: ' + err.type);
        });
    }

    // ---------- guest ----------

    function guestLinkLost() {
        const conn = net._guestConn;
        net._guestConn = null;
        net._guestPartial.clear();
        try { if (conn) conn.close(); } catch (e) { /* already closed */ }
        if (net.state === 'idle' || net.state === 'error') return;
        net.state = 'lost';
        scheduleRejoin();
    }

    function guestAttach(conn) {
        const gen = net._gen;
        const opened = () => {
            if (gen !== net._gen) return;
            clearTimeout(net._joinTimer);
            if (net._guestConn && net._guestConn !== conn) { try { net._guestConn.close(); } catch (e) { /* ignore */ } }
            net._guestConn = conn;
            net._guestLastRx = Date.now();
            net._everConnected = true;
            net.state = 'connected';
            net.connects++;
            startHeartbeat();
            try {
                const id = new TextEncoder().encode(net.clientId);
                const frame = new Uint8Array(1 + id.length);
                frame[0] = FRAME_HELLO;
                frame.set(id, 1);
                conn.send(frame);
            } catch (e) { guestLinkLost(); }
        };
        conn.on('open', opened);
        conn.on('data', raw => {
            if (conn !== net._guestConn) return;
            const frame = toBytes(raw);
            if (!frame || frame.length === 0) return;
            net._guestLastRx = Date.now();
            if (frame[0] === FRAME_DATA) onDataFrame(frame, net._guestPartial, 0);
            else if (frame[0] === FRAME_REFUSED) {
                stopTimers();
                net._guestConn = null;
                fail(frame[1] === 2
                    ? 'You are already connected to this room from another window of this browser.'
                    : 'The host did not accept you: the room is full, the game has already started, or you were taken out of the game.');
            }
        });
        conn.on('close', () => { if (gen === net._gen && conn === net._guestConn) guestLinkLost(); });
        conn.on('error', () => { if (gen === net._gen && conn === net._guestConn) guestLinkLost(); });
        if (conn.open) opened();
    }

    function connectToHost() {
        const gen = net._gen;
        if (!net._peer || net._peer.destroyed) return;
        if (net._peer.disconnected) { try { net._peer.reconnect(); } catch (e) { /* retry later */ } }
        const conn = net._peer.connect(ID_PREFIX + net.code, { reliable: true, serialization: 'raw' });
        guestAttach(conn);
        clearTimeout(net._joinTimer);
        net._joinTimer = setTimeout(() => {
            if (gen !== net._gen || net.state === 'connected') return;
            try { conn.close(); } catch (e) { /* ignore */ }
            if (net._everConnected) scheduleRejoin();
            else fail('Nobody answered for this room code.');
        }, JOIN_TIMEOUT_MS);
    }

    function scheduleRejoin() {
        clearTimeout(net._rejoinTimer);
        const gen = net._gen;
        net._rejoinTimer = setTimeout(() => {
            if (gen !== net._gen || net.state === 'connected' || net.state === 'idle' || net.state === 'error') return;
            connectToHost();
        }, REJOIN_EVERY_MS);
    }

    // ---------- public ----------

    function reset(role) {
        net._gen++;
        stopTimers();
        for (const slot of net._slots) { try { if (slot.conn) slot.conn.close(); } catch (e) { /* ignore */ } }
        try { if (net._guestConn) net._guestConn.close(); } catch (e) { /* ignore */ }
        try { if (net._peer) net._peer.destroy(); } catch (e) { /* ignore */ }
        net._slots = [];
        net._guestConn = null;
        net._peer = null;
        net._rx = [];
        net._guestPartial.clear();
        net.state = 'idle';
        net.role = role || '';
        net.code = '';
        net.error = '';
        net.connects = 0;
        net._everConnected = false;
        net._locked = false;
    }

    net.available = function () { return true; };

    net.host = function () {
        reset('host');
        net.state = 'starting';
        const hostGen = net._gen;
        loadPeerLibrary(ok => {
            if (hostGen !== net._gen || net.role !== 'host' || net.state !== 'starting') return;
            if (!ok) return fail('Could not load the online play library. Check the internet connection.');
            openHostPeer(0);
        });
    };

    net.join = function (code) {
        reset('guest');
        net.state = 'starting';
        net.code = String(code || '').trim().toUpperCase();
        const gen = net._gen;
        loadPeerLibrary(ok => {
            if (gen !== net._gen) return;
            if (!ok) return fail('Could not load the online play library. Check the internet connection.');
            const peer = new Peer();
            net._peer = peer;
            peer.on('open', () => {
                if (gen !== net._gen) return;
                net.state = 'connecting';
                connectToHost();
            });
            peer.on('disconnected', () => { if (gen === net._gen && !peer.destroyed) { try { peer.reconnect(); } catch (e) { /* retry on next event */ } } });
            peer.on('error', err => {
                if (gen !== net._gen) return;
                if (err.type === 'peer-unavailable') {
                    if (net._everConnected) scheduleRejoin();
                    else fail('No room with that code. Check the code and try again.');
                    return;
                }
                if (net.state === 'starting' || net.state === 'connecting') fail('Online error: ' + err.type);
            });
        });
    };

    // Host: slot is the guest number (0 ...). Guest: slot is ignored, the message goes to the host.
    net.send = function (slot, bytes) {
        const data = toBytes(bytes);
        if (!data) return false;
        try {
            if (net.role === 'host') {
                const entry = net._slots[slot];
                if (!entry || entry.state !== 'connected' || !entry.conn) return false;
                return sendChunks(entry.conn, data);
            }
            if (net.state !== 'connected' || !net._guestConn) return false;
            return sendChunks(net._guestConn, data);
        } catch (e) {
            if (net.role === 'host') slotLost(slot);
            else guestLinkLost();
            return false;
        }
    };

    net.nextSize = function () { return net._rx.length ? net._rx[0].bytes.length : -1; };
    net.nextSlot = function () { return net._rx.length ? net._rx[0].slot : -1; };
    net.pop = function () { const entry = net._rx.shift(); return entry ? entry.bytes : null; };

    // Host: how many guests have ever joined, the state of each (0 gone, 1 connected, 2 lost), and how often each link came up.
    net.slotCount = function () { return net._slots.length; };
    net.slotState = function (slot) {
        const entry = net._slots[slot];
        if (!entry || entry.state === 'closed') return 0;
        return entry.state === 'connected' ? 1 : 2;
    };
    net.slotConnects = function (slot) { const entry = net._slots[slot]; return entry ? entry.connects : 0; };

    // Host: no new guest may join any more (known guests can still come back).
    net.lockJoins = function () { net._locked = true; };

    // Host: sends the refusal to one guest and drops its place for good.
    net.dropSlot = function (slot) {
        const entry = net._slots[slot];
        if (!entry) return;
        const conn = entry.conn;
        entry.conn = null;
        entry.state = 'closed';
        if (conn) {
            try { conn.send(new Uint8Array([FRAME_REFUSED, 1])); } catch (e) { /* ignore */ }
            setTimeout(() => { try { conn.close(); } catch (e) { /* ignore */ } }, CLOSE_DELAY_MS);
        }
    };

    net.close = function () {
        // Let a last message (the "I leave" note) go out before the links are torn down.
        const conns = net._slots.map(slot => slot.conn).concat([net._guestConn]);
        const peer = net._peer;
        net._slots.forEach(slot => { slot.conn = null; });
        net._guestConn = null;
        net._peer = null;
        reset('');
        setTimeout(() => {
            for (const conn of conns) { try { if (conn) conn.close(); } catch (e) { /* ignore */ } }
            try { if (peer) peer.destroy(); } catch (e) { /* ignore */ }
        }, CLOSE_DELAY_MS);
    };

    // The state the C++ side reads: for the host the room (1 starting, 2 open, 6 error), for a guest the link to the host.
    net.stateCode = function () {
        switch (net.state) {
            case 'idle': return 0;
            case 'starting': return 1;
            case 'waiting': return 2;
            case 'connecting': return 3;
            case 'connected': return 4;
            case 'lost': return 5;
            default: return 6;
        }
    };

    window.fhNet = net;
})();
