// The page's side of gpu: a GPU by WebGPU, `navigator.gpu`.  A device,
// a buffer and a pipeline are WebGPU's own objects, given to the
// program as they are (a foreign value is the host's object) and taken
// back as they are: `Open` gives the device, `Close` destroys it;
// `NewBuffer` gives a buffer, `FreeBuffer` destroys it -- each at the
// end of the `with` that holds it.  A buffer the program drops
// otherwise is destroyed by WebGPU when the collector finds it; a
// pipeline is the device's and goes with it.  Sizes and offsets arrive
// as BigInts, bytes as a Uint8Array, a kernel's code as a string
// (`$toJs` makes it text).
//
// What WebGPU says of an operation it refuses (a validation error, an
// allocation it could not make) is caught by an error scope around it
// and given back as `Left`; so is a kernel whose code does not compile,
// by its compilation info, each message with its line and column.  A
// dispatch's and a read's answer is given when the GPU is done: after
// `onSubmittedWorkDone`, and a read's bytes copied to a buffer that is
// mapped (`mapAsync`) and read out.
export const $host = "page";
export const gpu = () => {
  // (WebGPU's flags, as the specification numbers them, where the page
  // has no `GPUBufferUsage` or `GPUMapMode`)
  const usage = globalThis.GPUBufferUsage ?? { MAP_READ: 1, COPY_SRC: 4, COPY_DST: 8, STORAGE: 128 };
  const mapMode = globalThis.GPUMapMode ?? { READ: 1 };
  const unit = { $: "MkUnit", a: [] };
  const right = (x) => ({ $: "Right", a: [x] });
  const left = (m) => ({ $: "Left", a: [{ message: $fromJs(String(m)) }] });
  const failure = (e) => left(e && e.message || e);
  const buffers = (l) => { const r = []; for (; l.$ === "Cons"; l = l.a[1]) r.push(l.a[0]); return r; };
  // (what `f` does on the device, a promise of its answer: `Left` where
  // it throws, or where WebGPU says it refused it)
  const scoped = (device, f) => {
    device.pushErrorScope("validation");
    device.pushErrorScope("out-of-memory");
    let made;
    try { made = Promise.resolve(f()); } catch (e) { made = Promise.reject(e); }
    const outOfMemory = device.popErrorScope(), validation = device.popErrorScope();
    return Promise.all([made, outOfMemory, validation]).then(
      ([x, o, v]) => o ? left(o.message) : v ? left(v.message) : right(x),
      failure);
  };
  // (a kernel's compilation's errors, a line each, or none)
  const errorsOf = (info) => (info && info.messages || [])
    .filter((m) => m.type === "error")
    .map((m) => `${m.lineNum}:${m.linePos}: ${m.message}`).join("\n");
  const run = (device, encode) => {
    const encoder = device.createCommandEncoder();
    encode(encoder);
    device.queue.submit([encoder.finish()]);
    return device.queue.onSubmittedWorkDone();
  };
  // (what lets a thing go, done: `Left` where it throws)
  const letGo = (f) => { try { f(); return right(unit); } catch (e) { return failure(e); } };
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Open": {
        if (typeof navigator === "undefined" || !navigator.gpu) return k(a[0](left("WebGPU is not here")));
        Promise.resolve(navigator.gpu.requestAdapter())
          .then((adapter) => adapter ? adapter.requestDevice() : null)
          .then((device) => device ? right(device) : left("no GPU adapter"), failure)
          .then((r) => k(a[0](r)));
        return;
      }
      case "Close": return k(a[1](letGo(() => { if (a[0].destroy) a[0].destroy(); })));
      case "NewBuffer":
        scoped(a[0], () => a[0].createBuffer({ size: Number(a[1]), usage: usage.STORAGE | usage.COPY_SRC | usage.COPY_DST }))
          .then((r) => k(a[2](r)));
        return;
      case "FreeBuffer": return k(a[1](letGo(() => a[0].destroy())));
      case "WriteBuffer":
        scoped(a[0], () => { a[0].queue.writeBuffer(a[1], Number(a[2]), a[3]); return unit; })
          .then((r) => k(a[4](r)));
        return;
      case "ReadBuffer": {
        const device = a[0], n = Number(a[3]);
        let staging = null;
        scoped(device, () => {
          staging = device.createBuffer({ size: n, usage: usage.MAP_READ | usage.COPY_DST });
          return run(device, (e) => e.copyBufferToBuffer(a[1], Number(a[2]), staging, 0, n))
            .then(() => staging.mapAsync(mapMode.READ))
            .then(() => new Uint8Array(staging.getMappedRange().slice(0)));
        }).then((r) => {
          if (staging) { try { staging.unmap(); } catch (e) {} staging.destroy(); }
          return r;
        }).then((r) => k(a[4](r)));
        return;
      }
      case "Load": {
        const device = a[0];
        scoped(device, () => {
          const module = device.createShaderModule({ code: $toJs(a[1]) });
          return Promise.resolve(module.getCompilationInfo ? module.getCompilationInfo() : null).then((info) => {
            const errors = errorsOf(info);
            if (errors) throw new Error(errors);
            return device.createComputePipelineAsync({ layout: "auto", compute: { module, entryPoint: $toJs(a[2]) } });
          });
        }).then((r) => k(a[3](r)));
        return;
      }
      case "Dispatch": {
        const device = a[0], pipeline = a[1];
        scoped(device, () => {
          const group = device.createBindGroup({
            layout: pipeline.getBindGroupLayout(0),
            entries: buffers(a[2]).map((buffer, i) => ({ binding: i, resource: { buffer } })),
          });
          return run(device, (e) => {
            const pass = e.beginComputePass();
            pass.setPipeline(pipeline);
            pass.setBindGroup(0, group);
            pass.dispatchWorkgroups(Number(a[3]), Number(a[4]), Number(a[5]));
            pass.end();
          }).then(() => unit);
        }).then((r) => k(a[6](r)));
        return;
      }
      default: throw new Error("heff: not an operation of gpu: " + (p && p.$));
    }
  };
};
