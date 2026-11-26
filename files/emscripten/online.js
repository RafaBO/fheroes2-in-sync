// Online play bridge for the browser build: one peer-to-peer link between two players, through the free PeerJS broker (no server of ours).
// The game (C++) polls window.fhNet; this file never touches game rules. Messages are opaque byte arrays.
(function () {
    'use strict';

    const PEER_SOURCES = [
        'https://unpkg.com/peerjs@1.5.4/dist/peerjs.min.js',
        'https://cdn.jsdelivr.net/npm/peerjs@1.5.4/dist/peerjs.min.js'
    ];
    const ID_PREFIX = 'fh2sync-';
    const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    const CHUNK_SIZE = 16000;
    const PING_EVERY_MS = 2000;
    const SILENCE_LIMIT_MS = 12000;
    const REJOIN_EVERY_MS = 3000;
    const JOIN_TIMEOUT_MS = 20000;

    const net = {
        // idle | starting | waiting (host, nobody joined yet) | connecting | connected | lost (link dropped, trying again) | error
        state: 'idle',
        role: '',
        code: '',
        error: '',
        connects: 0, // how often the link came up; the game re-sends its last message when this changes
        _peer: null,
        _conn: null,
        _rx: [],
        _partial: new Map(),
        _nextMsgId: 1,
        _lastRx: 0,
        _timer: 0,
        _rejoinTimer: 0,
        _joinTimer: 0,
        _everConnected: false,
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

    function onData(raw) {
        const frame = toBytes(raw);
        if (!frame || frame.length === 0) return;
        net._lastRx = Date.now();
        if (frame[0] === 0) return; // ping
        if (frame[0] !== 1 || frame.length < 9) return;
        const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
        const id = view.getUint32(1, true);
        const index = view.getUint16(5, true);
        const count = view.getUint16(7, true);
        const payload = frame.subarray(9);
        if (count === 1) {
            net._rx.push(payload.slice());
            return;
        }
        let entry = net._partial.get(id);
        if (!entry) {
            entry = { count, parts: new Array(count), got: 0 };
            net._partial.set(id, entry);
        }
        if (!entry.parts[index]) {
            entry.parts[index] = payload.slice();
            entry.got++;
        }
        if (entry.got === entry.count) {
            net._partial.delete(id);
            let total = 0;
            for (const part of entry.parts) total += part.length;
            const whole = new Uint8Array(total);
            let offset = 0;
            for (const part of entry.parts) {
                whole.set(part, offset);
                offset += part.length;
            }
            net._rx.push(whole);
        }
    }

    function startHeartbeat() {
        if (net._timer) return;
        net._timer = setInterval(() => {
            if (net.state !== 'connected') return;
            if (Date.now() - net._lastRx > SILENCE_LIMIT_MS) {
                linkLost();
                return;
            }
            try { net._conn.send(new Uint8Array([0])); } catch (e) { linkLost(); }
        }, PING_EVERY_MS);
    }

    function linkLost() {
        const conn = net._conn;
        net._conn = null;
        net._partial.clear();
        try { if (conn) conn.close(); } catch (e) { /* already closed */ }
        if (net.state === 'idle' || net.state === 'error') return;
        net.state = net._everConnected ? 'lost' : net.state;
        if (net.role === 'guest') scheduleRejoin();
        else net.state = 'lost';
    }

    function attach(conn) {
        const gen = net._gen;
        const opened = () => {
            if (gen !== net._gen) return;
            clearTimeout(net._joinTimer);
            net._conn = conn;
            net._lastRx = Date.now();
            net._everConnected = true;
            net.state = 'connected';
            net.connects++;
            startHeartbeat();
        };
        conn.on('open', opened);
        conn.on('data', data => { if (conn === net._conn || !net._conn) onData(data); });
        conn.on('close', () => { if (gen === net._gen && conn === net._conn) linkLost(); });
        conn.on('error', () => { if (gen === net._gen && conn === net._conn) linkLost(); });
        if (conn.open) opened();
    }

    function connectToHost() {
        const gen = net._gen;
        if (!net._peer || net._peer.destroyed) return;
        if (net._peer.disconnected) { try { net._peer.reconnect(); } catch (e) { /* retry later */ } }
        const conn = net._peer.connect(ID_PREFIX + net.code, { reliable: true, serialization: 'raw' });
        attach(conn);
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
            if (gen !== net._gen || net.state === 'connected' || net.state === 'idle') return;
            connectToHost();
        }, REJOIN_EVERY_MS);
    }

    function reset(role) {
        net._gen++;
        stopTimers();
        try { if (net._conn) net._conn.close(); } catch (e) { /* ignore */ }
        try { if (net._peer) net._peer.destroy(); } catch (e) { /* ignore */ }
        net._conn = null;
        net._peer = null;
        net._rx = [];
        net._partial.clear();
        net.state = 'idle';
        net.role = role || '';
        net.code = '';
        net.error = '';
        net.connects = 0;
        net._everConnected = false;
    }

    function openHostPeer(attempt) {
        const gen = net._gen;
        net.code = randomCode();
        const peer = new Peer(ID_PREFIX + net.code);
        net._peer = peer;
        peer.on('open', () => { if (gen === net._gen && net.state === 'starting') net.state = 'waiting'; });
        peer.on('connection', conn => {
            if (gen !== net._gen) return;
            // Only one guest. A second link is accepted only if the first one is gone (the guest coming back).
            if (net._conn && net._conn.open) { conn.on('open', () => conn.close()); return; }
            attach(conn);
        });
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

    net.available = function () { return true; };

    net.host = function () {
        reset('host');
        net.state = 'starting';
        loadPeerLibrary(ok => {
            if (net.role !== 'host' || net.state !== 'starting') return;
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

    net.send = function (bytes) {
        if (net.state !== 'connected' || !net._conn) return false;
        const data = toBytes(bytes);
        if (!data) return false;
        const count = Math.max(1, Math.ceil(data.length / CHUNK_SIZE));
        if (count > 65535) return false;
        const id = net._nextMsgId++ >>> 0;
        try {
            for (let i = 0; i < count; i++) {
                const part = data.subarray(i * CHUNK_SIZE, Math.min(data.length, (i + 1) * CHUNK_SIZE));
                const frame = new Uint8Array(9 + part.length);
                const view = new DataView(frame.buffer);
                frame[0] = 1;
                view.setUint32(1, id, true);
                view.setUint16(5, i, true);
                view.setUint16(7, count, true);
                frame.set(part, 9);
                net._conn.send(frame);
            }
        } catch (e) {
            linkLost();
            return false;
        }
        return true;
    };

    net.nextSize = function () { return net._rx.length ? net._rx[0].length : -1; };
    net.pop = function () { return net._rx.shift(); };

    net.close = function () { reset(''); };

    // Codes the C++ side reads.
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
