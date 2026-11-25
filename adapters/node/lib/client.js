'use strict';

const net = require('net');
const dgram = require('dgram');
const {
  SRR_MSG_REQUEST,
  SRR_MSG_REPLY,
  SRR_MSG_ERROR,
  SRR_MSG_PULSE,
  IPC_HEADER_SIZE,
  encodeIpcRequest,
  encodeIpcPulse,
  encodeIpcGoodbye,
  decodeIpcHeader,
  packUdpDatagram,
  unpackUdpDatagram
} = require('./protocol');

/**
 * Asynchronous / Promise-based SRR IPC Client over Unix Domain Sockets.
 */
class SRRClient {
  constructor(serviceName, options = {}) {
    this.serviceName = serviceName;
    this.socketPath = serviceName.startsWith('/')
      ? serviceName
      : `${options.socketDir || '/tmp'}/srr_${serviceName}.sock`;
    this.client = null;
    this.nextTransId = 1;
    this.rxBuffer = Buffer.alloc(0);
    this.pendingRequests = new Map(); // tid -> { resolve, reject, timer }
  }

  connect() {
    return new Promise((resolve, reject) => {
      if (this.client) return resolve();

      const sock = net.createConnection({ path: this.socketPath });

      sock.on('connect', () => {
        this.client = sock;
        resolve();
      });

      sock.on('data', (chunk) => {
        this._handleData(chunk);
      });

      sock.on('error', (err) => {
        this._failAll(err);
        if (!this.client) reject(err);
      });

      sock.on('close', () => {
        this.client = null;
        this._failAll(new Error('Connection closed by peer'));
      });
    });
  }

  _handleData(chunk) {
    this.rxBuffer = Buffer.concat([this.rxBuffer, chunk]);

    while (this.rxBuffer.length >= IPC_HEADER_SIZE) {
      let hdr;
      try {
        hdr = decodeIpcHeader(this.rxBuffer);
      } catch (err) {
        this._failAll(err);
        this.close();
        return;
      }

      const totalFrameLen = IPC_HEADER_SIZE + hdr.payloadLen;
      if (this.rxBuffer.length < totalFrameLen) {
        break; // Wait for full frame
      }

      const payload = this.rxBuffer.subarray(IPC_HEADER_SIZE, totalFrameLen);
      this.rxBuffer = this.rxBuffer.subarray(totalFrameLen);

      const pending = this.pendingRequests.get(hdr.transId);
      if (pending) {
        clearTimeout(pending.timer);
        this.pendingRequests.delete(hdr.transId);

        if (hdr.msgType === SRR_MSG_ERROR) {
          pending.reject(new Error(`Service returned error: ${payload.toString('utf-8')}`));
        } else {
          pending.resolve(payload);
        }
      }
    }
  }

  _failAll(err) {
    for (const [tid, pending] of this.pendingRequests.entries()) {
      clearTimeout(pending.timer);
      pending.reject(err);
    }
    this.pendingRequests.clear();
  }

  async call(payload, timeoutMs = 2000) {
    if (!this.client) {
      await this.connect();
    }

    const tid = this.nextTransId++;
    const frame = encodeIpcRequest(tid, payload);

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingRequests.delete(tid);
        reject(new Error(`SRR call timed out after ${timeoutMs}ms (transId: ${tid})`));
      }, timeoutMs);

      this.pendingRequests.set(tid, { resolve, reject, timer });

      this.client.write(frame, (err) => {
        if (err) {
          clearTimeout(timer);
          this.pendingRequests.delete(tid);
          reject(err);
        }
      });
    });
  }

  async pulse(code, value) {
    if (!this.client) {
      await this.connect();
    }
    const frame = encodeIpcPulse(code, value);
    return new Promise((resolve, reject) => {
      this.client.write(frame, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
  }

  close() {
    if (this.client) {
      try {
        this.client.write(encodeIpcGoodbye());
        this.client.end();
      } catch (e) {
        // ignore
      }
      this.client = null;
    }
  }
}

/**
 * Synchronous Send/Receive/Reply Client over UDP.
 */
class SRRUDPClient {
  constructor(host, port, defaultTimeoutMs = 1500) {
    this.host = host;
    this.port = port;
    this.defaultTimeoutMs = defaultTimeoutMs;
    this.sessionId = Math.floor(Date.now() / 1000) & 0xFFFFFFFF;
    this.nextTransId = 1;
    this.socket = dgram.createSocket('udp4');
  }

  call(serviceId, operationId, payload = '', timeoutMs = null) {
    const effectiveTimeout = timeoutMs || this.defaultTimeoutMs;
    const tid = this.nextTransId++;

    const dgramBuf = packUdpDatagram({
      msgType: SRR_MSG_REQUEST,
      serviceId,
      operationId,
      sessionId: this.sessionId,
      transId: tid,
      payload
    });

    return new Promise((resolve, reject) => {
      let timer = null;

      const onMessage = (msg) => {
        try {
          const { header, payload: replyPayload } = unpackUdpDatagram(msg);
          if (header.transId === tid) {
            cleanup();
            if (header.msgType === SRR_MSG_ERROR) {
              reject(new Error(`UDP error: ${replyPayload.toString('utf-8')}`));
            } else {
              resolve(replyPayload);
            }
          }
        } catch (e) {
          // Ignore invalid frames from other packets
        }
      };

      const cleanup = () => {
        clearTimeout(timer);
        this.socket.removeListener('message', onMessage);
      };

      timer = setTimeout(() => {
        cleanup();
        reject(new Error(`SRR UDP call timed out after ${effectiveTimeout}ms`));
      }, effectiveTimeout);

      this.socket.on('message', onMessage);

      this.socket.send(dgramBuf, 0, dgramBuf.length, this.port, this.host, (err) => {
        if (err) {
          cleanup();
          reject(err);
        }
      });
    });
  }

  pulse(serviceId, operationId, code, value) {
    const body = Buffer.alloc(16);
    body.writeUInt32LE(code, 0);
    body.writeUInt32LE(0, 4);
    body.writeBigUInt64LE(BigInt(value), 8);

    const dgramBuf = packUdpDatagram({
      msgType: SRR_MSG_PULSE,
      serviceId,
      operationId,
      sessionId: this.sessionId,
      transId: 0,
      payload: body
    });

    return new Promise((resolve, reject) => {
      this.socket.send(dgramBuf, 0, dgramBuf.length, this.port, this.host, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
  }

  close() {
    if (this.socket) {
      try {
        this.socket.close();
      } catch (e) {}
      this.socket = null;
    }
  }
}

module.exports = {
  SRRClient,
  SRRUDPClient
};
