// Run the existing aion2web handler against an isolated SQLite DB, then send
// actual WinHTTP requests from our C++ reporter. Never contacts the live website.
import assert from 'node:assert/strict'
import { readFileSync, readdirSync, mkdtempSync } from 'node:fs'
import { DatabaseSync } from 'node:sqlite'
import { runInNewContext } from 'node:vm'
import { createRequire } from 'node:module'
import { createServer } from 'node:http'
import { spawn } from 'node:child_process'
import { join, resolve } from 'node:path'
import { tmpdir } from 'node:os'
const web = resolve(process.argv[2] || 'E:/project/aion2web')
const ts = createRequire(join(web, 'package.json'))('typescript')
function load(path, deps = {}) {
  const exports = {}
  runInNewContext(ts.transpileModule(readFileSync(join(web, path), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
  }).outputText, { exports, Response, URL, require: name => { assert.ok(name in deps, name); return deps[name] } })
  return exports
}
const db = new DatabaseSync(':memory:')
for (const file of readdirSync(join(web, 'migrations')).filter(n => n.endsWith('.sql')).sort()) db.exec(readFileSync(join(web, 'migrations', file), 'utf8'))
const servers = load('src/lib/aion2-servers.ts')
const parser = load('src/lib/character-upload.ts')
const service = load('src/server/characters.server.ts', {
  '#/lib/aion2-servers': servers,
  'cloudflare:workers': { env: { DB: {
    prepare: sql => ({ bind: (...args) => ({ sql, args }) }),
    batch: async statements => {
      db.exec('BEGIN')
      try { const results = statements.map(({ sql, args }) => ({ meta: db.prepare(sql).run(...args) })); db.exec('COMMIT'); return results }
      catch (e) { db.exec('ROLLBACK'); throw e }
    },
  }, UPLOAD_API_TOKEN: 'x'.repeat(40) } },
})
const route = load('src/routes/api/characters/upload.ts', {
  '@tanstack/react-router': { createFileRoute: () => options => options },
  '#/lib/character-upload': parser,
  '#/server/characters.server': service,
  '#/server/admin-auth.server': { currentAdminPrincipal: async () => null },
  '#/server/api-auth.server': {
    verifyBearerToken: async req => req.headers.get('authorization') === `Bearer ${'x'.repeat(40)}`,
    jsonError: (error, status, details) => Response.json({ ok: false, error, details }, { status }),
  },
}).Route
const startedAt = Date.now()
let requests = 0, uploads = 0
const server = createServer(async (req, res) => {
  try {
    assert.equal(req.url, '/api/characters/upload'); assert.equal(req.method, 'POST')
    const chunks = []; for await (const chunk of req) chunks.push(chunk)
    const body = Buffer.concat(chunks)
    const payload = JSON.parse(body)
    assert.deepEqual(Object.keys(payload), ['characters'])
    for (const c of payload.characters) {
      assert.equal(typeof c.characterId, 'string')
      assert.ok(!('fields' in c) && !('entity_key' in c) && !('position' in c))
    }
    if (requests === 0) assert.ok(Date.now() - startedAt >= 59000, "automatic collection window")
    requests++
    // Exercise a temporary failure; no local acknowledgment may be recorded.
    if (requests === 1) { res.writeHead(503); res.end('{}'); return }
    const response = await route.server.handlers.POST({ request: new Request('http://localhost/api/characters/upload', { method: 'POST', headers: req.headers, body }) })
    if (response.ok) uploads++
    res.writeHead(response.status, Object.fromEntries(response.headers)); res.end(await response.text())
  } catch (error) { res.writeHead(500); res.end(JSON.stringify({ error: String(error) })); console.error(error) }
})
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
try {
  const directory = mkdtempSync(join(tmpdir(), 'aion-report-integration-'))
  const child = spawn(resolve('build/Release/character_report_probe.exe'), [`http://127.0.0.1:${server.address().port}`, directory], { stdio: 'inherit' })
  assert.equal(await new Promise(resolve => child.on('exit', resolve)), 0)
  assert.equal(requests, 3); assert.equal(uploads, 2)
  const rows = db.prepare('SELECT character_name,character_id,server_id,level FROM game_characters').all()
  assert.deepEqual(rows.map(r => ({ ...r })), [{ character_name: '上报联调玩家', character_id: '282882351594255517', server_id: '1005', level: 31 }])
  const settings = readFileSync(join(directory, 'report-config.json'), 'utf8')
  assert.ok(!settings.includes('x'.repeat(40)))
  console.log('Existing web upload handler + C++ reporter passed: retry, dedup, update, restart, encrypted credentials.')
} finally { server.close(); db.close() }
