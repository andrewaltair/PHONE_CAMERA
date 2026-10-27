'use strict';

const { test } = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { resolveNativeLaunch } = require('../scripts/launch-native.cjs');

test('the default entry opens the installed PHONE_CAMERA application', () => {
  const base = 'C:\\Program Files';
  const expected = path.win32.join(base, 'PHONE_CAMERA', 'PHONE_CAMERA.exe');
  const result = resolveNativeLaunch({ platform: 'win32', programFiles: base, exists: file => file === expected });
  assert.equal(result.executable, expected);
  assert.deepEqual(result.args, ['--settings']);
});

test('an absent installation produces setup guidance instead of launching OBS or the legacy server', () => {
  const result = resolveNativeLaunch({ platform: 'win32', programFiles: 'C:\\Program Files', exists: () => false });
  assert.equal(result.executable, null);
  assert.match(result.error, /install|setup/i);
});

test('unsupported desktop platforms are not silently sent to the legacy WebRTC path', () => {
  const result = resolveNativeLaunch({ platform: 'linux', programFiles: '', exists: () => false });
  assert.equal(result.executable, null);
  assert.match(result.error, /linux/i);
});
