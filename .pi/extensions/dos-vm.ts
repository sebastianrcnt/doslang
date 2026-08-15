import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type } from "typebox";
import net from "node:net";

const HOST = "127.0.0.1";
const PORT = 5555;
const MONITOR_PORT = 4444;
const CHUNK_SIZE = 4096;
const MAX_FILE_SIZE = 4 * 1024 * 1024;

function hexEncode(value: string | Buffer): string {
  return Buffer.from(value).toString("hex").toUpperCase();
}

function hexDecode(value: string): Buffer {
  return Buffer.from(value, "hex");
}

function normalizePath(path: string): string {
  const result = path.startsWith("@") ? path.slice(1) : path;
  if (!/^[A-Za-z]:[\\/]/.test(result)) {
    throw new Error(`DOS path must be absolute (for example C:\\SRC\\FILE.C): ${result}`);
  }
  return result.replaceAll("/", "\\");
}

async function request(command: string, signal?: AbortSignal, timeoutMs = 30_000): Promise<string> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: HOST, port: PORT });
    let buffered = "";
    let settled = false;

    const finish = (error?: Error, value?: string) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      socket.destroy();
      if (error) reject(error); else resolve(value ?? "");
    };
    const abort = () => finish(new Error("DOS tool call aborted"));
    const timer = setTimeout(() => finish(new Error(`DOS agent timed out after ${timeoutMs}ms`)), timeoutMs);

    signal?.addEventListener("abort", abort, { once: true });
    socket.setEncoding("ascii");
    socket.on("connect", () => socket.write(`${command}\r\n`, "ascii"));
    socket.on("data", (chunk) => {
      buffered += chunk;
      const lines = buffered.split(/\r?\n/);
      buffered = lines.pop() ?? "";
      for (const line of lines) {
        if (line.startsWith("OK ")) return finish(undefined, line.slice(3));
        if (line === "OK") return finish(undefined, "");
        if (line.startsWith("ERR ")) return finish(new Error(hexDecode(line.slice(4)).toString("utf8")));
      }
    });
    socket.on("error", (error) => finish(new Error(`Cannot connect to DOS agent at ${HOST}:${PORT}: ${error.message}`)));
    socket.on("close", () => {
      if (!settled) finish(new Error("DOS serial connection closed before a response"));
    });
  });
}

async function interruptForegroundCommand(): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    const socket = net.createConnection({ host: HOST, port: MONITOR_PORT });
    const timer = setTimeout(() => { socket.destroy(); reject(new Error("QEMU monitor did not accept Ctrl+C")); }, 5_000);
    socket.on("connect", () => socket.write("sendkey ctrl-c\r\n", "ascii"));
    socket.on("data", () => { clearTimeout(timer); socket.destroy(); resolve(); });
    socket.on("error", (error) => { clearTimeout(timer); reject(new Error(`Cannot contact QEMU monitor: ${error.message}`)); });
  });
}

async function waitForAgentRecovery(timeoutMs = 10_000): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      if (await request("PING", undefined, 1_000) === "504F4E47") return true;
    } catch { /* Command is still unwinding. */ }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  return false;
}

async function readFile(path: string, signal?: AbortSignal): Promise<Buffer> {
  const dosPath = normalizePath(path);
  const chunks: Buffer[] = [];
  let size = 0;
  let offset = 0;
  for (;;) {
    const response = await request(`READ ${hexEncode(dosPath)} ${offset}`, signal);
    const match = /^([01])(?:\s(.*))?$/.exec(response);
    if (!match) throw new Error(`Malformed READ response: ${response}`);
    const chunk = hexDecode(match[2] ?? "");
    chunks.push(chunk);
    size += chunk.length;
    if (size > MAX_FILE_SIZE) throw new Error(`DOS file exceeds ${MAX_FILE_SIZE} byte tool limit`);
    offset += chunk.length;
    if (match[1] === "1") return Buffer.concat(chunks, size);
    if (chunk.length === 0) throw new Error("DOS agent returned an empty non-final chunk");
  }
}

async function writeFile(path: string, content: Buffer, signal?: AbortSignal): Promise<void> {
  const dosPath = normalizePath(path);
  if (content.length > MAX_FILE_SIZE) throw new Error(`Content exceeds ${MAX_FILE_SIZE} byte tool limit`);
  if (content.length === 0) {
    await request(`WRITE ${hexEncode(dosPath)} T `, signal);
    return;
  }
  for (let offset = 0; offset < content.length; offset += CHUNK_SIZE) {
    const chunk = content.subarray(offset, offset + CHUNK_SIZE);
    await request(`WRITE ${hexEncode(dosPath)} ${offset === 0 ? "T" : "A"} ${hexEncode(chunk)}`, signal);
  }
}

function textResult(text: string) {
  return { content: [{ type: "text" as const, text }], details: {} };
}

export default function (pi: ExtensionAPI) {
  pi.registerTool({
    name: "dos_read",
    label: "DOS Read",
    description: "Read a text file from the running FreeDOS VM over COM1.",
    promptSnippet: "Read files inside the running FreeDOS VM",
    parameters: Type.Object({ path: Type.String({ description: "Absolute DOS path such as C:\\SRC\\MAIN.C" }) }),
    async execute(_id, params, signal) {
      const content = await readFile(params.path, signal);
      return textResult(content.toString("utf8"));
    },
  });

  pi.registerTool({
    name: "dos_write",
    label: "DOS Write",
    description: "Create or replace a text file in the running FreeDOS VM over COM1.",
    promptSnippet: "Write files inside the running FreeDOS VM",
    parameters: Type.Object({
      path: Type.String({ description: "Absolute DOS path" }),
      content: Type.String({ description: "Complete UTF-8 text content" }),
    }),
    async execute(_id, params, signal) {
      const content = Buffer.from(params.content, "utf8");
      await writeFile(params.path, content, signal);
      return textResult(`Wrote ${content.length} bytes to ${normalizePath(params.path)}`);
    },
  });

  pi.registerTool({
    name: "dos_edit",
    label: "DOS Edit",
    description: "Replace one uniquely matching text fragment in a file in the running FreeDOS VM.",
    promptSnippet: "Make exact text replacements inside FreeDOS files",
    parameters: Type.Object({
      path: Type.String({ description: "Absolute DOS path" }),
      old_text: Type.String({ description: "Text that must occur exactly once" }),
      new_text: Type.String({ description: "Replacement text" }),
    }),
    async execute(_id, params, signal) {
      const original = (await readFile(params.path, signal)).toString("utf8");
      const occurrences = original.split(params.old_text).length - 1;
      if (occurrences !== 1) throw new Error(`Expected old_text exactly once, found ${occurrences} occurrences`);
      const updated = original.replace(params.old_text, params.new_text);
      await writeFile(params.path, Buffer.from(updated, "utf8"), signal);
      return textResult(`Edited ${normalizePath(params.path)}`);
    },
  });

  pi.registerTool({
    name: "dos_list",
    label: "DOS List",
    description: "List a directory in the running FreeDOS VM over COM1.",
    promptSnippet: "List directories inside the running FreeDOS VM",
    parameters: Type.Object({ path: Type.String({ description: "Absolute DOS directory path" }) }),
    async execute(_id, params, signal) {
      const response = await request(`LIST ${hexEncode(normalizePath(params.path))}`, signal);
      return textResult(hexDecode(response).toString("utf8"));
    },
  });

  pi.registerTool({
    name: "dos_exec",
    label: "DOS Exec",
    description: "Execute a command in the running FreeDOS VM and return its output.",
    promptSnippet: "Run commands inside the running FreeDOS VM",
    parameters: Type.Object({
      command: Type.String({ description: "FreeDOS command line" }),
      timeout_seconds: Type.Optional(Type.Integer({ minimum: 1, maximum: 120, description: "Abort after this many seconds (default: 30)" })),
    }),
    async execute(_id, params, signal) {
      if (params.command.length > 600) throw new Error("DOS command exceeds 600 characters");
      const timeoutMs = (params.timeout_seconds ?? 30) * 1_000;
      let response: string;
      try {
        response = await request(`EXEC ${hexEncode(params.command)}`, signal, timeoutMs);
      } catch (error) {
        if (signal?.aborted) throw error;
        await interruptForegroundCommand();
        const recovered = await waitForAgentRecovery();
        throw new Error(`DOS command timed out after ${timeoutMs / 1_000}s and was interrupted with Ctrl+C${recovered ? "; agent recovered" : "; agent did not recover"}`);
      }
      const match = /^(-?\d+)(?:\s(.*))?$/.exec(response);
      if (!match) throw new Error(`Malformed EXEC response: ${response}`);
      const output = hexDecode(match[2] ?? "").toString("utf8");
      return textResult(`${output}${output.endsWith("\n") || !output ? "" : "\n"}[exit ${match[1]}]`);
    },
  });

  pi.registerTool({
    name: "dos_abort",
    label: "DOS Abort",
    description: "Send Ctrl+C to interrupt the foreground DOS command without rebooting the VM.",
    promptSnippet: "Interrupt a hung DOS command without rebooting the VM",
    parameters: Type.Object({}),
    async execute() {
      await interruptForegroundCommand();
      const recovered = await waitForAgentRecovery();
      return textResult(recovered ? "Sent Ctrl+C; DOS agent recovered." : "Sent Ctrl+C; DOS agent has not responded yet.");
    },
  });
}
