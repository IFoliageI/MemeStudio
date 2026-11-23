import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';

const cliExe = process.argv[2];
if (!cliExe || !fs.existsSync(cliExe)) {
  console.error(`Usage: node cli_e2e.mjs <path-to-MemeStudio-cli.exe> (got: ${cliExe})`);
  process.exit(1);
}

// Create 100% self-contained valid PNG (RGBA) & 2-frame animated GIF89a fixtures in memory
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    c ^= buf[i];
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const typeBuf = Buffer.from(type, 'ascii');
  const crcBuf = Buffer.alloc(4);
  crcBuf.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])), 0);
  return Buffer.concat([len, typeBuf, data, crcBuf]);
}

function makeSolidPng(width, height, r, g, b, a = 255) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // RGBA
  const raw = Buffer.alloc(height * (1 + width * 4));
  for (let y = 0; y < height; y++) {
    const rowOff = y * (1 + width * 4);
    raw[rowOff] = 0; // filter None
    for (let x = 0; x < width; x++) {
      const p = rowOff + 1 + x * 4;
      raw[p] = r;
      raw[p + 1] = g;
      raw[p + 2] = b;
      raw[p + 3] = a;
    }
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', zlib.deflateSync(raw)),
    pngChunk('IEND', Buffer.alloc(0))
  ]);
}

// Valid 2-frame 2x2 infinite-loop GIF89a
function makeTwoFrameGif() {
  return Buffer.from([
    0x47,0x49,0x46,0x38,0x39,0x61, // GIF89a
    0x02,0x00,0x02,0x00,0x80,0x00,0x00, // 2x2, GCT 2 colors
    0x10,0x20,0x30, 0xe0,0x50,0x60,     // Palette
    // NETSCAPE2.0 infinite loop (0)
    0x21,0xff,0x0b, 0x4e,0x45,0x54,0x53,0x43,0x41,0x50,0x45,0x32,0x2e,0x30, 0x03,0x01,0x00,0x00,0x00,
    // Frame 1 (delay 8 = 80ms)
    0x21,0xf9,0x04,0x04,0x08,0x00,0x00,0x00,
    0x2c,0x00,0x00,0x00,0x00,0x02,0x00,0x02,0x00,0x00,
    0x02,0x02,0x44,0x01,0x00,
    // Frame 2 (delay 12 = 120ms)
    0x21,0xf9,0x04,0x04,0x0c,0x00,0x00,0x00,
    0x2c,0x00,0x00,0x00,0x00,0x02,0x00,0x02,0x00,0x00,
    0x02,0x02,0x4c,0x01,0x00,
    0x3b // Trailer
  ]);
}

// Async CLI runner using regular file descriptors so:
// (a) Node's event loop stays unblocked to serve loopback HTTP/SSE requests concurrently, AND
// (b) It never creates named pipes (immune to EPERM in confined Windows sandboxes)!
async function runCli(args, sandboxDir, extraEnv = {}) {
  const outFile = path.join(sandboxDir, `cli-out-${Date.now()}-${Math.random().toString(16).slice(2)}.txt`);
  const errFile = path.join(sandboxDir, `cli-err-${Date.now()}-${Math.random().toString(16).slice(2)}.txt`);
  const outFd = fs.openSync(outFile, 'w');
  const errFd = fs.openSync(errFile, 'w');
  let exitCode = 1;
  try {
    exitCode = await new Promise((resolve, reject) => {
      const child = spawn(cliExe, args, {
        cwd: sandboxDir,
        env: { ...process.env, ...extraEnv },
        stdio: ['ignore', outFd, errFd]
      });
      child.on('error', reject);
      child.on('close', (code) => resolve(code ?? 1));
    });
  } finally {
    fs.closeSync(outFd);
    fs.closeSync(errFd);
  }
  const stdout = fs.readFileSync(outFile, 'utf8');
  const stderr = fs.readFileSync(errFile, 'utf8');
  fs.rmSync(outFile, { force: true });
  fs.rmSync(errFile, { force: true });
  return { status: exitCode, stdout, stderr };
}

async function main() {
  const baseTmp = fs.existsSync(os.tmpdir()) ? os.tmpdir() : process.cwd();
  const sandbox = fs.mkdtempSync(path.join(baseTmp, 'memestudio-e2e-'));

  let server = null;
  try {
    const srcPng = path.join(sandbox, 'source.png');
    const srcGif = path.join(sandbox, 'anim.gif');
    const charPng = path.join(sandbox, 'character.png');
    const returnedPng = makeSolidPng(8, 8, 30, 160, 240, 255);

    fs.writeFileSync(srcPng, makeSolidPng(8, 8, 200, 100, 50, 255));
    fs.writeFileSync(charPng, makeSolidPng(12, 12, 50, 200, 100, 255));
    fs.writeFileSync(srcGif, makeTwoFrameGif());

    // 1. Test `inspect` on PNG & animated GIF
    const inspPng = await runCli(['inspect', '--source', srcPng], sandbox);
    assert.equal(inspPng.status, 0, inspPng.stderr);
    const pngMeta = JSON.parse(inspPng.stdout);
    assert.equal(pngMeta.width, 8);
    assert.equal(pngMeta.frames, 1);
    assert.equal(pngMeta.animated, false);

    const inspGif = await runCli(['inspect', srcGif], sandbox);
    assert.equal(inspGif.status, 0, inspGif.stderr);
    const gifMeta = JSON.parse(inspGif.stdout);
    assert.equal(gifMeta.frames, 2);
    assert.equal(gifMeta.animated, true);
    assert.equal(gifMeta.loop, 0);

    // 2. Start ephemeral loopback Mock OpenAI server (supporting models, generations, and edits + SSE)
    let editRequests = 0;
    let genRequests = 0;
    server = http.createServer((req, res) => {
      if (req.url === '/v1/models' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
          data: [{ id: 'gpt-image-1' }, { id: 'custom-meme-v2' }, { id: 'gpt-image-1' }]
        }));
        return;
      }
      if (req.url === '/v1/images/generations' && req.method === 'POST') {
        req.on('data', () => {});
        req.on('end', () => {
          genRequests++;
          const b64 = returnedPng.toString('base64');
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ data: [{ b64_json: b64 }] }));
        });
        return;
      }
      if (req.url === '/v1/images/edits' && req.method === 'POST') {
        const chunks = [];
        req.on('data', (c) => chunks.push(c));
        req.on('end', () => {
          editRequests++;
          const bodyStr = Buffer.concat(chunks).toString('latin1');
          const b64 = returnedPng.toString('base64');
          if (req.headers.accept === 'text/event-stream' && bodyStr.includes('"stream"\r\n\r\ntrue')) {
            res.writeHead(200, { 'Content-Type': 'text/event-stream' });
            res.write(`event: image_edit.partial_image\r\ndata: ${JSON.stringify({ type: 'image_edit.partial_image', partial_image_index: 0, b64_json: b64 })}\r\n\r\n`);
            res.write(`event: image_edit.completed\r\ndata: ${JSON.stringify({ type: 'image_edit.completed', b64_json: b64 })}\r\n\r\n`);
            res.end('data: [DONE]\r\n\r\n');
          } else {
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ data: [{ b64_json: b64 }] }));
          }
        });
        return;
      }
      res.writeHead(404).end();
    });

    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    const port = server.address().port;
    const baseUrl = `http://127.0.0.1:${port}/v1`;

    // 3. Test `models` deduplication & sorting
    const modelsRes = await runCli(['models', '--base-url', baseUrl, '--api-key', 'sk-local-test'], sandbox);
    assert.equal(modelsRes.status, 0, modelsRes.stderr);
    const modelsJson = JSON.parse(modelsRes.stdout);
    assert.deepEqual(modelsJson.models, ['custom-meme-v2', 'gpt-image-1']);

    // 4. Test `run` with INI config (quoted Windows backslashes!), --count 2, --no-resume (zero .memestudio folders!)
    const outPngBase = path.join(sandbox, 'out.png');
    const iniPath = path.join(sandbox, 'job.ini');
    fs.writeFileSync(
      iniPath,
      `base_url = "${baseUrl}"\n` +
      `api_key = "sk-local-test"\n` +
      `model = "gpt-image-1"\n` +
      `source = "${srcPng}"\n` +
      `character = "${charPng}"\n` +
      `output = "${outPngBase}"\n` +
      `[crop]\nx = 0.0\ny = 0.0\nwidth = 1.0\nheight = 1.0\n`,
      'utf8'
    );

    const runPng = await runCli(['run', '--config', iniPath, '--count', '2', '--no-resume'], sandbox);
    assert.equal(runPng.status, 0, runPng.stderr);
    const runPngJson = JSON.parse(runPng.stdout);
    assert.equal(runPngJson.outputs.length, 2);
    assert.ok(fs.existsSync(path.join(sandbox, 'out_001.png')));
    assert.ok(fs.existsSync(path.join(sandbox, 'out_002.png')));

    // Verify ZERO .memestudio companion cache directories were created!
    const entries = fs.readdirSync(sandbox);
    assert.ok(!entries.some((n) => n.endsWith('.memestudio')), `Unexpected .memestudio folder in ${entries}`);

    // 5. Test `run --stream` on 2-frame animated GIF
    const outGif = path.join(sandbox, 'out_anim.gif');
    const runGif = await runCli([
      'run',
      '--base-url', baseUrl,
      '--api-key', 'sk-local-test',
      '--source', srcGif,
      '--character', charPng,
      '--output', outGif,
      '--stream',
      '--no-resume'
    ], sandbox);
    assert.equal(runGif.status, 0, runGif.stderr);
    assert.ok(fs.existsSync(outGif));

    const outGifMeta = JSON.parse((await runCli(['inspect', outGif], sandbox)).stdout);
    assert.equal(outGifMeta.animated, true);
    assert.equal(outGifMeta.frames, 2);
    assert.equal(editRequests, 4); // 2 copies * 1 frame + 2 GIF frames = 4 API calls

    // 6. Test built-in `draw` (pure prompt -> /v1/images/generations JSON)
    const drawnPng = path.join(sandbox, 'drawn.png');
    const drawRes = await runCli([
      'draw',
      '--base-url', baseUrl,
      '--api-key', 'sk-local-test',
      '--prompt', 'A happy sticker cat waving',
      '--output', drawnPng
    ], sandbox);
    assert.equal(drawRes.status, 0, drawRes.stderr);
    assert.ok(fs.existsSync(drawnPng));
    assert.equal(genRequests, 1);

    // 7. Test built-in `retouch` in BOTH prompt-guided full-image mode AND region-locked mode!
    const retPromptPng = path.join(sandbox, 'retouched_prompt.png');
    const retPromptRes = await runCli([
      'retouch',
      '--base-url', baseUrl,
      '--api-key', 'sk-local-test',
      '--source', srcPng,
      '--output', retPromptPng,
      '--mode', 'prompt',
      '--prompt', '给角色戴上一副黑色墨镜'
    ], sandbox);
    assert.equal(retPromptRes.status, 0, retPromptRes.stderr);
    assert.ok(fs.existsSync(retPromptPng));

    const retRegionPng = path.join(sandbox, 'retouched_region.png');
    const retRegionRes = await runCli([
      'retouch',
      '--base-url', baseUrl,
      '--api-key', 'sk-local-test',
      '--source', srcPng,
      '--output', retRegionPng,
      '--region', '0.5,0.5,0.5,0.5',
      '--prompt', '修复右下角手部细节'
    ], sandbox);
    assert.equal(retRegionRes.status, 0, retRegionRes.stderr);
    assert.ok(fs.existsSync(retRegionPng));
    assert.equal(editRequests, 6);

    console.log('MemeStudio CLI E2E test suite passed (run + draw + retouch, zero residue).');
  } finally {
    if (server) {
      await new Promise((resolve) => server.close(resolve));
    }
    fs.rmSync(sandbox, { recursive: true, force: true });
  }
}

main().catch((err) => {
  console.error('[E2E FAIL]', err);
  process.exit(1);
});
