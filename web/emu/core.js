/* FELUCCA + SHARD emulator core: one wasm instance of the firmware, driven by audio time.
 * Runs inside the AudioWorklet (emu-worklet.js) or, as a fallback, on the main thread.
 * Every 16 ms of rendered audio one UI frame runs (ui_input, ui_draw), as the device's main loop does. */
(function (G) {
  const FRAME_SAMPLES = 44100 * 0.016;           /* 705.6 samples per UI frame */

  class EmuCore {
    constructor(post) {
      this.post = post;                          /* (msg, transfer) to the page */
      this.e = null;
      this.acc = 0;
      this.queue = [];
      this.leds = new Uint8Array(42);
      this.lastLeds = new Uint8Array(42).fill(255);
      this.frames = 0;
      this.peak = 0;
    }
    async load(bytes) {
      const { instance } = await WebAssembly.instantiate(bytes, { env: {} });
      this.e = instance.exports;
      this.e.emu_init();
      this.e.emu_demo();
      this.shard = this.e.emu_shard_index();
      for (const m of this.queue) this.handle(m);
      this.queue = [];
      this.frame();
      this.post({ type: "ready" });
    }
    handle(m) {
      const e = this.e;
      if (!e) { this.queue.push(m); return; }
      switch (m.type) {
        case "btn": e.emu_button(m.id, m.down ? 1 : 0); break;
        case "key": e.emu_key(m.k, m.down ? 1 : 0); break;
        case "knob": e.emu_knob(m.role, m.steps | 0); break;
        case "slot": {
          const base = e.emu_slot_ptr(m.k), size = e.emu_slot_size();
          const mem = new Uint8Array(e.memory.buffer, base, size);
          mem.fill(0);
          mem.set(m.hdr, 0);
          mem.set(m.data, 512);
          e.emu_slot_loaded(m.k);
          e.emu_tempo_set(m.k + 1, m.bpm | 0);
          const hits = e.emu_sens_set(m.k, m.sens ?? 64);
          const set = e.emu_set_src(m.k + 1);
          const div = set && m.div >= 0 ? e.emu_set_div(m.div) : 0;
          this.post({ type: "slot", k: m.k, src: !!set, div: !!div, found: e.emu_tempo_found(m.k + 1), hits });
          break;
        }
        case "tempo":                              /* the slot's tempo after the fact (0: AUTO) */
          e.emu_tempo_set(m.k + 1, m.bpm | 0);
          this.post({ type: "tempo", k: m.k, found: e.emu_tempo_found(m.k + 1) });
          break;
        case "sens":
          this.post({ type: "sens", k: m.k, hits: e.emu_sens_set(m.k, m.sens) });
          break;
        case "div":
          this.post({ type: "div", ok: !!e.emu_set_div(m.div) });
          break;
        case "reset":
          e.emu_init();
          if (m.demo) e.emu_demo();
          break;
      }
    }
    frame() {
      const e = this.e;
      e.emu_frame();
      this.frames++;
      const d = e.emu_dirty();
      if (d & 1) {
        const scr = new Uint16Array(e.memory.buffer, e.emu_screen(), 240 * 240).slice();
        this.post({ type: "screen", px: scr, off: !!(d & 2) }, [scr.buffer]);
      }
      let changed = false;
      for (let i = 0; i < 42; i++) {
        const v = e.emu_led(i);
        this.leds[i] = v;
        if (v !== this.lastLeds[i]) changed = true;
      }
      if (changed) {
        this.lastLeds.set(this.leds);
        this.post({ type: "leds", leds: this.leds.slice() });
      }
      if (this.frames % 15 === 0)
        this.post({ type: "status", playing: e.emu_playing(), sel: 0, peak: this.peak });
      this.peak = 0;
    }
    /* fill two Float32Arrays of n samples (n a multiple of 32) */
    render(L, R) {
      const e = this.e, n = L.length;
      if (!e) { L.fill(0); R.fill(0); return; }
      for (let at = 0; at < n; at += 128) {
        const m = Math.min(128, n - at);
        const o = new Int32Array(e.memory.buffer, e.emu_render(m), 2 * m);
        for (let i = 0; i < m; i++) {
          let l = o[2 * i] / 32768, r = o[2 * i + 1] / 32768;
          l = l > 1 ? 1 : l < -1 ? -1 : l;
          r = r > 1 ? 1 : r < -1 ? -1 : r;
          L[at + i] = l; R[at + i] = r;
          const a = l < 0 ? -l : l;
          if (a > this.peak) this.peak = a;
        }
        this.acc += m;
        while (this.acc >= FRAME_SAMPLES) {
          this.acc -= FRAME_SAMPLES;
          this.frame();
        }
      }
    }
  }
  G.EmuCore = EmuCore;
})(typeof globalThis !== "undefined" ? globalThis : self);
