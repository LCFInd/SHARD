
/* the AudioWorklet side: the page sends the wasm bytes, then input; screen and LEDs come back */
class FeluccaProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.core = new EmuCore((m, t) => this.port.postMessage(m, t || []));
    this.port.onmessage = (ev) => {
      const m = ev.data;
      if (m.type === "wasm") this.core.load(m.bytes).catch((err) => this.port.postMessage({ type: "error", msg: String(err) }));
      else this.core.handle(m);
    };
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    this.core.render(L, R);
    return true;
  }
}
registerProcessor("felucca", FeluccaProcessor);
