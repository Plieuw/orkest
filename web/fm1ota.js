// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
//
// The FM-1 USB-MIDI update protocol over WebMIDI (after tools/fm1_install.py).
// Step 1: the running firmware (official
// or Felucca) reads parts of the package and stages the update loader. Step 2:
// the loader reads the whole image and writes it. Both steps are "the device
// asks, we answer": cmd 0x30 read requests on the logical image.

const HS_QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
const MAXDATA = 512;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const IS_OTA = (id) => /^ota-/i.test(id.model);

// errors carry a code the pages translate: notfound, model, stopped, noloader, lost, noreturn,
// mismatch (detail: the identity the device reports), badreq
function fail(code, msg, detail = "") { const e = new Error(msg); e.code = code; e.detail = detail; return e; }

export function pack7(data) {
  const out = []; let acc = 0, nb = 0;
  for (const b of data) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return out;
}

export function unpack7(s) {
  const out = []; let acc = 0, nb = 0;
  for (const b of s) {
    acc |= (b & 0x7F) << nb; nb += 7;
    while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; }
  }
  return out;
}

function response(addr, data, fl = 0) {
  const len = data.length;
  const body = [0x00, 0x59, 0x30, (len + 8) & 0xFF, ((len + 8) >> 8) & 0xFF, 0, fl,
    addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF,
    len & 0xFF, (len >> 8) & 0xFF, 0, ...data];
  let s = 0;
  for (let i = 6; i < body.length; i++) s += body[i];
  body.push(~s & 0xFF);
  return [0xF0, ...pack7(body), 0xF7];
}

function parseRequest(pkt) {
  if (pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const u = unpack7(pkt.slice(1, -1));
  if (u.length !== 15 || u[0] !== 0 || u[1] !== 0x59 || u[2] !== 0x30) return null;
  let s = 0;
  for (let i = 6; i < 14; i++) s += u[i];
  if ((~s & 0xFF) !== u[14]) return null;
  return { fl: u[6], addr: (u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24)) >>> 0, len: u[11] | (u[12] << 8) | (u[13] << 16) };
}

function parseIdentity(pkt) {
  const d = unpack7(pkt.slice(1, -1));
  if (d.length !== 34 || d[0] !== 0 || d[1] !== 0x59 || d[2] !== 0x11) return null;
  const txt = String.fromCharCode(...d.slice(6, 33)).replace(/\0+$/, "");
  const m = /^([^_]+)_(\d+)$/.exec(txt);
  return m ? { text: txt, model: m[1], version: parseInt(m[2], 10) } : null;
}

// a command frame of the FM-1 protocol: F0 pack7(00 59 cmd len24 payload ~sum) F7 (M-VAVE's FM1 Open uses the same)
function frame(cmd, payload = []) {
  let s = 0;
  for (const b of payload) s = (s + b) & 0xFF;
  return [0xF0, ...pack7([0x00, 0x59, cmd, payload.length & 0xFF, (payload.length >> 8) & 0xFF, 0, ...payload, ~s & 0xFF]), 0xF7];
}
function parseFrame(pkt) {
  if (pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const u = unpack7(pkt.slice(1, -1));
  if (u.length < 7 || u[0] !== 0 || u[1] !== 0x59 || (u[3] | (u[4] << 8) | (u[5] << 16)) !== u.length - 7) return null;
  let s = 0;
  for (let i = 6; i < u.length; i++) s = (s + u[i]) & 0xFF;
  return s === 0xFF ? { cmd: u[2], data: u.slice(6, -1) } : null;
}
// send a command and wait for the reply with that command number (null: none in time)
async function query(link, cmd, payload = [], ms = 1500, replyCmd = cmd) {
  link.drain();
  if (!link.send(frame(cmd, payload))) return null;
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const p = await link.read(Math.min(250, end - Date.now()));
    const f = p && parseFrame(p);
    if (f && f.cmd === replyCmd) return f.data;
  }
  return null;
}
const hexBytes = (h) => h.match(/../g).map((x) => parseInt(x, 16));

// one MIDI in/out pair with a SysEx queue
class Link {
  constructor(input, output) {
    this.input = input; this.output = output; this.q = []; this.waiter = null;
    input.onmidimessage = (ev) => {
      if (ev.data[0] !== 0xF0) return;
      this.q.push(Array.from(ev.data));
      if (this.waiter) { const w = this.waiter; this.waiter = null; w(); }
    };
  }
  get lost() { return this.input.state === "disconnected" || this.output.state === "disconnected"; }
  // false when the port is gone (unplugged): WebMIDI throws on a disconnected output
  send(bytes) {
    try { this.output.send(bytes); return true; } catch (_) { return false; }
  }
  drain() { this.q.length = 0; }
  async read(ms) {
    if (!this.q.length) {
      let tm;
      await Promise.race([new Promise((r) => { this.waiter = r; }), new Promise((r) => { tm = setTimeout(r, ms); })]);
      clearTimeout(tm);
      this.waiter = null;
    }
    return this.q.shift() || null;
  }
  close() { this.input.onmidimessage = null; }
}

async function handshake(link, tries = 3) {
  link.drain();
  for (let t = 0; t < tries; t++) {
    if (!link.send(HS_QUERY)) return null;
    const end = Date.now() + 1000;
    while (Date.now() < end) {
      const p = await link.read(end - Date.now());
      if (!p) break;
      const id = parseIdentity(p);
      if (id) return id;
    }
  }
  return null;
}

export class Updater {
  constructor(access, log) {
    this.access = access;
    this.log = log || (() => {});
  }

  // every in/out pair with the same port name that answers the handshake
  async find(filter) {
    const outs = [...this.access.outputs.values()];
    for (const input of this.access.inputs.values()) {
      if (input.state === "disconnected") continue;
      if (!/fm.?1|felucca|orkest|ota|composite|sinco|usb.?midi/i.test(input.name || "")) continue;   // never probe other gear
      const output = outs.find((o) => o.name === input.name && o.state !== "disconnected");
      if (!output) continue;
      try { await input.open(); await output.open(); } catch (_) { continue; }
      const link = new Link(input, output);
      const id = await handshake(link, 2);
      if (id && (!filter || filter(id))) return { link, id, name: input.name };
      link.close();
    }
    return null;
  }

  async waitFor(filter, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const dev = await this.find(filter);
      if (dev) return dev;
      await sleep(1000);
    }
    return null;
  }

  // serve read requests until the device asks for one of the finish addresses; stops early
  // when the port disappears (lost: true) instead of waiting out idleMs
  async serve(link, image, finishAddr, idleMs, onProgress) {
    let served = 0, last = Date.now();
    for (;;) {
      const pkt = await link.read(1000);
      if (!pkt) {
        if (link.lost) return { served, finished: false, lost: true };
        if (Date.now() - last > idleMs) return { served, finished: false };
        continue;
      }
      const r = parseRequest(pkt);
      if (!r) continue;
      last = Date.now();
      if (r.addr === 0xE0000000 || r.addr === 0xF0000000) {
        link.send(response(r.addr, [...new TextEncoder().encode("success"), 0]));
        if (r.addr === finishAddr) return { served, finished: true };
        continue;
      }
      if (r.len > MAXDATA || r.addr + r.len > image.length) throw fail("badreq", `bad request ${r.addr.toString(16)}+${r.len}`);
      await sleep(10);
      if (!link.send(response(r.addr, Array.from(image.subarray(r.addr, r.addr + r.len)), r.fl))) {
        return { served, finished: false, lost: true };
      }
      served++;
      if (onProgress) onProgress(served, r.addr);
    }
  }

  // FM1 Open (M-VAVE, identity FM-1_8xxxxxx) takes an update only after it was armed with the package's size and
  // build hash (as M-VAVE's workbench: command 49 capability, command 50 arm). Firmware without command 49 (V15,
  // Felucca) does not answer it: nothing to arm.
  async arm(link, image, buildId) {
    const cap = await query(link, 49, [], 1500);
    if (!cap) return false;
    if (cap.length !== 47 || String.fromCharCode(...cap.slice(0, 4)) !== "FMC1") throw fail("stopped", "the FM-1 answered the capability query unexpectedly");
    if (cap[6] !== 0) throw fail("stopped", "the FM-1 is busy with another update");
    if (!buildId) throw fail("stopped", "this package has no build hash to arm FM1 Open with");
    const n = image.length;
    const ack = await query(link, 50, [1, cap[5], n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255, ...hexBytes(buildId)], 3000);
    if (!ack || ack.length !== 1 || ack[0] !== 0) throw fail("stopped", "the FM-1 refused the package" + (ack ? ` (${ack[0]})` : ""));
    this.log(`armed: family ${cap[5]}, ${n} B`);
    return true;
  }

  // full install: running firmware -> loader -> new firmware. buildId: the package's build hash (hex), for FM1 Open
  async install(image, product, onStep, buildId = null) {
    const step = onStep || (() => {});
    const dev = await this.find((id) => !IS_OTA(id));
    if (!dev) throw fail("notfound", "FM-1 not found (USB cable, and no other app using it?)");
    const [model] = product.split("_");
    if (dev.id.model !== model) { dev.link.close(); throw fail("model", `the device is ${dev.id.text}, the package is for ${product}`); }
    step("start", dev.id.text);
    if (/^FM-1_8\d{6}$/.test(dev.id.text)) {
      try { await this.arm(dev.link, image, buildId); } catch (e) { dev.link.close(); throw e; }
    }
    dev.link.send(UPGRADE);
    await sleep(2000);
    const s1 = await this.serve(dev.link, image, 0xE0000000, 8000, (n) => step("verify", n));
    dev.link.close();
    if (!s1.finished) throw fail(s1.lost ? "lost" : "stopped", `the device ${s1.lost ? "was disconnected" : "stopped"} after ${s1.served} requests: nothing was written`);
    step("loader");
    await sleep(3000);
    const ota = await this.waitFor((id) => id.model === "ota-" + model, 30000);
    if (!ota) throw fail("noloader", "the update loader did not appear. Replug the USB cable and press Install again: the device stays in update mode until it is finished.");
    const s2 = await this.write(ota, image, step);
    if (!s2.finished) throw fail(s2.lost ? "lost" : "stopped", `the loader ${s2.lost ? "was disconnected" : "stopped"} after ${s2.served} requests. Replug and press Install again to resume.`);
    step("reboot");
    await sleep(3000);
    const back = await this.waitFor((id) => !IS_OTA(id), 40000);
    if (!back) throw fail("noreturn", "the device did not come back: power-cycle it");
    back.link.close();
    if (back.id.text !== product) throw fail("mismatch", `installed, but the device reports ${back.id.text}`, back.id.text);
    step("done", back.id.text);
    return back.id.text;
  }

  // official V15's four factory DX7 voice banks back into its flash (16 KiB, 512 bytes a message; V15 answers each
  // with command 0, status 0), as M-VAVE's workbench does after putting V15 back
  async restoreFactory(factory, onProgress) {
    const dev = await this.waitFor((id) => id.text === "FM-1_015", 20000);
    if (!dev) throw fail("noreturn", "official V15 did not answer");
    const ack = async (cmd, payload) => {
      const r = await query(dev.link, cmd, payload, 10000, 0);
      if (!r || r.length !== 1 || r[0] !== 0) throw fail("stopped", "V15 did not accept its factory voices");
    };
    try {
      for (let bank = 0; bank < 16384; bank += 4096) {
        await ack(33, [5, 0, bank >>> 8, 0, 0]);
        for (let off = bank; off < bank + 4096; off += 512) {
          await ack(34, [5, off & 255, off >>> 8, 0, 0, 0, 2, 0, ...factory.subarray(off, off + 512)]);
          if (onProgress) onProgress(off + 512, 16384);
        }
      }
    } finally { dev.link.close(); }
  }

  // step 2 with the loader found by find()/waitFor()
  async write(ota, image, step) {
    ota.link.send(UPGRADE);
    await sleep(2000);
    const s2 = await this.serve(ota.link, image, 0xF0000000, 180000, (n) => step("write", n));
    ota.link.close();
    return s2;
  }

  // resume: the device is already in update mode (loader) -> true when the write finished
  async resume(image, onStep, product = null) {
    const model = product && product.split("_")[0];
    const ota = await this.find((id) => model ? id.model === "ota-" + model : IS_OTA(id));
    if (!ota) return false;
    const step = onStep || (() => {});
    const result = await this.write(ota, image, step);
    if (!result.finished) return false;
    if (product) {
      step("reboot");
      await sleep(3000);
      const back = await this.waitFor((id) => id.model === model, 40000);
      if (!back) throw fail("noreturn", "The FM-1 did not return after resuming the write.");
      back.link.close();
      if (back.id.text !== product) throw fail("mismatch", "The FM-1 reports a different firmware after resuming.", back.id.text);
      step("done", back.id.text);
    }
    return true;
  }
}
