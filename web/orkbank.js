// SPDX-License-Identifier: GPL-3.0-only
// Orkest on FM1 Open: put the orchestra's sound bank into the FM1's user flash over Web MIDI (SysEx).
// The protocol is the firmware's (v14/app/orkest_bank.c): INFO, BEGIN, DATA (512 bytes a message, each answered
// with the next offset), END (the FM1 checks the CRC, then commits). A NAK carries the offset the FM1 expects:
// the upload resumes from there. Nothing is used until END succeeds.

const BANK_HDR = [0xF0, 0x7D, 0x4F, 0x4B];
const BANK_CHUNK = 512;
const BANK_PORTS = /fm.?1|orkest|usb.?midi/i;

function bput7(v, n) {
  const out = [];
  for (let i = 0; i < n; i++, v = Math.floor(v / 128)) out.push(v & 0x7F);
  return out;
}
function bget7(a, off, n) {
  let v = 0;
  for (let i = n - 1; i >= 0; i--) v = v * 128 + a[off + i];
  return v;
}
function bpack(bytes) {                      // 8 bytes per 7: the first holds the others' top bits
  const out = [];
  for (let g = 0; g < bytes.length; g += 7) {
    let top = 0;
    const grp = bytes.subarray(g, g + 7);
    grp.forEach((b, i) => { top |= (b >> 7) << i; });
    out.push(top);
    for (let i = 0; i < 7; i++) out.push(i < grp.length ? grp[i] & 0x7F : 0);
  }
  return out;
}
const bankFail = (code, message) => Object.assign(new Error(message), { code });

export class BankLink {
  constructor(input, output) {
    this.input = input; this.output = output; this.waiter = null;
    this.onmsg = (e) => {
      const d = e.data;
      if (d.length >= 6 && d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x4F && d[3] === 0x4B && this.waiter) {
        const w = this.waiter; this.waiter = null; w(d.subarray(4, d.length - 1));
      }
    };
    input.addEventListener("midimessage", this.onmsg);
  }
  close() { this.input.removeEventListener("midimessage", this.onmsg); }
  request(body, ms = 3000) {
    return new Promise((resolve) => {
      const t = setTimeout(() => { this.waiter = null; resolve(null); }, ms);
      this.waiter = (r) => { clearTimeout(t); resolve(r); };
      try { this.output.send([...BANK_HDR, ...body, 0xF7]); } catch (e) { clearTimeout(t); this.waiter = null; resolve(null); }
    });
  }
  async info() {
    const r = await this.request([0x10], 800);
    if (!r || r[0] !== 0x11) return null;
    const ascii = (a) => String.fromCharCode(...a).replace(/\0.*$/, "");
    return { state: r[1], total: bget7(r, 2, 5), flash: bget7(r, 7, 5), name: ascii(r.subarray(12, 28)), version: ascii(r.subarray(28)) };
  }
}

// an FM1 running Orkest V14: the first port pair that answers INFO
export async function findOrkest(midi) {
  for (const input of midi.inputs.values()) {
    if (input.state !== "connected" || !BANK_PORTS.test(input.name || "")) continue;
    const output = [...midi.outputs.values()].find((o) => o.name === input.name && o.state !== "disconnected");
    if (!output) continue;
    const link = new BankLink(input, output);
    const info = await link.info();
    if (info) return { link, info };
    link.close();
  }
  return null;
}

// write the bank: progress(bytesDone, total). Throws with .code: notfound, toobig, lost, rejected, crc
export async function uploadBank(midi, bank, progress = () => {}) {
  const dev = await findOrkest(midi);
  if (!dev) throw bankFail("notfound", "Orkest V14 not found on any MIDI port");
  const { link, info } = dev;
  try {
    if (bank.length > info.flash) throw bankFail("toobig", `the bank is ${bank.length} B, the FM1 holds ${info.flash} B`);
    let r = await link.request([0x20, ...bput7(bank.length, 5)], 5000);
    if (!r || r[0] !== 0x7E) throw bankFail(r ? "rejected" : "lost", "the FM1 did not start the upload");
    let off = 0, tries = 0;
    while (off < bank.length) {
      const chunk = bank.subarray(off, Math.min(bank.length, off + BANK_CHUNK));
      let sum = 0;
      chunk.forEach((b) => { sum ^= b; });
      r = await link.request([0x21, ...bput7(off, 5), ...bput7(chunk.length, 2), ...bpack(chunk), sum & 0x7F], 3000);
      if (r && r[0] === 0x7E && r[1] === 0x21) {
        off = bget7(r, 2, 5); tries = 0; progress(off, bank.length);
        continue;
      }
      if (++tries > 4) throw bankFail("lost", `no answer at ${off} B`);
      if (r && r[0] === 0x7D) off = bget7(r, 3, 5);          // the FM1 says where it is: resume there
      else {                                                // no answer: ask where it is with an empty probe
        await new Promise((s) => setTimeout(s, 300));
      }
    }
    r = await link.request([0x22], 15000);                  // END: the FM1 reads the whole bank back (CRC)
    if (!r || r[0] !== 0x7E) throw bankFail(r ? "crc" : "lost", "the FM1 did not accept the bank");
    progress(bank.length, bank.length);
    return await link.info();
  } finally {
    link.close();
  }
}
