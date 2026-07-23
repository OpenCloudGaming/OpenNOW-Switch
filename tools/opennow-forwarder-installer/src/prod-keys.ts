import { generateAesKek, generateAesKey } from './ipc/spl';
import { arrayBufferToHex } from './util';

export function loadForwarderKeys(): Uint8Array {
  const existing = Switch.readFileSync('sdmc:/switch/prod.keys');
  if (existing) return new Uint8Array(existing);

  const headerKekSource = new Uint8Array([
    0x1f, 0x12, 0x91, 0x3a, 0x4a, 0xcb, 0xf0, 0x0d,
    0x4c, 0xde, 0x3a, 0xf6, 0xd5, 0x23, 0x88, 0x2a,
  ]);
  const headerKeySource = new Uint8Array([
    0x5a, 0x3e, 0xd8, 0x4f, 0xde, 0xc0, 0xd8, 0x26,
    0x31, 0xf7, 0xe2, 0x5d, 0x19, 0x7b, 0xf5, 0xd0,
    0x1c, 0x9b, 0x7b, 0xfa, 0xf6, 0x28, 0x18, 0x3d,
    0x71, 0xf6, 0x4d, 0x73, 0xf1, 0x50, 0xb9, 0xd2,
  ]);
  const kek = generateAesKek(headerKekSource.buffer, 0, 0);
  const key0 = generateAesKey(kek, headerKeySource.buffer.slice(0, 0x10));
  const key1 = generateAesKey(kek, headerKeySource.buffer.slice(0x10));
  const keyHex = arrayBufferToHex(
    new Uint8Array([
      ...new Uint8Array(key0),
      ...new Uint8Array(key1),
    ]).buffer,
  );
  return new TextEncoder().encode(
    `header_key = ${keyHex}\n` +
      'key_area_key_application_00 = 00000000000000000000000000000001\n',
  );
}
