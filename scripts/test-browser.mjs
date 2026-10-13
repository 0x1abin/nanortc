#!/usr/bin/env node
// Local Chrome interop; Node 22+ built-ins only. No browser profile is reused.
// Usage: node scripts/test-browser.mjs <browser_interop binary> [--media] [--weak-network]
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {access, mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createServer} from 'node:net';
import {createSocket} from 'node:dgram';
import {once} from 'node:events';
import {setTimeout as delay} from 'node:timers/promises';
import {UdpRelay, networkProfiles} from './udp-relay.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const binary = resolve(process.argv[2] || 'build/examples/browser_interop/browser_interop');
const weakNetwork = process.argv.includes('--weak-network');
const media = process.argv.includes('--media') || weakNetwork;
if (media) {
  for (const path of ['opusSampleFrames/sample-000.opus', 'h264SampleFrames/frame-0001.h264']) {
    await access(resolve(root, 'examples/sample_data', path)).catch(() => {
      throw Error(`Missing media fixture: examples/sample_data/${path}; run git submodule update --init examples/sample_data`);
    });
  }
}
const temp = await mkdtemp(`${tmpdir()}/nanortc-browser-`);
const children = [];
const relays = [];
let activeRelay, activePeer, phases;
const peerStats = peer => [...peer.log.matchAll(/^\[stats\] (.+)$/gm)].map(match => JSON.parse(match[1]));
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
async function exercise(nanoRole, peerId, withMedia, relayPorts) {
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
  const candidate = port => `candidate:relay 1 UDP 2130706431 127.0.0.1 ${port} typ host`;
  const routeSdp = (sdp, port) => sdp.replace(/^a=candidate:.*$/gm, `a=${candidate(port)}`);
  const local = async description => {
    await pc.setLocalDescription(description);
    await until(() => pc.iceGatheringState === 'complete', 'ICE gathering');
    await send({type: pc.localDescription.type, sdp: relayPorts ? routeSdp(pc.localDescription.sdp, relayPorts.nano) : pc.localDescription.sdp});
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
          if (relayPorts) message.sdp = routeSdp(message.sdp, relayPorts.browser);
          await pc.setRemoteDescription(message);
          if (message.type === 'offer') await local(await pc.createAnswer());
        } else if (message.type === 'candidate' && message.candidate) {
          await pc.addIceCandidate({candidate: relayPorts ? candidate(relayPorts.browser) : message.candidate, sdpMid: '0', sdpMLineIndex: 0});
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
    let network;
    if (relayPorts) {
      const snapshot = async () => {
        const reports = [...(await pc.getStats()).values()].filter(s => s.type === 'inbound-rtp');
        return {videoFrames: reports.find(s => s.kind === 'video')?.framesDecoded || 0,
          audioSamples: reports.find(s => s.kind === 'audio')?.totalSamplesReceived || 0,
          ice: pc.iceConnectionState};
      };
      const changeNetwork = phase => new Promise(resolve => {
        window.__nanortcNetworkApplied = resolve;
        window.nanortcNetwork(phase);
      });
      const reliable = channels.find(s => s.channel.ordered);
      const observations = [];
      for (const [phase, duration] of [['baseline', 3000], ['constrained', 8000], ['recovered', 8000]]) {
        await changeNetwork(phase);
        const before = await snapshot();
        const end = Date.now() + duration;
        let echoes = 0;
        while (Date.now() < end) {
          const message = `network:${phase}:${echoes}`;
          reliable.channel.send(message);
          await until(() => reliable.messages.includes(message), `${phase} reliable echo`);
          reliable.messages.splice(reliable.messages.indexOf(message), 1);
          echoes++;
          await sleep(1000);
        }
        const after = await snapshot();
        if (!['connected', 'completed'].includes(after.ice)) throw Error(`${phase}: ICE disconnected`);
        if (after.audioSamples <= before.audioSamples) throw Error(`${phase}: audio stopped`);
        if (phase !== 'constrained' && after.videoFrames <= before.videoFrames) throw Error(`${phase}: video did not decode`);
        observations.push({phase, before, after, echoes});
      }
      network = {observations, limitation: 'Fixed encoded frames do not follow the BWE target; constrained video may stall until the next IDR. Recovery timing of an adaptive encoder is tested by the SDK simulator.'};
    }
    return {nanoRole, echoed, trickledCandidates, network, maxMessageSize: pc.sctp.maxMessageSize, inbound: inbound.map(s => ({kind: s.kind, packetsReceived: s.packetsReceived, framesDecoded: s.framesDecoded, totalSamplesReceived: s.totalSamplesReceived}))};
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
    if (message.method === 'Runtime.bindingCalled' && message.params.name === 'nanortcNetwork') {
      const phase = message.params.payload;
      activeRelay.setProfile(phase);
      phases.push({phase, statsOffset: peerStats(activePeer).length, relay: activeRelay.snapshot()});
      console.log(`[network] ${activePeer.name}: ${phase}`);
      call('Runtime.evaluate', {expression: 'window.__nanortcNetworkApplied()'}).catch(error => {
        console.error(error);
        socket.close();
      });
      return;
    }
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
  if (weakNetwork) await call('Runtime.enable');
  for (const role of ['answer', 'offer']) {
    const port = await freePort();
    const server = await start('python3', ['examples/browser_interop/signaling_server.py', '--port', String(port), '--discovery-port', '0'], `signaling-${role}`);
    const base = `http://127.0.0.1:${port}`;
    await waitFor(() => fetch(base).then(r => r.ok).catch(() => false));
    const {id: peerId} = await (await fetch(`${base}/join`, {method: 'POST'})).json();
    await call('Page.navigate', {url: base});
    await waitFor(() => evaluate(`location.href === ${JSON.stringify(base + '/')} && document.readyState === 'complete'`));
    if (weakNetwork) await call('Runtime.addBinding', {name: 'nanortcNetwork'});
    const nanoPort = await freePort(true);
    activeRelay = weakNetwork ? await UdpRelay.create(nanoPort) : null;
    if (activeRelay) relays.push(activeRelay);
    phases = [];
    const args = [`--${role}`, '-b', '127.0.0.1', '-p', String(nanoPort), '--host-only', '-s', `127.0.0.1:${port}`];
    if (media) args.push('-a', 'examples/sample_data/opusSampleFrames', '-v', 'examples/sample_data/h264SampleFrames');
    if (weakNetwork) args.push('--stats');
    const peer = await start(binary, args, `peer-${role}`);
    activePeer = peer;
    await waitFor(() => {
      if (peer.exitCode !== null) throw Error(peer.log);
      return peer.log.includes('Joined as peer');
    });
    const result = await evaluate(`(${exercise.toString()})(${JSON.stringify(role)}, ${peerId}, ${media}, ${JSON.stringify(activeRelay?.ports || null)})`);
    assert.equal(result.echoed, 18);
    assert.ok(result.trickledCandidates > 0);
    assert.equal(result.maxMessageSize, 4096);
    if (weakNetwork) {
      const samples = peerStats(peer);
      const phaseResults = phases.map((entry, index) => {
        const stats = samples.slice(entry.statsOffset, phases[index + 1]?.statsOffset);
        assert.ok(stats.length > 0, `No public BWE samples during ${entry.phase}`);
        return {phase: entry.phase, profile: networkProfiles[entry.phase], samples: stats,
          relayBefore: entry.relay, relayAfter: phases[index + 1]?.relay || activeRelay.snapshot()};
      });
      const baseline = phaseResults[0].samples.at(-1).estimate_bps;
      const constrained = Math.min(...phaseResults[1].samples.map(s => s.estimate_bps));
      assert.ok(constrained < baseline, `BWE did not decrease (${baseline} -> ${constrained})`);
      assert.ok(phaseResults[1].relayAfter.downlink.dropped > phaseResults[1].relayBefore.downlink.dropped, 'The constrained link did not exercise queue loss');
      for (const phase of phaseResults) for (const sample of phase.samples) {
        if (sample.remb_cap_valid) assert.ok(sample.estimate_bps <= sample.remb_cap_bps, 'BWE exceeded current REMB cap');
      }
      result.network.phases = phaseResults;
      result.network.estimate = {baseline, constrained, recovered: phaseResults[2].samples.at(-1).estimate_bps};
      await writeFile(`${temp}/peer-${role}.log`, peer.log);
      await writeFile(`${temp}/result-${role}.json`, JSON.stringify(result, null, 2));
      await activeRelay.close();
    }
    console.log(JSON.stringify(result));
    await stop(peer);
    await stop(server);
  }
  console.log('Browser interop passed.');
  if (weakNetwork) console.log(`Weak-network logs: ${temp}`);
} catch (error) {
  failed = true;
  for (const child of children) await writeFile(`${temp}/${child.name}.log`, child.log);
  console.error(`Logs: ${temp}`);
  throw error;
} finally {
  socket?.close();
  for (const child of children.reverse()) await stop(child);
  for (const relay of relays) await relay.close();
  await rm(failed || weakNetwork ? `${temp}/profile` : temp, {recursive: true, force: true, maxRetries: 10, retryDelay: 100});
}
