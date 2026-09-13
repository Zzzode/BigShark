import {
  spawn,
  type ChildProcessByStdio,
  type SpawnOptions,
} from 'node:child_process';
import type { Readable, Writable } from 'node:stream';

type EngineChild = ChildProcessByStdio<Writable, Readable, null>;
type SpawnProcess = (
  command: string,
  args: readonly string[],
  options: SpawnOptions & { stdio: ['pipe', 'pipe', 'ignore'] },
) => EngineChild;

interface PendingRequest<Response> {
  settled: boolean;
  timer: NodeJS.Timeout;
  resolve(value: Response): void;
  reject(error: Error): void;
}

export interface EngineProcessClientOptions<Request> {
  command: string;
  args?: readonly string[];
  spawnProcess?: SpawnProcess;
  warmupRequest?: Request;
  warmupTimeoutMs?: number;
}

export class EngineProcessClient<Request, Response> {
  readonly command: string;
  readonly args: readonly string[];
  readonly spawnProcess: SpawnProcess;
  readonly warmupRequest: Request;
  readonly warmupTimeoutMs: number;

  private process: EngineChild | null = null;
  private queue: Array<PendingRequest<Response>> = [];
  private buffer = '';
  private startPromise: Promise<void> | null = null;

  constructor({
    command,
    args = ['--serve'],
    spawnProcess = spawn as SpawnProcess,
    warmupRequest,
    warmupTimeoutMs = 600,
  }: EngineProcessClientOptions<Request>) {
    if (!command) throw new Error('Engine process command is required');
    if (warmupRequest === undefined) {
      throw new Error('Engine warmup request is required');
    }
    this.command = command;
    this.args = args;
    this.spawnProcess = spawnProcess;
    this.warmupRequest = warmupRequest;
    this.warmupTimeoutMs = warmupTimeoutMs;
  }

  async request(message: Request, timeoutMs = 2000): Promise<Response> {
    await this.start();
    return this.enqueue(message, timeoutMs);
  }

  start(): Promise<void> {
    if (this.process && this.startPromise) return this.startPromise;

    const child = this.spawnProcess(this.command, this.args, {
      stdio: ['pipe', 'pipe', 'ignore'],
    });
    this.process = child;
    this.buffer = '';
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk: string) => this.onData(child, chunk));
    child.on('error', error => this.onExit(child, error));
    child.on('exit', (code, signal) => {
      this.onExit(child, new Error(
        `Engine process exited${code === null ? '' : ` with code ${code}`}`
        + `${signal ? ` (${signal})` : ''}`,
      ));
    });
    child.unref?.();

    this.startPromise = this.enqueue(this.warmupRequest, this.warmupTimeoutMs)
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

  private enqueue(message: Request, timeoutMs: number): Promise<Response> {
    const child = this.process;
    if (!child?.stdin.writable) {
      return Promise.reject(new Error('Engine process is unavailable'));
    }
    const serialized = typeof message === 'string'
      ? message
      : JSON.stringify(message);

    return new Promise<Response>((resolve, reject) => {
      const entry = {} as PendingRequest<Response>;
      entry.settled = false;
      entry.resolve = (value: Response) => {
        if (entry.settled) return;
        entry.settled = true;
        clearTimeout(entry.timer);
        resolve(value);
      };
      entry.reject = (error: Error) => {
        if (entry.settled) return;
        entry.settled = true;
        clearTimeout(entry.timer);
        reject(error);
      };
      entry.timer = setTimeout(
        () => this.failProcess(new Error('Engine request timed out')),
        timeoutMs,
      );
      this.queue.push(entry);
      child.stdin.write(`${serialized}\n`, error => {
        if (error && this.process === child) this.failProcess(error);
      });
    });
  }

  private onData(child: EngineChild, chunk: string): void {
    if (this.process !== child) return;
    this.buffer += chunk;
    let newline: number;
    while ((newline = this.buffer.indexOf('\n')) !== -1) {
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      const entry = this.queue.shift();
      if (!entry) continue;
      if (!line) {
        entry.reject(new Error('Engine returned an empty response'));
        continue;
      }
      try {
        entry.resolve(JSON.parse(line) as Response);
      } catch (error) {
        entry.reject(new Error(`Engine returned invalid JSON: ${errorMessage(error)}`));
      }
    }
  }

  private onExit(child: EngineChild, error: Error): void {
    if (this.process !== child) return;
    this.process = null;
    this.startPromise = null;
    this.buffer = '';
    this.rejectAll(error);
  }

  private failProcess(error: Error): void {
    const child = this.process;
    this.process = null;
    this.startPromise = null;
    this.buffer = '';
    this.rejectAll(error);
    if (child && !child.killed) child.kill();
  }

  private rejectAll(error: Error): void {
    const pending = this.queue;
    this.queue = [];
    for (const entry of pending) entry.reject(error);
  }
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
