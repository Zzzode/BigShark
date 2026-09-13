#!/usr/bin/env node

import { createHash } from 'node:crypto';
import {
  chmodSync,
  createWriteStream,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
} from 'node:fs';
import { get } from 'node:https';
import { join, resolve } from 'node:path';
import { pipeline } from 'node:stream/promises';
import { spawnSync } from 'node:child_process';

const PROTOBUF_VERSION = '36.1';
const root = resolve(process.env.BIGSHARK_PROJECT_ROOT ?? process.cwd());
const toolsDirectory = join(root, '.tools');
const installDirectory = join(toolsDirectory, 'protobuf', PROTOBUF_VERSION);
const protoc = join(installDirectory, 'bin', 'protoc');
const DOWNLOAD_TIMEOUT_MS = 120_000;
const IO_IDLE_TIMEOUT_MS = 30_000;
const PROCESS_TIMEOUT_MS = 60_000;

interface ProtocArchive {
  name: string;
  sha256: string;
}

const archives: Record<string, ProtocArchive> = {
  'darwin-arm64': {
    name: 'protoc-36.1-osx-aarch_64.zip',
    sha256: 'de56d57afe30c5d191b11d24ff93dd4025728d7fb43b773886b2d3613e0bdbb2',
  },
  'darwin-x64': {
    name: 'protoc-36.1-osx-x86_64.zip',
    sha256: 'ee2c5496e4af0aa6a224894bc0f7025145260e004d890487d510725ce8b473eb',
  },
  'linux-arm64': {
    name: 'protoc-36.1-linux-aarch_64.zip',
    sha256: '237a68856edf1bd28b6204bddd0596c1cf46d298bc29c620012540b2e44c73e7',
  },
  'linux-x64': {
    name: 'protoc-36.1-linux-x86_64.zip',
    sha256: 'c4bc672d9d49214dc8cafdceadf4df92182d6ca8e3ec65a56b2d7de5602669b4',
  },
};

function checkProtoc(path: string): boolean {
  if (!existsSync(path)) return false;
  const result = spawnSync(path, ['--version'], {
    encoding: 'utf8',
    timeout: PROCESS_TIMEOUT_MS,
  });
  return result.status === 0
    && result.stdout.trim() === `libprotoc ${PROTOBUF_VERSION}`;
}

function checkInstalled(): boolean {
  return checkProtoc(protoc);
}

function removeTemporaryEntries(directory: string): void {
  if (!existsSync(directory)) return;
  for (const entry of readdirSync(directory)) {
    if (entry.endsWith('.tmp')) {
      rmSync(join(directory, entry), { force: true, recursive: true });
    }
  }
}

async function download(url: string, destination: string, redirects = 5): Promise<void> {
  if (redirects < 0) throw new Error(`Too many redirects while downloading ${url}`);
  await new Promise<void>((resolveDownload, rejectDownload) => {
    let totalTimeout: NodeJS.Timeout | undefined;
    const succeed = (): void => {
      if (totalTimeout) clearTimeout(totalTimeout);
      resolveDownload();
    };
    const fail = (error: Error): void => {
      if (totalTimeout) clearTimeout(totalTimeout);
      rejectDownload(error);
    };
    const request = get(url, response => {
      const location = response.headers.location;
      if (response.statusCode
        && response.statusCode >= 300
        && response.statusCode < 400
        && location) {
        response.resume();
        download(new URL(location, url).href, destination, redirects - 1)
          .then(succeed, fail);
        return;
      }
      if (response.statusCode !== 200) {
        response.resume();
        fail(new Error(
          `Download failed with HTTP ${response.statusCode ?? 'unknown'}: ${url}`,
        ));
        return;
      }
      pipeline(response, createWriteStream(destination))
        .then(succeed, fail);
    });
    request.setTimeout(IO_IDLE_TIMEOUT_MS, () => {
      request.destroy(new Error(`Download stalled while fetching ${url}`));
    });
    request.on('error', fail);
    totalTimeout = setTimeout(() => {
      request.destroy(new Error(`Download timed out while fetching ${url}`));
    }, DOWNLOAD_TIMEOUT_MS);
  });
}

function sha256(path: string): string {
  return createHash('sha256').update(readFileSync(path)).digest('hex');
}

if (!checkInstalled()) {
  const platform = `${process.platform}-${process.arch}`;
  const archive = archives[platform];
  if (!archive) throw new Error(`Unsupported protoc platform: ${platform}`);

  const downloads = join(toolsDirectory, 'downloads');
  const archivePath = join(downloads, archive.name);
  const archiveTemporaryPath = `${archivePath}.tmp`;
  const installTemporaryDirectory = `${installDirectory}.tmp`;
  try {
    mkdirSync(downloads, { recursive: true });
    removeTemporaryEntries(downloads);
    removeTemporaryEntries(join(toolsDirectory, 'protobuf'));
    await download(
      `https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/${archive.name}`,
      archiveTemporaryPath,
    );
    const digest = sha256(archiveTemporaryPath);
    if (digest !== archive.sha256) {
      throw new Error(`Checksum mismatch for ${archive.name}: ${digest}`);
    }
    renameSync(archiveTemporaryPath, archivePath);

    mkdirSync(installTemporaryDirectory, { recursive: true });
    const temporaryProtoc = join(installTemporaryDirectory, 'bin', 'protoc');
    const unzip = spawnSync(
      'unzip',
      ['-q', '-o', archivePath, '-d', installTemporaryDirectory],
      { encoding: 'utf8', timeout: PROCESS_TIMEOUT_MS },
    );
    if (unzip.status !== 0) {
      throw new Error(unzip.stderr || `Failed to extract ${archive.name}`);
    }
    chmodSync(temporaryProtoc, 0o755);
    if (!checkProtoc(temporaryProtoc)) {
      throw new Error(`Downloaded protoc ${PROTOBUF_VERSION} failed validation`);
    }

    rmSync(installDirectory, { force: true, recursive: true });
    renameSync(installTemporaryDirectory, installDirectory);
  } finally {
    rmSync(archiveTemporaryPath, { force: true });
    rmSync(installTemporaryDirectory, { force: true, recursive: true });
  }
}

if (!checkInstalled()) {
  throw new Error(`protoc ${PROTOBUF_VERSION} installation failed`);
}

process.stdout.write(`${protoc}\n`);
