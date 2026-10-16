// The page's side of browser.gpu.canvas: the `<canvas>` browser.dom's provider
// handed out, WebGPU's device and buffer browser.gpu's did, each the host's own
// object, given as they are.  The canvas is sized to the picture and
// configured for the device, `rgba8unorm`, and the buffer is copied
// into its current texture, a row of `w * 4` bytes each (WebGPU wants
// rows of 256 bytes: `w` a multiple of 64).  What WebGPU refuses is
// caught by an error scope and given back as `Left`.
export const $host = "page";
export const canvas = () => {
  const textureUsage = globalThis.GPUTextureUsage ?? { COPY_DST: 2, RENDER_ATTACHMENT: 16 };
  const unit = { $: "MkUnit", a: [] };
  const right = (x) => ({ $: "Right", a: [x] });
  const left = (m) => ({ $: "Left", a: [{ message: $fromJs(String(m)) }] });
  const failure = (e) => left(e && e.message || e);
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
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Present": {
        const device = a[0], element = a[1], buffer = a[2], w = Number(a[3]), h = Number(a[4]);
        scoped(device, () => {
          const context = element.getContext ? element.getContext("webgpu") : null;
          if (!context) throw new Error("the element is not a <canvas> with WebGPU");
          element.width = w;
          element.height = h;
          context.configure({ device, format: "rgba8unorm", usage: textureUsage.COPY_DST | textureUsage.RENDER_ATTACHMENT });
          const encoder = device.createCommandEncoder();
          encoder.copyBufferToTexture({ buffer, bytesPerRow: w * 4 }, { texture: context.getCurrentTexture() }, [w, h]);
          device.queue.submit([encoder.finish()]);
          return device.queue.onSubmittedWorkDone().then(() => unit);
        }).then((r) => k(a[5](r)));
        return;
      }
      default: throw new Error("heff: not an operation of canvas: " + (p && p.$));
    }
  };
};
