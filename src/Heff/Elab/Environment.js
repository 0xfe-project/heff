// A side channel for the elaboration trace, for debugging: when enabled,
// every trace event is also kept here, so that the events leading up to an
// internal error survive it.
let sink = null;
export const enableSinkImpl = () => { sink = []; };
export const sinkPushImpl = (ev) => (x) => { if (sink) sink.push(ev); return x; };
export const sinkTakeImpl = () => { const s = sink || []; sink = []; return s; };

// run a pure computation, turning an exception (an internal error) into a value
export const tryPureImpl = (ok) => (err) => (thunk) => {
  try { return ok(thunk()); } catch (e) { return err(String(e && e.message || e)); }
};
