#!/usr/bin/env node
// EDGE.GAME relay server -- Phase 1 (see ../NORTHSTAR.md's own phased plan).
//
// Holds one live TCP connection from the cabinet client (authenticated by EDGE_CLIENT_TOKEN) and
// exposes an HTTP API for the operator (authenticated by EDGE_OPERATOR_TOKEN) to send it a command
// and get the response back synchronously. Plain TCP + newline-delimited JSON, not WebSocket --
// avoids needing a WS handshake/framing library on the C client side, which matters once that
// client is cross-compiled for Windows (same reasoning DEADWEIGHT_2's own http.h being
// plain-HTTP-only, no mbedTLS, already established -- see NORTHSTAR.md's own "Architecture"
// section).
"use strict";

const net = require("net");
const http = require("http");
const crypto = require("crypto");

const CLIENT_PORT = parseInt(process.env.EDGE_CLIENT_PORT || "8091", 10);
const OPERATOR_PORT = parseInt(process.env.EDGE_OPERATOR_PORT || "8092", 10);
const CLIENT_TOKEN = process.env.EDGE_CLIENT_TOKEN || "";
const OPERATOR_TOKEN = process.env.EDGE_OPERATOR_TOKEN || "";
const COMMAND_TIMEOUT_MS = parseInt(process.env.EDGE_COMMAND_TIMEOUT_MS || "5000", 10);

if (!CLIENT_TOKEN || !OPERATOR_TOKEN) {
  console.error("EDGE_CLIENT_TOKEN and EDGE_OPERATOR_TOKEN must both be set");
  process.exit(1);
}

let cabinet = null; // { socket }
const pending = new Map(); // id -> { resolve, timer }

function send(socket, obj) {
  socket.write(JSON.stringify(obj) + "\n");
}

const clientServer = net.createServer((socket) => {
  let buf = "";
  let authed = false;
  socket.on("data", (chunk) => {
    buf += chunk.toString("utf8");
    let idx;
    while ((idx = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, idx);
      buf = buf.slice(idx + 1);
      if (!line.trim()) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        continue;
      }

      if (!authed) {
        if (msg.type === "hello" && msg.token === CLIENT_TOKEN) {
          authed = true;
          if (cabinet && cabinet.socket !== socket) cabinet.socket.destroy();
          cabinet = { socket };
          send(socket, { type: "hello_ok" });
          console.log("[relay] cabinet connected");
        } else {
          socket.destroy();
        }
        continue;
      }

      if (msg.id && pending.has(msg.id)) {
        const p = pending.get(msg.id);
        clearTimeout(p.timer);
        pending.delete(msg.id);
        p.resolve(msg);
      }
    }
  });
  socket.on("close", () => {
    if (cabinet && cabinet.socket === socket) {
      cabinet = null;
      console.log("[relay] cabinet disconnected");
    }
  });
  socket.on("error", () => {});
});

const operatorServer = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, cabinet_connected: !!cabinet }));
    return;
  }
  if (req.method === "POST" && req.url === "/commands") {
    const auth = req.headers["authorization"] || "";
    if (auth !== `Bearer ${OPERATOR_TOKEN}`) {
      res.writeHead(401, { "content-type": "application/json" });
      res.end(JSON.stringify({ error: "unauthorized" }));
      return;
    }
    if (!cabinet) {
      res.writeHead(503, { "content-type": "application/json" });
      res.end(JSON.stringify({ error: "no cabinet connected" }));
      return;
    }
    let body = "";
    req.on("data", (c) => (body += c));
    req.on("end", () => {
      let cmd;
      try {
        cmd = JSON.parse(body || "{}");
      } catch {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: "invalid JSON body" }));
        return;
      }
      const id = crypto.randomUUID();
      const timer = setTimeout(() => {
        pending.delete(id);
        res.writeHead(504, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: "cabinet did not respond in time" }));
      }, COMMAND_TIMEOUT_MS);
      pending.set(id, {
        resolve: (msg) => {
          res.writeHead(200, { "content-type": "application/json" });
          res.end(JSON.stringify(msg));
        },
        timer,
      });
      send(cabinet.socket, { id, type: cmd.type, payload: cmd.payload });
    });
    return;
  }
  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

clientServer.listen(CLIENT_PORT, () => console.log(`[relay] cabinet listener on :${CLIENT_PORT}`));
operatorServer.listen(OPERATOR_PORT, () => console.log(`[relay] operator API on :${OPERATOR_PORT}`));
