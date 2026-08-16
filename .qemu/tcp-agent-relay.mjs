import net from "node:net";

const HOST = "127.0.0.1";
const TOOL_PORT = 5555;
const OBSERVER_PORT = 5557;
const TCP_AGENT_PORT = 5558;

let tcpAgent = null;
let controller = null;
const observers = new Set();

function safeWrite(socket, data) {
  if (socket && !socket.destroyed && socket.writable) socket.write(data);
}

function trace(source, data) {
  const text = data.toString("ascii");
  const shortened = text.length > 400 ? `${text.slice(0, 400)}…` : text;
  for (const observer of observers) safeWrite(observer, `${source} ${shortened}\r\n`);
}

const tcpAgentServer = net.createServer((socket) => {
  if (tcpAgent && !tcpAgent.destroyed) {
    socket.end();
    return;
  }
  tcpAgent = socket;
  socket.setNoDelay(true);
  socket.on("data", (data) => {
    safeWrite(controller, data);
    trace("DOS->Pi", data);
  });
  socket.on("close", () => {
    if (tcpAgent === socket) tcpAgent = null;
  });
  socket.on("error", () => {});
});

const toolServer = net.createServer((socket) => {
  if (controller && !controller.destroyed) {
    socket.end("ERR 544350204147454E5420434F4E54524F4C4C45522042555359\r\n");
    return;
  }
  if (!tcpAgent || tcpAgent.destroyed) {
    socket.end("ERR 544350204147454E54204E4F5420434F4E4E4543544544\r\n");
    return;
  }
  controller = socket;
  socket.setNoDelay(true);
  socket.on("data", (data) => {
    safeWrite(tcpAgent, data);
    trace("Pi->DOS", data);
  });
  socket.on("close", () => {
    if (controller === socket) controller = null;
  });
  socket.on("error", () => {});
});

const observerServer = net.createServer((socket) => {
  observers.add(socket);
  socket.setNoDelay(true);
  socket.write("DOS TCP agent observer (read-only)\r\n");
  socket.on("data", () => {});
  socket.on("close", () => observers.delete(socket));
  socket.on("error", () => observers.delete(socket));
});

tcpAgentServer.listen(TCP_AGENT_PORT, HOST);
toolServer.listen(TOOL_PORT, HOST);
observerServer.listen(OBSERVER_PORT, HOST);
