import {
  spawn,
  type ChildProcessByStdio,
  type SpawnOptions,
} from 'node:child_process';
import { fromBinary, toBinary } from '@bufbuild/protobuf';
import type { Readable, Writable } from 'node:stream';

import {
  EnvelopeSchema,
  type Envelope,
} from '../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

type EngineChild = ChildProcessByStdio<Writable, Readable, null>;
type SpawnProcess = (
  command: string,
  args: readonly string[],
  options: SpawnOptions & { stdio: ['pipe', 'pipe', 'ignore'] },
) => EngineChild;

interface PendingRequest {
  settled: boolean;
  timer: NodeJS.Timeout;
  resolve(value: Envelope): void;
  reject(error: Error): void;
}

export interface ProtoEngineClientOptions {
  command: string;
  args?: readonly string[];
  spawnProcess?: SpawnProcess;
  /** Capabilities or decision envelope used to validate the spawned process. */
  warmupEnvelope: Envelope;
  warmupTimeoutMs?: number;
}

// RFC 0002 transport limit. Declared lengths above this are rejected before
// any payload Buffer is allocated.
export const MAX_FRAME_BYTES = 1 << 20;

const CONTINUATION = 0x80;
const PAYLOAD_MASK = 0x7f;
const MAX_VARINT_BYTES = 5;

/** Canonical ULEB128 encoding. Returns null above the 1 MiB frame limit. */
export function encodeVarint(value: number | bigint): Buffer {
  let remaining = typeof value === 'bigint' ? value : BigInt(value);
  if (remaining < 0n || remaining > BigInt(MAX_FRAME_BYTES)) {
    throw new Error(`Refusing to encode frame length ${remaining}`);
  }
  const bytes: number[] = [];
  do {
    let byte = Number(remaining & 0x7fn);
    remaining >>= 7n;
    if (remaining !== 0n) byte |= CONTINUATION;
    bytes.push(byte);
  } while (remaining !== 0n);
  return Buffer.from(bytes);
}

type FrameScan =
  | { status: 'need-data' }
  | {
      status: 'complete';
      headerLength: number;
      frameLength: number;
    }
  | { status: 'malformed' }
  | { status: 'oversize' };

/** Scans one length-delimited frame without allocating the payload yet. */
function scanFrame(buffer: Buffer, offset: number): FrameScan {
  let length = 0n;
  let prefixBytes = 0;
  for (;;) {
    if (offset + prefixBytes >= buffer.length) return { status: 'need-data' };
    const byte = buffer[offset + prefixBytes] ?? 0;
    if (prefixBytes >= MAX_VARINT_BYTES) return { status: 'malformed' };
    const group = byte & PAYLOAD_MASK;
    length |= BigInt(group) << BigInt(7 * prefixBytes);
    prefixBytes++;
    if (length > BigInt(MAX_FRAME_BYTES)) return { status: 'oversize' };
    if ((byte & CONTINUATION) === 0) {
      // A terminating zero group in a multi-byte prefix is overlong.
      if (prefixBytes > 1 && group === 0) return { status: 'malformed' };
      break;
    }
  }
  const frameLength = Number(length);
  if (frameLength === 0) return { status: 'malformed' };
  if (buffer.length - offset - prefixBytes < frameLength) return { status: 'need-data' };
  return { status: 'complete', headerLength: prefixBytes, frameLength };
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * Length-delimited Protobuf engine client. Responses are correlated by the
 * echoed request_id (a map, never FIFO), all uint64 values stay bigint, and
 * timeout/exit/hard-restart semantics mirror the NDJSON EngineProcessClient.
 */
export class ProtoEngineProcessClient {
  readonly command: string;
  readonly args: readonly string[];
  readonly spawnProcess: SpawnProcess;
  readonly warmupEnvelope: Envelope;
  readonly warmupTimeoutMs: number;

  private process: EngineChild | null = null;
  private pending = new Map<string, PendingRequest>();
  private chunks: Buffer[] = [];
  private startPromise: Promise<void> | null = null;
  private requestCounter = 0;

  constructor({
    command,
    args = ['--serve-proto'],
    spawnProcess = spawn as SpawnProcess,
    warmupEnvelope,
    warmupTimeoutMs = 600,
  }: ProtoEngineClientOptions) {
    if (!command) throw new Error('Engine process command is required');
    this.command = command;
    this.args = args;
    this.spawnProcess = spawnProcess;
    this.warmupEnvelope = warmupEnvelope;
    this.warmupTimeoutMs = warmupTimeoutMs;
  }

  async request(envelope: Envelope, timeoutMs = 2000): Promise<Envelope> {
    await this.start();
    return this.enqueue(envelope, timeoutMs);
  }

  start(): Promise<void> {
    if (this.process && this.startPromise) return this.startPromise;

    const child = this.spawnProcess(this.command, this.args, {
      stdio: ['pipe', 'pipe', 'ignore'],
    });
    this.process = child;
    this.chunks = [];
    child.stdout.on('data', (chunk: Buffer) => this.onData(child, chunk));
    child.on('error', error => this.onExit(child, error));
    child.on('exit', (code, signal) => {
      this.onExit(
        child,
        new Error(
          `Engine process exited${code === null ? '' : ` with code ${code}`}`
            + `${signal ? ` (${signal})` : ''}`,
        ),
      );
    });
    child.unref?.();

    this.startPromise = this.enqueue(this.warmupEnvelope, this.warmupTimeoutMs)
      .then(() => undefined)
      .catch((error: unknown) => {
        this.stop();
        throw new Error(`Engine warmup failed: ${errorMessage(error)}`);
      });
    return this.startPromise;
  }

  stop(): void {
    this.failProcess(new Error('Engine client stopped'));
  }

  private assignRequestId(envelope: Envelope): string {
    if (!envelope.requestId) {
      this.requestCounter += 1;
      envelope.requestId = `req-${process.pid}-${this.requestCounter}`;
    }
    return envelope.requestId;
  }

  private enqueue(envelope: Envelope, timeoutMs: number): Promise<Envelope> {
    const child = this.process;
    if (!child?.stdin.writable) {
      return Promise.reject(new Error('Engine process is unavailable'));
    }
    const requestId = this.assignRequestId(envelope);
    if (this.pending.has(requestId)) {
      return Promise.reject(new Error(`Duplicate engine request id ${requestId}`));
    }

    let frame: Buffer;
    try {
      const payload = Buffer.from(toBinary(EnvelopeSchema, envelope));
      frame = Buffer.concat([encodeVarint(payload.length), payload]);
    } catch (error) {
      return Promise.reject(error instanceof Error ? error : new Error(String(error)));
    }

    return new Promise<Envelope>((resolve, reject) => {
      const entry = {} as PendingRequest;
      entry.settled = false;
      entry.resolve = (value: Envelope) => {
        if (entry.settled) return;
        entry.settled = true;
        clearTimeout(entry.timer);
        this.pending.delete(requestId);
        resolve(value);
      };
      entry.reject = (error: Error) => {
        if (entry.settled) return;
        entry.settled = true;
        clearTimeout(entry.timer);
        this.pending.delete(requestId);
        reject(error);
      };
      entry.timer = setTimeout(
        () => this.failProcess(new Error('Engine request timed out')),
        timeoutMs,
      );
      this.pending.set(requestId, entry);
      child.stdin.write(frame, error => {
        if (error && this.process === child) this.failProcess(error);
      });
    });
  }

  private onData(child: EngineChild, chunk: Buffer): void {
    if (this.process !== child) return;
    this.chunks.push(chunk);
    this.pump(child);
  }

  private pump(child: EngineChild): void {
    // Concatenate once per arrival; completed frames are sliced without
    // copying, and only the unconsumed tail is retained.
    const packed = Buffer.concat(this.chunks);
    let offset = 0;
    for (;;) {
      const scan = scanFrame(packed, offset);
      if (scan.status === 'need-data') break;
      if (scan.status === 'oversize') {
        this.failProcess(new Error('Engine frame exceeds the 1 MiB limit'));
        return;
      }
      if (scan.status === 'malformed') {
        this.failProcess(new Error('Engine sent a malformed frame'));
        return;
      }
      const start = offset + scan.headerLength;
      const payload = packed.subarray(start, start + scan.frameLength);
      offset = start + scan.frameLength;
      this.dispatch(child, payload);
      if (this.process !== child) return;  // dispatch killed the process
    }
    this.chunks = offset < packed.length ? [packed.subarray(offset)] : [];
  }

  private dispatch(child: EngineChild, payload: Buffer): void {
    let envelope: Envelope;
    try {
      envelope = fromBinary(EnvelopeSchema, payload, {
        // Wire-level parser stays strict; ignoreUnknownFields defaults false.
      });
    } catch (error) {
      this.failProcess(
        new Error(`Engine sent an undecodable envelope: ${errorMessage(error)}`),
      );
      return;
    }
    const entry = this.pending.get(envelope.requestId);
    if (!entry) {
      // Uncorrelated responses (unknown or reused ids) are dropped rather
      // than shifting later legitimate responses.
      return;
    }
    if (this.process !== child) return;
    entry.resolve(envelope);
  }

  private onExit(child: EngineChild, error: Error): void {
    if (this.process !== child) return;
    this.process = null;
    this.startPromise = null;
    this.chunks = [];
    this.rejectAll(error);
  }

  private failProcess(error: Error): void {
    const child = this.process;
    this.process = null;
    this.startPromise = null;
    this.chunks = [];
    this.rejectAll(error);
    if (child && !child.killed) child.kill();
  }

  private rejectAll(error: Error): void {
    const pending = [...this.pending.values()];
    this.pending.clear();
    for (const entry of pending) entry.reject(error);
  }
}
