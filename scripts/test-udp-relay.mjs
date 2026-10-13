#!/usr/bin/env node
// Validate the impairment fixture itself with real localhost datagrams.
import assert from 'node:assert/strict';
import {createSocket} from 'node:dgram';
import {once} from 'node:events';
import {performance} from 'node:perf_hooks';
import {setTimeout as delay} from 'node:timers/promises';
import {test} from 'node:test';
import {UdpRelay, networkProfiles} from './udp-relay.mjs';

test('UDP relay imposes delay, bounded queue loss, then restores delivery', {timeout: 5000}, async () => {
  const nano = createSocket('udp4');
  const browser = createSocket('udp4');
  let relay;
  const received = [];
  try {
    for (const socket of [nano, browser]) {
      const ready = once(socket, 'listening');
      socket.bind(0, '127.0.0.1');
      await ready;
    }
    nano.on('message', (data, source) => nano.send(data, source.port, source.address));
    browser.on('message', data => received.push(data));
    relay = await UdpRelay.create(nano.address().port);
    const echo = async text => {
      const reply = once(browser, 'message', {signal: AbortSignal.timeout(2000)});
      browser.send(Buffer.from(text), relay.ports.browser, '127.0.0.1');
      assert.equal((await reply)[0].toString(), text);
    };
    const started = performance.now();
    await echo('baseline');
    assert.ok(performance.now() - started >= 2 * networkProfiles.baseline.delayMs,
      'Both propagation delays must be applied');
    relay.setProfile('constrained');
    received.length = 0;
    for (let index = 0; index < 50; index++) {
      const packet = Buffer.alloc(1000);
      packet.writeUInt32BE(index);
      nano.send(packet, relay.ports.nano, '127.0.0.1');
    }
    await delay(600);
    const stats = relay.snapshot();
    assert.ok(stats.downlink.dropped > 0, 'The bottleneck must discard excess queue input');
    assert.ok(stats.downlink.maxQueuedBytes <= networkProfiles.constrained.queueBytes);
    assert.ok(received.length > 0 && received.length < 50);
    const sequence = received.map(packet => packet.readUInt32BE());
    assert.deepEqual(sequence, [...sequence].sort((a, b) => a - b), 'Retained packets preserve order');
    relay.setProfile('recovered');
    await echo('recovered');
    assert.equal(relay.snapshot().downlink.sendErrors, 0);
  } finally {
    await relay?.close();
    nano.close();
    browser.close();
  }
});
