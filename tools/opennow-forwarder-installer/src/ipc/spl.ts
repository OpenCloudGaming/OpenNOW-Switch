const splMig = new Switch.Service('spl:mig');

export function generateAesKek(
  wrappedKek: ArrayBuffer,
  keyGeneration: number,
  option: number,
): ArrayBuffer {
  const input = new Uint8Array(0x18);
  input.set(new Uint8Array(wrappedKek, 0, 0x10), 0);
  const options = new Uint32Array(input.buffer, 0x10, 2);
  options[0] = keyGeneration;
  options[1] = option;
  const output = new Uint8Array(0x10);
  splMig.dispatchInOut(2, input.buffer, output.buffer);
  return output.buffer;
}

export function generateAesKey(
  sealedKek: ArrayBuffer,
  wrappedKey: ArrayBuffer,
): ArrayBuffer {
  const input = new Uint8Array(0x20);
  input.set(new Uint8Array(sealedKek, 0, 0x10), 0);
  input.set(new Uint8Array(wrappedKey, 0, 0x10), 0x10);
  const output = new Uint8Array(0x10);
  splMig.dispatchInOut(4, input.buffer, output.buffer);
  return output.buffer;
}
