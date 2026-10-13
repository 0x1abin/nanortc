// Local, bounded UDP link used by the real-browser weak-network fixture.
// Each direction has its own FIFO serialization clock; no global tc/netns state.
import {createSocket} from 'node:dgram';
import {once} from 'node:events';
import {performance} from 'node:perf_hooks';

export const networkProfiles = {
  baseline: {bitrateBps: 2_000_000, delayMs: 10, queueMs: 120, queueBytes: 48_000},
  constrained: {bitrateBps: 300_000, delayMs: 60, queueMs: 120, queueBytes: 12_000},
  recovered: {bitrateBps: 2_000_000, delayMs: 10, queueMs: 120, queueBytes: 48_000},
};

class Link {
  constructor(socket, profile) {
    this.socket = socket;
    this.profile = profile;
    this.queue = [];
    this.queuedBytes = 0;
    this.availableAt = 0;
    this.timer = null;
    this.stats = {forwarded: 0, bytes: 0, dropped: 0, maxQueuedBytes: 0, maxDelayMs: 0, sendErrors: 0};
  }
  send(data, destination) {
    const now = performance.now();
    const finish = Math.max(now, this.availableAt) + data.length * 8000 / this.profile.bitrateBps;
    if (finish - now > this.profile.queueMs || this.queuedBytes + data.length > this.profile.queueBytes) {
      this.stats.dropped++;
      return;
    }
    this.availableAt = finish;
    const deadline = Math.max(finish + this.profile.delayMs, this.queue.at(-1)?.deadline || 0);
    this.queue.push({data, destination, deadline, entered: now});
    this.queuedBytes += data.length;
    this.stats.maxQueuedBytes = Math.max(this.stats.maxQueuedBytes, this.queuedBytes);
    this.arm();
  }
  arm() {
    if (this.timer || !this.queue.length) return;
    this.timer = setTimeout(() => {
      this.timer = null;
      const now = performance.now();
      while (this.queue.length && this.queue[0].deadline <= now) {
        const item = this.queue.shift();
        this.queuedBytes -= item.data.length;
        this.stats.maxDelayMs = Math.max(this.stats.maxDelayMs, now - item.entered);
        this.socket.send(item.data, item.destination.port, item.destination.address, error => {
          if (error) this.stats.sendErrors++;
          else { this.stats.forwarded++; this.stats.bytes += item.data.length; }
        });
      }
      this.arm();
    }, Math.max(1, Math.ceil(this.queue[0].deadline - performance.now())));
  }
  close() {
    clearTimeout(this.timer);
    this.timer = null;
    this.queue = [];
    this.queuedBytes = 0;
  }
}

export class UdpRelay {
  static async create(nanortcPort) {
    const relay = new UdpRelay(nanortcPort);
    try {
      for (const socket of [relay.browser, relay.nano]) {
        const bound = once(socket, 'listening');
        socket.bind(0, '127.0.0.1');
        await bound;
      }
      return relay;
    } catch (error) {
      await relay.close();
      throw error;
    }
  }
  constructor(nanortcPort) {
    this.browser = createSocket('udp4');
    this.nano = createSocket('udp4');
    this.destination = {address: '127.0.0.1', port: nanortcPort};
    this.browserAddress = null;
    this.unroutable = 0;
    this.uplink = new Link(this.nano, networkProfiles.baseline);
    this.downlink = new Link(this.browser, networkProfiles.baseline);
    this.browser.on('message', (data, source) => {
      this.browserAddress = {address: source.address, port: source.port};
      this.uplink.send(data, this.destination);
    });
    this.nano.on('message', (data, source) => {
      if (source.port !== this.destination.port || !this.browserAddress) {
        this.unroutable++;
        return;
      }
      this.downlink.send(data, this.browserAddress);
    });
  }
  get ports() { return {browser: this.browser.address().port, nano: this.nano.address().port}; }
  setProfile(name) {
    const profile = networkProfiles[name];
    if (!profile) throw Error(`Unknown network profile: ${name}`);
    this.downlink.profile = profile;
    // Feedback/control travels through the same propagation delay, with a
    // separate 2Mbps upstream. The bottleneck is NanoRTC's media egress.
    this.uplink.profile = {...profile, bitrateBps: 2_000_000};
  }
  snapshot() {
    return {downlink: {...this.downlink.stats}, uplink: {...this.uplink.stats}, unroutable: this.unroutable};
  }
  async close() {
    this.uplink.close();
    this.downlink.close();
    await Promise.all([this.browser, this.nano].map(socket => new Promise(resolve => {
      try { socket.close(resolve); } catch { resolve(); }
    })));
  }
}
