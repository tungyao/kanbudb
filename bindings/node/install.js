'use strict';

const https = require('https');
const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');
const os = require('os');

const pkg = require('./package.json');
const version = pkg.version;
const name = pkg.name;

function platformTag() {
  const m = { linux: 'linux', darwin: 'macos', win32: 'windows' }[os.platform()];
  const a = os.arch() === 'x64' ? 'x86_64' : os.arch();
  return `${m}-${a}`;
}

function libName() {
  if (os.platform() === 'win32') return 'kanbudb_shared.dll';
  if (os.platform() === 'darwin') return 'libkanbudb_shared.dylib';
  return 'libkanbudb_shared.so';
}

const libDir = path.join(__dirname, 'prebuilds', platformTag());
const libFile = path.join(libDir, libName());

if (fs.existsSync(libFile)) {
  process.exit(0);
}

const url = `https://github.com/kanbudb/${name}/releases/download/v${version}/prebuild-${platformTag()}.tar.gz`;

console.log(`Downloading prebuilt binary: ${url}`);

// Try building from source if download fails
function buildFromSource() {
  console.log('Building from source...');
  const root = path.join(__dirname, '..', '..');
  try {
    execSync('cmake --version', { stdio: 'ignore' });
    execSync(`cmake -B ${path.join(root, 'build')} -DKANBUDB_BUILD_TESTS=OFF`, { cwd: root, stdio: 'inherit' });
    execSync(`cmake --build ${path.join(root, 'build')}`, { cwd: root, stdio: 'inherit' });
    fs.mkdirSync(libDir, { recursive: true });
    fs.copyFileSync(
      path.join(root, 'build', libName()),
      libFile
    );
    console.log('Build complete.');
  } catch (e) {
    console.error('Failed to build from source:', e.message);
    process.exit(1);
  }
}

https.get(url, (res) => {
  if (res.statusCode === 200) {
    fs.mkdirSync(libDir, { recursive: true });
    const tmp = path.join(libDir, 'prebuild.tar.gz');
    const f = fs.createWriteStream(tmp);
    res.pipe(f);
    f.on('finish', () => {
      execSync(`tar -xzf "${tmp}" -C "${libDir}"`, { stdio: 'ignore' });
      fs.unlinkSync(tmp);
      console.log('Prebuilt binary downloaded.');
    });
  } else {
    buildFromSource();
  }
}).on('error', buildFromSource);
