// the interpreter's `list` layout, flattened
export const flat = (s) => s.replace(/\s*\n\s*/g, " ").replace(/\[ /g, "[").replace(/ \]/g, "]").replace(/ ,/g, ",");
