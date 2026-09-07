#!/usr/bin/env node
// Local Chrome interop; Node 22+ built-ins only. No browser profile is reused.
// Usage: node scripts/test-browser.mjs <browser_interop binary> [--media]
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createServer} from 'node:net';
import {createSocket} from 'node:dgram';
import {once} from 'node:events';
import {setTimeout as delay} from 'node:timers/promises';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const binary = resolve(process.argv[2] || 'build/examples/browser_interop/browser_interop');
const media = process.argv.includes('--media');
const temp = await mkdtemp(`${tmpdir()}/nanortc-browser-`);
const children = [];
let socket;
let failed = false;
async function waitFor(check, timeout = 15000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    const value = await check();
    if (value) return value;
    await delay(50);
  }
  throw Error('Timed out waiting for browser/peer');
}
async function freePort(udp = false) {
  const server = udp ? createSocket('udp4') : createServer();
  const listening = once(server, 'listening');
  if (udp) server.bind(0, '127.0.0.1');
  else server.listen(0, '127.0.0.1');
  await listening;
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}
async function start(command, args, name) {
  const child = spawn(command, args, {cwd: root, stdio: ['ignore', 'pipe', 'pipe']});
  child.log = '';
  child.name = name;
  child.stdout.on('data', data => { child.log += data; });
  child.stderr.on('data', data => { child.log += data; });
  child.on('error', error => { child.log += error.stack; });
  children.push(child);
  await once(child, 'spawn');
  return child;
}
async function stop(child) {
  if (!child.pid || child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill('SIGTERM');
  const timer = setTimeout(() => child.kill('SIGKILL'), 2000);
  try { await exited; } finally { clearTimeout(timer); }
}

// Executed inside the actual browser, independently of the example UI.
async function exercise(nanoRole, peerId, withMedia) {
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  const until = async (check, label) => {
    const end = Date.now() + 20000;
    while (Date.now() < end) {
      if (await check()) return;
      await sleep(20);
    }
    throw Error(`Timeout: ${label}, ICE=${pc.iceConnectionState}`);
  };
  const pc = new RTCPeerConnection({iceServers: []});
  const channels = [];
  const attach = channel => {
    channel.binaryType = 'arraybuffer';
    const state = {channel, messages: []};
    channel.onmessage = event => state.messages.push(event.data);
    channels.push(state);
  };
  pc.ondatachannel = event => attach(event.channel);
  const video = document.createElement('video');
  video.autoplay = true;
  video.muted = true;
  video.srcObject = new MediaStream();
  document.body.append(video);
  pc.ontrack = event => { video.srcObject.addTrack(event.track); video.play().catch(() => {}); };
  const send = message => fetch(`/send?id=${peerId}`, {
    method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(message)
  });
  const local = async description => {
    await pc.setLocalDescription(description);
    await until(() => pc.iceGatheringState === 'complete', 'ICE gathering');
    await send({type: pc.localDescription.type, sdp: pc.localDescription.sdp});
  };
  let polling = true;
  let signalingError;
  let trickledCandidates = 0;
  const controller = new AbortController();
  const poll = (async () => {
    try {
      while (polling) {
        const response = await fetch(`/recv?id=${peerId}&timeout=1`, {signal: controller.signal});
        if (response.status === 204) continue;
        if (!response.ok) throw Error(`Signaling HTTP ${response.status}`);
        const message = await response.json();
        if (message.type === 'offer' || message.type === 'answer') {
          await pc.setRemoteDescription(message);
          if (message.type === 'offer') await local(await pc.createAnswer());
        } else if (message.type === 'candidate' && message.candidate) {
          await pc.addIceCandidate({candidate: message.candidate, sdpMid: '0', sdpMLineIndex: 0});
          trickledCandidates++;
        }
      }
    } catch (error) { if (polling) signalingError = error.message; }
  })();
  try {
    if (nanoRole === 'answer') {
      attach(pc.createDataChannel('reliable'));
      attach(pc.createDataChannel('unordered', {ordered: false, maxRetransmits: 0, protocol: 'control.v1'}));
      if (withMedia) {
        pc.addTransceiver('audio', {direction: 'recvonly'});
        pc.addTransceiver('video', {direction: 'recvonly'});
      }
      await local(await pc.createOffer());
    }
    await until(() => {
      if (signalingError) throw Error(signalingError);
      return channels.length && channels[0].channel.readyState === 'open';
    }, 'DataChannel open');
    if (nanoRole === 'offer') attach(pc.createDataChannel('unordered', {ordered: false, maxRetransmits: 0, protocol: 'control.v1'}));
    await until(() => channels.length === 2 && channels.every(s => s.channel.readyState === 'open' && s.messages.includes('hello')), 'both channel greetings');
    let echoed = 0;
    for (const state of channels) {
      state.messages.length = 0;
      const payloads = [new Uint8Array(0), new Uint8Array([0, 1, 255]),
        Uint8Array.from({length: 4096}, (_, i) => (i * 37 + 11) & 255),
        '', 'short', '长消息🙂', 'x'.repeat(4096), 'after-long', 'a\0b'];
      for (const payload of payloads) {
        state.channel.send(payload);
        await until(() => state.messages.length, `echo ${typeof payload}/${payload.length}`);
        const received = state.messages.shift();
        const correct = typeof payload === 'string' ? received === payload :
          received instanceof ArrayBuffer && received.byteLength === payload.length &&
          new Uint8Array(received).every((byte, i) => byte === payload[i]);
        if (!correct) throw Error(`Echo mismatch: sent ${typeof payload}/${payload.length}, received ${typeof received}/${typeof received === 'string' ? received.length : received.byteLength}`);
        echoed++;
      }
    }
    await until(() => {
      if (signalingError) throw Error(signalingError);
      return trickledCandidates > 0;
    }, 'forwarded local ICE candidate');
    let inbound = [];
    if (withMedia) {
      await until(async () => {
        inbound = [...(await pc.getStats()).values()].filter(s => s.type === 'inbound-rtp');
        return inbound.some(s => s.kind === 'video' && s.framesDecoded >= 10) &&
          inbound.some(s => s.kind === 'audio' && s.totalSamplesReceived > 0);
      }, 'decoded audio/video');
    }
    return {nanoRole, echoed, trickledCandidates, maxMessageSize: pc.sctp.maxMessageSize, inbound: inbound.map(s => ({kind: s.kind, packetsReceived: s.packetsReceived, framesDecoded: s.framesDecoded, totalSamplesReceived: s.totalSamplesReceived}))};
  } finally {
    polling = false;
    controller.abort();
    pc.close();
    video.remove();
    await poll;
  }
}

try {
  const chrome = await start(process.env.CHROME || 'google-chrome', ['--headless=new', '--no-sandbox', '--disable-dev-shm-usage', '--disable-background-networking', '--disable-features=WebRtcHideLocalIpsWithMdns', '--autoplay-policy=no-user-gesture-required', '--remote-debugging-port=0', `--user-data-dir=${temp}/profile`, 'about:blank'], 'chrome');
  const port = await waitFor(async () => {
    if (chrome.exitCode !== null) throw Error(chrome.log);
    return readFile(`${temp}/profile/DevToolsActivePort`, 'utf8').then(s => s.split('\n')[0]).catch(() => null);
  });
  const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  socket = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  let id = 0;
  const pending = new Map();
  socket.onmessage = event => {
    const message = JSON.parse(event.data);
    if (!pending.has(message.id)) return;
    const {resolve, reject, timer} = pending.get(message.id);
    clearTimeout(timer);
    pending.delete(message.id);
    message.error ? reject(Error(JSON.stringify(message.error))) : resolve(message.result);
  };
  socket.onclose = () => {
    for (const {reject, timer} of pending.values()) {
      clearTimeout(timer);
      reject(Error('Chrome debugging connection closed'));
    }
    pending.clear();
  };
  const call = (method, params = {}) => new Promise((resolve, reject) => {
    const seq = ++id;
    const timer = setTimeout(() => { pending.delete(seq); reject(Error(`CDP timeout: ${method}`)); }, 90000);
    pending.set(seq, {resolve, reject, timer});
    socket.send(JSON.stringify({id: seq, method, params}));
  });
  const evaluate = async expression => {
    const response = await call('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true});
    if (response.exceptionDetails) throw Error(JSON.stringify(response.exceptionDetails));
    return response.result.value;
  };
  console.log(await call('Browser.getVersion'));
  for (const role of ['answer', 'offer']) {
    const port = await freePort();
    const server = await start('python3', ['examples/browser_interop/signaling_server.py', '--port', String(port), '--discovery-port', '0'], `signaling-${role}`);
    const base = `http://127.0.0.1:${port}`;
    await waitFor(() => fetch(base).then(r => r.ok).catch(() => false));
    const {id: peerId} = await (await fetch(`${base}/join`, {method: 'POST'})).json();
    await call('Page.navigate', {url: base});
    await waitFor(() => evaluate(`location.href === ${JSON.stringify(base + '/')} && document.readyState === 'complete'`));
    const args = [`--${role}`, '-b', '127.0.0.1', '-p', String(await freePort(true)), '--host-only', '-s', `127.0.0.1:${port}`];
    if (media) args.push('-a', 'examples/sample_data/opusSampleFrames', '-v', 'examples/sample_data/h264SampleFrames');
    const peer = await start(binary, args, `peer-${role}`);
    await waitFor(() => {
      if (peer.exitCode !== null) throw Error(peer.log);
      return peer.log.includes('Joined as peer');
    });
    const result = await evaluate(`(${exercise.toString()})(${JSON.stringify(role)}, ${peerId}, ${media})`);
    assert.equal(result.echoed, 18);
    assert.ok(result.trickledCandidates > 0);
    assert.equal(result.maxMessageSize, 4096);
    console.log(JSON.stringify(result));
    await stop(peer);
    await stop(server);
  }
  console.log('Browser interop passed.');
} catch (error) {
  failed = true;
  for (const child of children) await writeFile(`${temp}/${child.name}.log`, child.log);
  console.error(`Logs: ${temp}`);
  throw error;
} finally {
  socket?.close();
  for (const child of children.reverse()) await stop(child);
  await rm(failed ? `${temp}/profile` : temp, {recursive: true, force: true, maxRetries: 10, retryDelay: 100});
}
