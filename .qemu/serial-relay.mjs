import net from "node:net";

const HOST = "127.0.0.1";
const QEMU_PORT = 5556;
const TOOL_PORT = 5555;
const OBSERVER_PORT = 5557;

let qemu = null;
let controller = null;
const observers = new Set();

function safeWrite(socket, data) {
  if (socket && !socket.destroyed && socket.writable) socket.write(data);
}

function decodeHex(hex) {
  if (!hex || hex.length % 2 || !/^[0-9a-f]+$/i.test(hex)) return null;
  return Buffer.from(hex, "hex").toString("utf8");
}

function visible(text) {
  const shortened = text.length > 400 ? `${text.slice(0, 400)}…` : text;
  return shortened
    .replace(/\r\n?/g, "\n")
    .replace(/[^\x20-\x7E\n]/g, (char) => `[0x${char.charCodeAt(0).toString(16).padStart(2, "0")}]`)
    .replace(/\n/g, "\r\n    ");
}

function formatLine(source, line) {
  if (!line) return null;
  if (line === "PING" || line === "QUIT" || line === "DOSAGENT READY") return `${source}  ${line}`;

  const [command, ...args] = line.split(" ");
  if (["READ", "LIST"].includes(command) && args[0]) {
    return `${source}  ${command} ${visible(decodeHex(args[0]) ?? args[0])}${args[1] ? ` @${args[1]}` : ""}`;
  }
  if (command === "WRITE" && args.length >= 3) {
    const path = decodeHex(args[0]) ?? args[0];
    const body = decodeHex(args.slice(2).join(" ")) ?? args.slice(2).join(" ");
    return `${source}  WRITE ${visible(path)} (${args[1] === "T" ? "replace" : "append"}) ${visible(body)}`;
  }
  if (command === "EXEC" && args[0]) return `${source}  EXEC ${visible(decodeHex(args.join(" ")) ?? args.join(" "))}`;
  if (command === "ERR" && args[0]) return `${source}  ERROR ${visible(decodeHex(args.join(" ")) ?? args.join(" "))}`;
  if (command === "OK") {
    const singlePayload = args.length === 1 ? decodeHex(args[0]) : null;
    const payload = singlePayload ?? (args.length > 1 ? decodeHex(args.slice(1).join(" ")) : null);
    const status = singlePayload !== null ? "" : (args[0] ? ` (${args[0]})` : "");
    return `${source}  OK${status}${payload !== null ? ` ${visible(payload)}` : ""}`;
  }
  return `${source}  ${line}`;
}

const traceBuffers = new Map();
function trace(source, data) {
  const pending = (traceBuffers.get(source) ?? "") + data.toString("ascii");
  const parts = pending.split(/\r?\n/);
  traceBuffers.set(source, parts.pop());
  for (const line of parts) {
    const formatted = formatLine(source, line);
    if (formatted) for (const observer of observers) safeWrite(observer, `${formatted}\r\n`);
  }
}

const qemuServer = net.createServer((socket) => {
  if (qemu && !qemu.destroyed) {
    socket.end();
    return;
  }
  qemu = socket;
  socket.setNoDelay(true);
  socket.on("data", (data) => {
    safeWrite(controller, data);
    trace("DOS -> Pi", data);
  });
  socket.on("close", () => {
    if (qemu === socket) qemu = null;
    if (controller && !controller.destroyed) controller.destroy();
  });
  socket.on("error", () => {});
});

const toolServer = net.createServer((socket) => {
  if (controller && !controller.destroyed) {
    socket.end("ERR 53455249414C20434F4E54524F4C4C45522042555359\r\n");
    return;
  }
  controller = socket;
  socket.setNoDelay(true);
  socket.on("data", (data) => {
    safeWrite(qemu, data);
    trace("Pi -> DOS", data);
  });
  socket.on("close", () => {
    if (controller === socket) controller = null;
  });
  socket.on("error", () => {});
});

const observerServer = net.createServer((socket) => {
  observers.add(socket);
  socket.setNoDelay(true);
  socket.write("DOS serial observer - decoded protocol (read-only)\r\n");
  socket.on("data", () => {});
  socket.on("close", () => observers.delete(socket));
  socket.on("error", () => observers.delete(socket));
});

qemuServer.listen(QEMU_PORT, HOST);
toolServer.listen(TOOL_PORT, HOST);
observerServer.listen(OBSERVER_PORT, HOST);
