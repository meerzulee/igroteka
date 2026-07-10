// Shared SAB protocol for the OPFS bridge (Approach B).
//
// The interpreter worker (worker.js) makes SYNCHRONOUS file calls from deep
// inside the wasm (guest CreateFile/GetFileAttributes/FindFirstFile). OPFS is
// async, so those calls are serviced by a dedicated fs-worker (fsworker.js)
// that owns OPFS access. They talk over two SharedArrayBuffers:
//
//   ctrl  (Int32Array)  — signaling + small scalar args/results
//   data  (Uint8Array)  — request path in, response bytes/dir-entries out
//
// Handshake (one in-flight request at a time; the interpreter is single-threaded):
//   interpreter: write path bytes to data[0..pathLen), fill ctrl args,
//                Atomics.store(ctrl, SIGNAL, REQ), postMessage('go') to fsworker,
//                Atomics.wait(ctrl, SIGNAL, REQ)
//   fsworker:    on 'go', read op/args, do async OPFS work, write result into
//                data + ctrl[STATUS], Atomics.store(ctrl, SIGNAL, RESP),
//                Atomics.notify(ctrl, SIGNAL)
//   interpreter: wakes, reads ctrl[STATUS] + data
//
// Case-insensitivity: guest paths are normalized lowercase (Win32 semantics);
// OPFS stores exact case. The fsworker indexes each directory and matches
// segments case-insensitively.

const FSPROTO = {
  // ctrl Int32Array indices
  SIGNAL: 0,     // IDLE=0, REQ=1, RESP=2
  OP: 1,         // one of OP_*
  PATH_LEN: 2,   // request path byte length, at data[0..PATH_LEN)
  OFFSET: 3,     // READ: byte offset into the file
  MAXLEN: 4,     // READ: max bytes to return (<= DATA_BYTES)
  STATUS: 5,     // response: see per-op below
  EXTRA: 6,      // response: LISTDIR packed-bytes length; else unused
  CTRL_LEN: 8,   // Int32Array length

  // SIGNAL states
  IDLE: 0,
  REQ: 1,
  RESP: 2,

  // ops
  OP_EXISTS: 1,  // STATUS: 1=exists(file), 2=exists(dir), 0=absent
  OP_STAT: 2,    // STATUS: file size in bytes, or -1 if absent/dir
  OP_READ: 3,    // STATUS: bytes written to data[0..), or -1 on error
  OP_LISTDIR: 4, // STATUS: entry count, or -1; EXTRA: packed byte length in data

  // sizes
  DATA_BYTES: 16 * 1024 * 1024, // 16 MB read chunk / listing buffer

  // LISTDIR packed entry format (little-endian), repeated STATUS times:
  //   u8  type      (1=file, 2=dir)
  //   u32 size      (0 for dirs)
  //   u16 nameLen   (bytes of the EXACT-CASE basename, UTF-8)
  //   u8[nameLen] name
};

if (typeof module !== 'undefined') module.exports = FSPROTO;
