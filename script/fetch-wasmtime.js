#!/usr/bin/env node

// Downloads the prebuilt Wasmtime C API that native Tree-sitter uses to run
// WASM grammars, for the platform being built, into
// vendor/wasmtime/<target>/{include,lib}. Prints the target directory.
//
// The version must match the one Tree-sitter's wasm store was written for
// (see vendor/tree-sitter/README.md).

const fs = require('fs')
const https = require('https')
const path = require('path')
const crypto = require('crypto')
const {execFileSync} = require('child_process')

const VERSION = '29.0.1'

// sha256 of each release archive, checked after download.
const CHECKSUMS = {
  'x86_64-linux': '27871520e02193badd5a83e16a5009c45371ec5ec9e1df8342b4a17efc3e35b6',
  'aarch64-linux': null,
  'x86_64-macos': null,
  'aarch64-macos': null,
  'x86_64-windows': null
}

function target () {
  const arch = process.env.npm_config_arch || process.arch
  const wasmtimeArch = {x64: 'x86_64', arm64: 'aarch64'}[arch]
  const os = {linux: 'linux', darwin: 'macos', win32: 'windows'}[process.platform]
  if (!wasmtimeArch || !os) throw new Error(`Unsupported platform for wasmtime: ${process.platform}-${arch}`)
  return `${wasmtimeArch}-${os}`
}

function download (url, destination) {
  return new Promise((resolve, reject) => {
    https.get(url, {headers: {'User-Agent': 'superstring-build'}}, (response) => {
      if (response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
        response.resume()
        download(response.headers.location, destination).then(resolve, reject)
        return
      }
      if (response.statusCode !== 200) {
        response.resume()
        reject(new Error(`Downloading ${url} failed with status ${response.statusCode}`))
        return
      }
      const file = fs.createWriteStream(destination)
      response.pipe(file)
      file.on('finish', () => file.close(resolve))
      file.on('error', reject)
    }).on('error', reject)
  })
}

async function main () {
  const name = target()
  const root = path.join(__dirname, '..', 'vendor', 'wasmtime')
  const directory = path.join(root, name)
  if (fs.existsSync(path.join(directory, 'include', 'wasmtime.h'))) {
    process.stdout.write(directory)
    return
  }

  const extension = name.endsWith('windows') ? 'zip' : 'tar.xz'
  const archiveName = `wasmtime-v${VERSION}-${name}-c-api`
  const url = `https://github.com/bytecodealliance/wasmtime/releases/download/v${VERSION}/${archiveName}.${extension}`
  fs.mkdirSync(root, {recursive: true})
  const archive = path.join(root, `${archiveName}.${extension}`)
  await download(url, archive)

  const hash = crypto.createHash('sha256').update(fs.readFileSync(archive)).digest('hex')
  const expected = CHECKSUMS[name]
  if (expected && expected !== hash) {
    fs.unlinkSync(archive)
    throw new Error(`Checksum mismatch for ${archiveName}: expected ${expected}, got ${hash}`)
  }

  // bsdtar (Windows 10+, macOS) and GNU tar both handle these archives.
  execFileSync('tar', ['-xf', archive, '-C', root])
  fs.renameSync(path.join(root, archiveName), directory)
  fs.unlinkSync(archive)
  process.stdout.write(directory)
}

main().catch((error) => {
  console.error(error.message)
  process.exit(1)
})
