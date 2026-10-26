import {
  spawn,
  type ChildProcessByStdio,
  type SpawnOptions,
} from 'node:child_process';
import { create, fromBinary, toBinary } from '@bufbuild/protobuf';
import { performance } from 'node:perf_hooks';
import type { Readable, Writable } from 'node:stream';

import {
  EnvelopeSchema,
  GetCapabilitiesRequestSchema,
  type CapabilityTuple,
  type Envelope,
  type GetCapabilitiesResponse,
} from '../../build/generated/ts/bigshark/engine/engine_pb.js';

// --- ULEB128 frame codec ---------------------------------------------------

export const MAX_FRAME_BYTES = 1 << 20;

const CONTINUATION = 0x80;
const PAYLOAD_MASK = 0x7f;

/** Canonical ULEB128 encoding. Throws above the 1 MiB frame limit. */
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
  /** Capabilities envelope used to validate the spawned process. */
  warmupEnvelope: Envelope;
  warmupTimeoutMs?: number;
}

// ULEB128 frame scanner.
type FrameScan =
  | { status: 'need-data' }
  | {
      status: 'complete';
      headerLength: number;
      frameLength: number;
    }
  | { status: 'malformed' }
  | { status: 'oversize' };

const MAX_VARINT_BYTES = 5;

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
 * exit/hard-restart recovery. The capabilities handshake replaces
 * minor negotiation with a capabilities query that verifies
 * `supported_protocol_versions` includes 3 and stores the advertised
 * CapabilityTuples.
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
  private capabilitiesList: CapabilityTuple[] = [];
  private engineBuildVersion = '';

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

  /** Always 3. */
  get protocolVersion(): 3 {
    return 3;
  }

  /** Capability tuples from the handshake; empty before start() resolves. */
  get capabilities(): readonly CapabilityTuple[] {
    return this.capabilitiesList;
  }

  /** Engine build version from the handshake; empty before start() resolves. */
  get buildVersion(): string {
    return this.engineBuildVersion;
  }

  async request(envelope: Envelope, timeoutMs = 2000): Promise<Envelope> {
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0)
      throw new Error('Engine request timeout must be finite and positive');
    const deadline = performance.now() + timeoutMs;
    let startupTimer: NodeJS.Timeout | undefined;
    try {
      const startupExpiry = new Promise<never>((_resolve, reject) => {
        startupTimer = setTimeout(
          () => reject(new Error('Engine request timed out')),
          timeoutMs,
        );
      });
      await Promise.race([this.start(), startupExpiry]);
    } finally {
      clearTimeout(startupTimer);
    }
    const remainingMs = deadline - performance.now();
    if (remainingMs <= 0) throw new Error('Engine request timed out');
    const response = await this.enqueue(envelope, remainingMs);
    if (performance.now() >= deadline) throw new Error('Engine request timed out');
    return response;
  }

  start(): Promise<void> {
    if (this.process && this.startPromise) return this.startPromise;

    const child = this.spawnProcess(this.command, this.args, {
      stdio: ['pipe', 'pipe', 'ignore'],
    });
    this.process = child;
    this.chunks = [];
    this.capabilitiesList = [];
    this.engineBuildVersion = '';
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
      .then(response => this.handshake(response))
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

  /**
   * Capabilities handshake: verify the host supports protocol version 3
   * and store the advertised capability tuples. A host that does not support
   * protocol version 3 (missing 3 in supported_protocol_versions) causes start() to fail.
   */
  private handshake(response: Envelope): void {
    if (response.payload.case !== 'getCapabilitiesResponse') {
      throw new Error('Engine warmup did not return a capabilities response');
    }
    const caps: GetCapabilitiesResponse = response.payload.value;
    if (!caps.supportedProtocolVersions.includes(3)) {
      throw new Error(
        `Engine does not support protocol version 3 (supported: [${caps.supportedProtocolVersions.join(', ')}])`,
      );
    }
    this.capabilitiesList = caps.capabilities;
    this.engineBuildVersion = caps.engineBuildVersion;
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
      if (this.process !== child) return;
    }
    this.chunks = offset < packed.length ? [packed.subarray(offset)] : [];
  }

  private dispatch(child: EngineChild, payload: Buffer): void {
    let envelope: Envelope;
    try {
      envelope = fromBinary(EnvelopeSchema, payload);
    } catch (error) {
      this.failProcess(
        new Error(`Engine sent an undecodable envelope: ${errorMessage(error)}`),
      );
      return;
    }
    const entry = this.pending.get(envelope.requestId);
    if (!entry) return;
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

/**
 * Creates a warmup envelope (GetCapabilitiesRequest with
 * protocol_version = 3).
 */
export function warmupEnvelope(): Envelope {
  const envelope = create(EnvelopeSchema, { protocolVersion: 3 });
  envelope.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  envelope.requestId = 'warmup-capabilities';
  return envelope;
}
