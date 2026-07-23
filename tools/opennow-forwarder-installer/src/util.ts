export function arrayBufferToHex(value: ArrayBuffer): string {
  return Array.from(new Uint8Array(value))
    .map((byte) => byte.toString(16).padStart(2, '0'))
    .join('');
}

export async function deterministicTitleId(
  nroPath: string,
  args: string,
): Promise<string> {
  const encoded = new TextEncoder().encode(nroPath + args);
  const digest = await crypto.subtle.digest('SHA-256', encoded);
  const hash64 = new DataView(digest).getBigUint64(0, true);
  const id = 0x0100000000000000n | (hash64 & 0x00fffffffffff000n);
  return id.toString(16).padStart(16, '0');
}
