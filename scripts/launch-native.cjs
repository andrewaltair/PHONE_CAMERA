'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');

function resolveNativeLaunch({ platform = process.platform, programFiles = process.env.ProgramW6432 || process.env.ProgramFiles,
  exists = fs.existsSync } = {}) {
  if (platform !== 'win32') {
    return { executable: null, error: 'The native Windows launcher requires Windows. See the Linux preview build instructions in README.md.' };
  }
  if (!programFiles) {
    return { executable: null, error: 'Windows Program Files was not found. Install PHONE_CAMERA first; see README.md for setup.' };
  }
  const executable = path.win32.join(programFiles, 'PHONE_CAMERA', 'PHONE_CAMERA.exe');
  if (!exists(executable)) {
    return { executable: null, error: 'PHONE_CAMERA is not installed yet. Build and run its Windows setup as described in README.md.' };
  }
  return { executable, args: ['--settings'] };
}

if (require.main === module) {
  const result = resolveNativeLaunch();
  if (!result.executable) {
    console.error(result.error);
    process.exitCode = 1;
  } else {
    const child = spawn(result.executable, result.args, { detached: true, stdio: 'ignore', windowsHide: false });
    child.once('error', error => { console.error('Could not start PHONE_CAMERA: ' + error.message); process.exitCode = 1; });
    child.once('spawn', () => child.unref());
  }
}

module.exports = { resolveNativeLaunch };
