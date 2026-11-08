// PocketGPU の /kit を Cloudflare Workers で受ける。PC が切れていても配れるようにする
// - キットは PC が先に作って KV に置いておく（discord/kit_pool.py）。zip は 24 MiB ずつ blob:<番号>:<i> に分け、
//   /dl/<合言葉> でつないで渡す。合言葉は dl:<合言葉> に（番号・分けた数・大きさ・名前）。10/12 までは Google ドライブだった
// - /kit を受けたら、空いているキットを 1 つその人に割り当て「user:<Discord の番号>」に書く。1 人 1 回
// - 返事は本人にだけ見える形（ephemeral）。もう一度 /kit と打てば同じリンクをまた見せる
// - 7 日たったら PC が KV から中身を消し、user の記録に expired を付ける（kit_pool.py sweep）
// 利用者に見せる文は仮。言い回しは鶴田さんが直す
//
// 更新（10/12）：一度入れた人に、ベータの間も更新を配る。人ごとの「更新用の合言葉」は KV の upd:<合言葉>（キットの番号と印）。
// キットの tools\update.ini に入っている（make_kit.py が書く）。7 日でキットのリンクが切れても、更新用の合言葉は切れない
// - /u/<合言葉>/source.json        その人だけの AltStore のソース。iPhone のアプリの新しい版が出ると AltStore に Update が出る
// - /u/<合言葉>/update.json        PC 側の最新の版の一覧（ファイルごとの SHA-256・大きさ・印の場所）と、その人の印
// - /u/<合言葉>/f/<sha256>/<名前>  1 つのファイル。印の入る物（dll と IPA）は、その人の印をその場で入れて渡す
// - /u/<合言葉>/updater.zip        10/12 より前のキット用：Update-PocketGPU.cmd・tools\update.ps1・tools\update.ini の 3 つ
// 中身は publish_update.py が置く：urel:latest（最新の版の一覧）・urel:<n>（前の版）・ufile:<sha256>（並び）・ublob:<sha256>:<i>（中身）
// 印を入れる所は ublob に入れず、並びの 'm'（印 32 文字）と 'c'（印の CRC32。IPA の zip の中の 2 か所）で表す。
// worker は中身を読まずに流し、印と CRC だけを書くので、大きい IPA でも CPU をほとんど使わない
//
// 直し（10/13）：
// - /kit の割り当ては Durable Object（KitAllocator。SQLite の物で、無料の枠がある）に 1 本化した。KV は書き込みが全体に伝わるまで
//   数十秒かかり、2 人がほぼ同時に打つと同じ pool: を 2 人が読めた。DO は 1 つの名前に対して 1 か所で 1 本ずつ動くので重ならない。
//   KV の pool:・taken:・user:・upd: は今までの形のまま使う（kit_pool.py はそのまま動く）。DO ができる前に渡した人の user: は、
//   その人が /kit を打ったときに DO が取り込む。KV への書き込みは DO の outbox に入れておき、alarm で書く。失敗しても 1 分ごとに書き直す
// - /dl/ は Range（bytes=a-b・a-・-n）に 206 で答える。HEAD に Content-Length を付ける。KV には範囲を指定して読む方法が無いので、
//   要る塊だけを読み、塊の途中から始まる・途中で終わるときだけ、その塊を丸ごと読んで要る分を切り出す
// - 名前が .tar.gz・.tgz のキットは application/gzip で渡す
// - /u/ と /dl/ の後ろの合言葉の形が合わないときは 404 を返す
// - テスターのログの受け口 POST /log と、鶴田さんだけが読む管理ページ /admin/logs（Cloudflare Access の JWT を worker の中でも確かめる）を足した。下の LogStore の前の説明を見る

import { DurableObject } from 'cloudflare:workers';

const KEEP_DAYS = 7;
const CHUNK = 24 * 1024 * 1024;   // kit_pool.py の CHUNK と同じ。blob:<番号>:<i> の 1 つの大きさ（最後の 1 つだけ短い）
const EPHEMERAL = 64;
const TOKEN_RE = '[A-Za-z0-9_-]{16,64}';
const NO_MARK = '#'.repeat(32);

// AltStore のソースとアプリのアイコン（256x256 の PNG。ipa/serve/icon.png と同じ）
const ICON_PNG = 'iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAADSUlEQVR42u3cIbKFMBBFwSyD/WtkFoXIIigcgipAXDFJn7p6xP+0g9c2aeGaP4EAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAHis9/117ufuAwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHlAAALga47BCAwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABgTgBfPsi4zyNVa3//vwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGAAAGAAAGAAAGAAeBnOvAwHgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAEAgAGQyUfxPor3qxAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACApwoAAAwAL8OZl+EAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAAMADy+SjeR/F+FQIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADwVAEAgAHgZTjzMhwABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABgAABkA+H8X7KN6vQgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACeKgAAMAC8DGdehgPAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAKhf+kNs9+cIAPcBAMB9AABwHwAA3AcAAPcBAMB9AABwHwAA3AcAAPcBAMB9AABwHwAA3AcAAPcBAMB9AABwHwAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQAJAAkACQABIAEgASAt1wlF55ksLjCSkAAAAABJRU5ErkJggg==';

function hex(s) {
  const b = new Uint8Array(s.length / 2);
  for (let i = 0; i < b.length; i++) b[i] = parseInt(s.substr(i * 2, 2), 16);
  return b;
}

async function verified(req, body, pub) {
  const sig = req.headers.get('X-Signature-Ed25519');
  const ts = req.headers.get('X-Signature-Timestamp');
  if (!sig || !ts) return false;
  try {
    const key = await crypto.subtle.importKey('raw', hex(pub), { name: 'Ed25519' }, false, ['verify']);
    return await crypto.subtle.verify('Ed25519', key, hex(sig), new TextEncoder().encode(ts + body));
  } catch {
    return false;
  }
}

function reply(content) {
  return Response.json({ type: 4, data: { content, flags: EPHEMERAL } });
}

// 乱数の合言葉。24 バイトを URL に使える base64 にする（Python の secrets.token_urlsafe(24) と同じ 32 文字）
function newToken() {
  const b = crypto.getRandomValues(new Uint8Array(24));
  return btoa(String.fromCharCode(...b)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

// 更新の案内。10/12 より前のキットには Update-PocketGPU.cmd が無いので、更新の道具の zip を案内する
function updateText(base, utok, old) {
  let s = `\n\nUpdates / 更新\n` +
    `iPhone app: in AltStore, open Sources, tap +, and add this URL. When a new version is out, AltStore shows Update.\n` +
    `iPhone のアプリ：AltStore の「Sources」で「+」をタップして、この URL を追加してください。新しいバージョンが出ると、AltStore に「Update」が表示されます。\n` +
    `${base}/u/${utok}/source.json\n`;
  if (old) {
    s += `PC: your kit was made before updates existed. Download this zip, extract it, and double-click Update-PocketGPU.cmd in it. It finds your kit folder.\n` +
      `PC：このキットは更新の機能より前に作られました。次の zip をダウンロードして展開し、中の Update-PocketGPU.cmd をダブルクリックしてください。キットのフォルダを自動で探します。\n` +
      `${base}/u/${utok}/updater.zip`;
  } else {
    s += `PC: double-click Update-PocketGPU.cmd in the kit folder. It downloads only the files that changed.\n` +
      `PC：キットのフォルダの Update-PocketGPU.cmd をダブルクリックしてください。変更されたファイルだけをダウンロードします。`;
  }
  return s;
}

function kitText(link, base, utok, old) {
  return `Your PocketGPU test kit. It is marked for you, so please do not share it.\n${link}\n` +
    `The link expires ${KEEP_DAYS} days after you first got it. After unzipping, start with Start-PocketGPU.cmd: it checks your PC and iPhone step by step and tells you what to fix. Type /kit again to see this link again.\n\n` +
    `あなた専用の PocketGPU の試験用キットです。識別用の印が含まれているので、他の人に渡さないでください。\n` +
    `リンクは最初に受け取ってから ${KEEP_DAYS} 日で無効になります。展開したら、最初に Start-PocketGPU.cmd を起動してください。PC と iPhone を順にチェックして、必要な対処方法を表示します。もう一度 /kit と入力すると、このリンクを再表示できます。` +
    updateText(base, utok, old);
}

function expiredText(base, utok, old) {
  return 'Your kit download link has expired. You can still update the kit you installed. If you need a new kit, ask the admin.\n' +
    'キットのダウンロードのリンクは有効期限が切れています。インストール済みのキットは、引き続き更新できます。新しいキットが必要な場合は、管理者に連絡してください。' +
    updateText(base, utok, old);
}

async function kit(i, env, base) {
  if (i.channel_id !== env.KIT_CHANNEL_ID)
    return reply('Please use this in #get-kit. / #get-kit で使用してください。');
  const roles = i.member?.roles || [];
  if (!roles.includes(env.TESTER_ROLE_ID))
    return reply('Testers only. / テスターのみ使用できます。');
  const uid = i.member.user.id;
  let r;
  try {
    // 割り当ては全部 DO の中で決める。名前は 1 つだけ（全員が同じ DO に届く）
    r = await env.ALLOC.get(env.ALLOC.idFromName('kit')).assign(uid, i.member.user.username);
  } catch (e) {
    console.error('assign failed', String(e));
    return reply('Something went wrong. Please try /kit again in a moment. / うまく処理できませんでした。少し待ってから、もう一度 /kit と入力してください。');
  }
  if (!r.ok)
    return reply('No kits are ready right now. The admin has been told, please try again later. / 現在お渡しできるキットがありません。管理者が準備しますので、しばらくしてからもう一度お試しください。');
  const rec = r.rec;
  if (rec.expired) return reply(expiredText(base, rec.utoken, rec.uold));
  return reply(kitText(rec.link, base, rec.utoken, rec.uold));
}

// キットの割り当て。DO は同じ名前に 1 つだけあり、await の無い所は他の要求が割り込まない。
// そこで「空きを選ぶ・claims に書く」を 1 かたまり（await なし）にして、同じキットが 2 人に渡らず、同じ人に 2 つ渡らないようにする。
// KV は読み書きとも古い値が返ることがあるので、割り当て済みかどうかは KV ではなくこの DO の claims で決める
//   claims(uid 主キー, kid 一意, rec)  誰にどのキットを渡したか。rec は user: に書く記録と同じ形
//   outbox(k, v)                        KV に書くはずの物（v が NULL なら消す）。alarm が書いて消す
export class KitAllocator extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec('CREATE TABLE IF NOT EXISTS claims (uid TEXT PRIMARY KEY, kid TEXT NOT NULL UNIQUE, rec TEXT NOT NULL)');
    this.sql.exec('CREATE TABLE IF NOT EXISTS outbox (id INTEGER PRIMARY KEY AUTOINCREMENT, k TEXT NOT NULL, v TEXT)');
    this.chain = Promise.resolve();
    this.cache = [];            // [kid, pool: の中身] の並び。メモリだけ（DO が眠れば消える）
    this.cacheAt = 0;
    this.refilling = null;
    this.waiting = 0;           // 空きを待っている要求の数
  }

  mine(uid) {
    const r = this.sql.exec('SELECT rec FROM claims WHERE uid = ?', uid).toArray()[0];
    return r ? JSON.parse(r.rec) : null;
  }
  taken(kid) {
    return this.sql.exec('SELECT 1 FROM claims WHERE kid = ?', kid).toArray().length > 0;
  }
  save(uid, rec, ops) {
    this.sql.exec('INSERT INTO claims (uid, kid, rec) VALUES (?, ?, ?) ON CONFLICT(uid) DO UPDATE SET rec = excluded.rec', uid, rec.kit, JSON.stringify(rec));
    for (const [k, v] of ops) this.sql.exec('INSERT INTO outbox (k, v) VALUES (?, ?)', k, v === null ? null : JSON.stringify(v));
  }

  // 空きの写し。pool: の先頭から、claims に無い物を中身つきで持つ。KV の list は消した pool: を数十秒は返すことがあるので claims で外す。
  // 写しは 10 秒で捨てる（PC 側の retire で下げたキットを、いつまでも渡さないため）
  fresh() {
    return this.cache.length > 0 && Date.now() - this.cacheAt < 10_000;
  }
  // 写しを読み直す。同時に来た要求は 1 回の読み込みを待つ。読み込む数は 4 ＋ 待っている人数。読み込んだ数を返す（0 なら空）
  refill() {
    if (!this.refilling) {
      this.refilling = (async () => {
        const list = await this.env.KITS.list({ prefix: 'pool:', limit: 100 });
        const ids = list.keys.map(k => k.name.slice(5)).filter(id => !this.taken(id)).slice(0, 4 + this.waiting);
        const infos = await Promise.all(ids.map(id => this.env.KITS.get(`pool:${id}`, 'json')));
        this.cache = ids.map((id, n) => [id, infos[n]]).filter(([, v]) => v);
        this.cacheAt = Date.now();
        return this.cache.length;
      })().finally(() => { this.refilling = null; });
    }
    return this.refilling;
  }

  // 写しの先頭から、まだ誰にも渡していない物をその人に決める（await なし）
  claimNext(uid, name) {
    while (this.cache.length) {
      const [id, info] = this.cache.shift();
      if (this.taken(id)) continue;
      const at = new Date().toISOString();
      const ops = [];
      let utoken = info.utoken, uold = false;
      if (!utoken) {
        // 更新の機能より前に作り置いたキット：合言葉をここで作る。キットの中に Update-PocketGPU.cmd は無い
        utoken = newToken();
        uold = true;
        ops.push([`upd:${utoken}`, { kid: id, mark: info.mark || '', uid, at, late: true }]);
      }
      const rec = { kit: id, link: info.link, token: info.token, chunks: info.chunks, mark: info.mark, utoken, uold, at, name };
      ops.push([`taken:${id}`, { uid, ...rec }], [`user:${uid}`, rec], [`pool:${id}`, null]);
      this.save(uid, rec, ops);
      return rec;
    }
    return null;
  }

  // DO ができる前に渡した人（KV の user: にだけ記録がある）を、claims に取り込む
  adopt(uid, old) {
    const rec = { ...old };
    if (rec.utoken) {
      this.save(uid, rec, []);
    } else {
      // 10/12 より前に渡したキット：更新用の合言葉をここで 1 回だけ作る
      rec.utoken = newToken();
      rec.uold = true;
      this.save(uid, rec, [[`upd:${rec.utoken}`, { kid: rec.kit, mark: rec.mark || '', uid, at: new Date().toISOString(), late: true }], [`user:${uid}`, rec]]);
    }
    return rec;
  }

  async assign(uid, name) {
    let rec = this.mine(uid);
    if (!rec) {
      this.waiting++;
      try {
        const [old] = await Promise.all([this.env.KITS.get(`user:${uid}`, 'json'), this.fresh() ? null : this.refill()]);
        for (let tries = 0; tries < 20 && !rec; tries++) {
          // ---- ここから await なし：決める・書くまでを 1 かたまりにする ----
          rec = this.mine(uid);                  // 読んでいる間に、同じ人の別の要求が先に決めたかもしれない
          if (rec) break;
          if (old) { rec = this.adopt(uid, old); break; }
          if (!this.fresh()) this.cache = [];
          rec = this.claimNext(uid, name);
          if (rec) break;
          // ---- 写しを使い切った：読み直す。読み直しても 0 個なら本当に空 ----
          if (!(await this.refill())) { rec = this.mine(uid); break; }
        }
      } finally {
        this.waiting--;
      }
      if (!rec) return { ok: false };
      await this.ctx.storage.setAlarm(Date.now());   // KV への書き込みは alarm でする（返事を待たせない）
    } else {
      // 7 日の sweep が KV に付ける expired を拾う
      try {
        const kv = await this.env.KITS.get(`user:${uid}`, 'json');
        if (kv?.expired) rec = { ...rec, expired: kv.expired };
      } catch { }
    }
    return { ok: true, rec };
  }

  // outbox の物を KV に書く。同じキーは最後の物だけ。書けなかった物は残して 1 分後にもう一度
  flush() {
    this.chain = this.chain.then(() => this.drain(), () => this.drain());
    return this.chain;
  }
  async drain() {
    const rows = this.sql.exec('SELECT id, k, v FROM outbox ORDER BY id').toArray();
    if (!rows.length) return;
    const last = new Map();
    for (const r of rows) last.set(r.k, r);
    await Promise.all([...last.values()].map(async r => {
      try {
        if (r.v === null) await this.env.KITS.delete(r.k);
        else await this.env.KITS.put(r.k, r.v);
      } catch (e) {
        console.error('outbox', r.k.split(':')[0], String(e));
        return;
      }
      this.sql.exec('DELETE FROM outbox WHERE k = ? AND id <= ?', r.k, r.id);
    }));
    if (this.sql.exec('SELECT 1 FROM outbox LIMIT 1').toArray().length)
      await this.ctx.storage.setAlarm(Date.now() + 60_000);
  }
  async alarm() {
    await this.flush();
  }
}

// 名前に合わせた Content-Type。キットは Windows が .zip・Linux が .tar.gz
function mimeOf(name) {
  if (/\.zip$/i.test(name)) return 'application/zip';
  if (/\.(tar\.gz|tgz)$/i.test(name)) return 'application/gzip';
  return 'application/octet-stream';
}

// Range の読み方。null は「Range が無い・読まない」（全体を 200 で返す）、'bad' は範囲がおかしい（416）、それ以外は {start, end}（両端を含む）
// bytes=a-b・a-・-n に答える。bytes 以外の単位と、複数の範囲（a-b,c-d）は読まずに全体を返す（RFC 9110 で許されている）
function parseRange(h, size) {
  if (!h) return null;
  const m = /^\s*bytes\s*=\s*(.*?)\s*$/i.exec(h);
  if (!m || m[1].includes(',')) return null;
  const r = /^(\d*)-(\d*)$/.exec(m[1]);
  if (!r || (r[1] === '' && r[2] === '')) return 'bad';
  if (r[1] === '') {                                  // -n：後ろから n バイト。n が全体より大きければ全体
    const n = Number(r[2]);
    return n > 0 ? { start: Math.max(0, size - n), end: size - 1 } : 'bad';
  }
  const start = Number(r[1]);
  const end = r[2] === '' ? size - 1 : Number(r[2]);
  if (start >= size || end < start) return 'bad';
  return { start, end: Math.min(end, size - 1) };
}

// start〜end（両端を含む）を writable に流す。blob:<kid>:<n> は CHUNK ずつなので、重なる塊だけを読む。
// 塊の全部が要るときは stream でそのまま流す。塊の一部だけが要るときは、KV に範囲を指定して読む方法が無いので、
// 塊を丸ごと読んで要る分を切り出す（stream を JS で読み飛ばすより CPU が少ない。24 MiB の入れ物が 1 つ要る）
async function pumpRange(env, info, start, end, writable) {
  for (let n = Math.floor(start / CHUNK); n <= Math.floor(end / CHUNK); n++) {
    const base = n * CHUNK;
    const len = Math.min(CHUNK, info.size - base);
    const from = Math.max(start, base) - base;
    const to = Math.min(end, base + len - 1) - base;
    const key = `blob:${info.kid}:${n}`;
    if (from === 0 && to === len - 1) {
      const part = await env.KITS.get(key, 'stream');
      if (!part) throw new Error('missing part');
      await part.pipeTo(writable, { preventClose: true });
    } else {
      const buf = await env.KITS.get(key, 'arrayBuffer');
      if (!buf || buf.byteLength !== len) throw new Error('missing or bad part');
      const w = writable.getWriter();
      await w.write(new Uint8Array(buf, from, to - from + 1));
      w.releaseLock();
    }
  }
  await writable.close();
}

// /dl/<合言葉>：分けて置いたキットを順につないで渡す。FixedLengthStream で大きさを先に伝える。
// Range に 206 で答え、HEAD には大きさだけ返す。ETag は「キットの番号-大きさ」（中身は作り直さない）。If-Range が合わなければ全体を返す
async function download(token, req, env, ctx) {
  const info = await env.KITS.get(`dl:${token}`, 'json');
  if (!info) return new Response('This link has expired or does not exist. / このリンクは無効です。', { status: 404 });
  const etag = `"${info.kid}-${info.size}"`;
  const headers = {
    'Content-Type': mimeOf(info.name),
    'Content-Disposition': `attachment; filename="${info.name}"`,
    'Accept-Ranges': 'bytes',
    'ETag': etag,
    'Cache-Control': 'no-store',
  };
  let range = parseRange(req.headers.get('Range'), info.size);
  const ifRange = req.headers.get('If-Range');
  if (range && ifRange && ifRange !== etag) range = null;
  if (range === 'bad')
    return new Response('The requested range is not satisfiable. / 指定された範囲は取得できません。', { status: 416, headers: { ...headers, 'Content-Range': `bytes */${info.size}` } });
  const start = range ? range.start : 0;
  const end = range ? range.end : info.size - 1;
  const status = range ? 206 : 200;
  if (range) headers['Content-Range'] = `bytes ${start}-${end}/${info.size}`;
  if (req.method === 'HEAD') return new Response(null, { status, headers: { ...headers, 'Content-Length': String(end - start + 1) } });
  const { readable, writable } = new FixedLengthStream(end - start + 1);
  ctx.waitUntil(pumpRange(env, info, start, end, writable).catch(e => writable.abort(e).catch(() => {})));
  return new Response(readable, { status, headers });
}

// ---- 更新 ----
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(b) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}
function le32(n) { return new Uint8Array([n & 0xFF, (n >>> 8) & 0xFF, (n >>> 16) & 0xFF, (n >>> 24) & 0xFF]); }

const NO_STORE = { 'Cache-Control': 'no-store' };
function notFound(msg) {
  return new Response(msg || 'Not found. / 見つかりません。', { status: 404, headers: NO_STORE });
}

// 合言葉の持ち主。無い合言葉・止めた合言葉は null
async function owner(tok, env) {
  const u = await env.KITS.get(`upd:${tok}`, 'json');
  return u && !u.revoked ? u : null;
}
function markOf(u) {
  return /^[\x21-\x7e]{32}$/.test(u.mark || '') ? u.mark : NO_MARK;
}
function fileUrl(base, tok, sha, name) {
  return `${base}/u/${tok}/f/${sha}/${encodeURIComponent(name)}`;
}
function baseName(p) { return p.split('/').pop(); }

// PC 側の一覧。ファイルごとの URL と、その人の印を足して返す
function updateJson(rel, base, tok, u) {
  return {
    rel: rel.rel, version: rel.version, date: rel.date, notes: rel.notes || {}, mark: markOf(u),
    files: (rel.files || []).map(f => ({ ...f, url: fileUrl(base, tok, f.sha, baseName(f.path)) })),
    remove: rel.remove || [],
    apps: (rel.apps || []).map(a => ({ prefix: a.prefix, file: a.file, version: a.version, build: a.build, sha: a.sha, size: a.size,
      mark_at: a.mark_at, crc_at: a.crc_at, url: fileUrl(base, tok, a.sha, a.file) })),
  };
}

// その人だけの AltStore のソース。形は AltStore の資料（faq.altstore.io/developers/make-a-source）どおり。
// アプリごとに最新の版を 1 つだけ載せる。sha256 は人ごとに違う（印が入る）ので載せない（AltStore では無くてもよい項目）
function sourceJson(rel, base, tok) {
  const s = rel.source || {};
  return {
    name: s.name || 'PocketGPU beta',
    subtitle: s.subtitle || '',
    description: s.description || '',
    iconURL: `${base}/icon.png`,
    tintColor: s.tintColor || '#1B1B1B',
    apps: (rel.apps || []).map(a => ({
      name: a.name,
      bundleIdentifier: a.bundle,
      developerName: s.developer || 'PocketGPU',
      subtitle: a.subtitle || '',
      localizedDescription: a.desc || a.name,
      iconURL: `${base}/icon.png`,
      tintColor: s.tintColor || '#1B1B1B',
      category: 'utilities',
      versions: [{
        version: a.version,
        buildVersion: a.build,
        date: a.date,
        localizedDescription: a.notes || '',
        downloadURL: fileUrl(base, tok, a.sha, a.file),
        size: a.size,
        minOSVersion: a.minOS,
      }],
      appPermissions: a.perm || { entitlements: [], privacy: {} },
    })),
    news: [],
  };
}

// 1 つのファイルを並び（ufile:<sha>）のとおりに流す。'b' は次の ublob、'm' は印、'c' は印の CRC32
async function sendFile(info, sha, name, u, env, ctx, head) {
  const headers = {
    'Content-Type': 'application/octet-stream',
    'Content-Disposition': `attachment; filename="${name.replace(/[^A-Za-z0-9._-]/g, '_')}"`,
    ...NO_STORE,
  };
  if (head) return new Response(null, { headers: { ...headers, 'Content-Length': String(info.size) } });
  const mark = new TextEncoder().encode(markOf(u));
  const crc = le32(crc32(mark));
  const { readable, writable } = new FixedLengthStream(info.size);
  const pump = (async () => {
    let k = 0;
    for (const [kind, len] of info.parts) {
      if (kind === 'b') {
        const part = await env.KITS.get(`ublob:${sha}:${k++}`, 'stream');
        if (!part) { await writable.abort(new Error('missing part')); return; }
        await part.pipeTo(writable, { preventClose: true });
      } else {
        const bytes = kind === 'm' ? mark : crc;
        if (bytes.length !== len) { await writable.abort(new Error('bad layout')); return; }
        const w = writable.getWriter();
        await w.write(bytes);
        w.releaseLock();
      }
    }
    await writable.close();
  })();
  ctx.waitUntil(pump.catch(() => {}));
  return new Response(readable, { headers });
}

// 印の入らない小さいファイルを、まとめて読む（更新の道具の zip 用）
async function readSmall(sha, env) {
  const info = await env.KITS.get(`ufile:${sha}`, 'json');
  if (!info || info.parts.some(p => p[0] !== 'b')) return null;
  const out = new Uint8Array(info.size);
  let off = 0, k = 0;
  for (const [, len] of info.parts) {
    const b = await env.KITS.get(`ublob:${sha}:${k++}`, 'arrayBuffer');
    if (!b || b.byteLength !== len) return null;
    out.set(new Uint8Array(b), off);
    off += len;
  }
  return out;
}

// 圧縮しない zip。中身は 3 つで 60 KB ほど
function zipStore(files) {
  const enc = new TextEncoder();
  const parts = [], central = [];
  let off = 0;
  const d = new Date();
  const dosTime = (d.getUTCHours() << 11) | (d.getUTCMinutes() << 5) | (d.getUTCSeconds() >> 1);
  const dosDate = ((d.getUTCFullYear() - 1980) << 9) | ((d.getUTCMonth() + 1) << 5) | d.getUTCDate();
  for (const f of files) {
    const name = enc.encode(f.name);
    const crc = crc32(f.data);
    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true); lh.setUint16(6, 0x0800, true); lh.setUint16(8, 0, true);
    lh.setUint16(10, dosTime, true); lh.setUint16(12, dosDate, true); lh.setUint32(14, crc, true);
    lh.setUint32(18, f.data.length, true); lh.setUint32(22, f.data.length, true); lh.setUint16(26, name.length, true); lh.setUint16(28, 0, true);
    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true); ch.setUint16(4, 20, true); ch.setUint16(6, 20, true); ch.setUint16(8, 0x0800, true); ch.setUint16(10, 0, true);
    ch.setUint16(12, dosTime, true); ch.setUint16(14, dosDate, true); ch.setUint32(16, crc, true);
    ch.setUint32(20, f.data.length, true); ch.setUint32(24, f.data.length, true); ch.setUint16(28, name.length, true);
    ch.setUint16(30, 0, true); ch.setUint16(32, 0, true); ch.setUint16(34, 0, true); ch.setUint16(36, 0, true); ch.setUint32(38, 0, true); ch.setUint32(42, off, true);
    parts.push(new Uint8Array(lh.buffer), name, f.data);
    central.push(new Uint8Array(ch.buffer), name);
    off += 30 + name.length + f.data.length;
  }
  const cdSize = central.reduce((n, b) => n + b.length, 0);
  const eocd = new DataView(new ArrayBuffer(22));
  eocd.setUint32(0, 0x06054b50, true); eocd.setUint16(8, files.length, true); eocd.setUint16(10, files.length, true);
  eocd.setUint32(12, cdSize, true); eocd.setUint32(16, off, true);
  const all = [...parts, ...central, new Uint8Array(eocd.buffer)];
  const out = new Uint8Array(all.reduce((n, b) => n + b.length, 0));
  let p = 0;
  for (const b of all) { out.set(b, p); p += b.length; }
  return out;
}

async function updaterZip(rel, base, tok, env) {
  const files = [];
  for (const path of ['Update-PocketGPU.cmd', 'tools/update.ps1']) {
    const f = (rel.files || []).find(x => x.path === path);
    const data = f && await readSmall(f.sha, env);
    if (!data) return new Response('The updater is not published yet. / 更新用のファイルはまだ公開されていません。', { status: 503, headers: NO_STORE });
    files.push({ name: path, data });
  }
  files.push({ name: 'tools/update.ini', data: new TextEncoder().encode(`url=${base}\r\ntoken=${tok}\r\n`) });
  return new Response(zipStore(files), { headers: {
    'Content-Type': 'application/zip',
    'Content-Disposition': 'attachment; filename="PocketGPU-updater.zip"',
    ...NO_STORE,
  } });
}

async function updates(m, req, env, ctx, base) {
  const tok = m[1], what = m[2];
  const u = await owner(tok, env);
  if (!u) return notFound('This update link is not valid. Type /kit in #get-kit. / この更新用のリンクは無効です。#get-kit で /kit と入力してください。');
  if (what.startsWith('f/')) {
    const sha = m[3];
    const info = await env.KITS.get(`ufile:${sha}`, 'json');
    if (!info) return notFound();
    let name = sha;
    try { if (m[4]) name = decodeURIComponent(m[4]); } catch { }
    return sendFile(info, sha, name, u, env, ctx, req.method === 'HEAD');
  }
  const rel = await env.KITS.get('urel:latest', 'json');
  if (!rel) return new Response('No update has been published yet. / 配布中の更新はまだありません。', { status: 503, headers: NO_STORE });
  if (what === 'update.json') return Response.json(updateJson(rel, base, tok, u), { headers: NO_STORE });
  if (what === 'source.json') return Response.json(sourceJson(rel, base, tok), { headers: NO_STORE });
  return updaterZip(rel, base, tok, env);
}

const U_PATH = new RegExp(`^/u/(${TOKEN_RE})/(source\\.json|update\\.json|updater\\.zip|f/([0-9a-f]{64})(?:/([A-Za-z0-9._%-]{1,120}))?)$`);

// ---- テスターのログの受け口（POST /log）と、鶴田さんだけが開ける管理ページ（/admin/）----
// 送る側：PC アプリ（PocketGPU.exe）は tools\update.ini の合言葉を X-PG-Token に、iPhone のアプリは PGKitMark.txt の印を X-PG-Mark に入れる。
// 保存は Durable Object（LogStore。SQLite の物。無料の枠）。KV には書かない（/kit・更新の 1 日 1,000 回の枠を使わない）。
// 30 日たった物と、全体が上限を超えた分（古い物から）は、DO の alarm で消す。数える・入れる・消すが DO の中で 1 本ずつ動くので、回数の上限が重ならない。
// 受ける側は合言葉・印・IP アドレスを保存せず、ログにも書かない。
const LOG_MAX = 1_900_000;                 // 1 件の上限（gzip したあとのバイト数）。DO の SQLite の 1 行は 2,000,000 バイトまで
const LOG_DAILY = 10;                      // 1 つのキットが 1 日（UTC）に送れる回数。pc と iphone は別々に数える
const LOG_DAILY_ALL = 2000;                // 全体で 1 日に受ける件数の上限（DO の書き込みが無料の枠を超えないように）
const LOG_KEEP_DAYS = 30;
const LOG_CAP_BYTES = 200_000_000;         // 全体の上限。超えたら古い物から消す
const SCAN_EVERY = 15 * 60_000;            // 知らない印が来たとき、upd: を読み直す間隔
const SEC_HEADERS = { 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer' };

function jsonRes(obj, status = 200, extra = {}) {
  return Response.json(obj, { status, headers: { ...SEC_HEADERS, ...extra } });
}

// 本文を max バイトまで読む。超えたら null
async function readCapped(req, max) {
  if (Number(req.headers.get('Content-Length')) > max) return null;
  if (!req.body) return new ArrayBuffer(0);
  const reader = req.body.getReader();
  const parts = [];
  let n = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    n += value.length;
    if (n > max) { await reader.cancel(); return null; }
    parts.push(value);
  }
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out.buffer;
}

// POST /log。状態の番号：201 受けた・400 ヘッダが違う・403 知らない/止めた合言葉か印（中身は捨てる）・405 POST 以外・
// 413 大きすぎる・415 gzip ではない・429 今日の回数が上限・503 受けられない（あとでもう一度）
async function receiveLog(req, env) {
  if (req.method !== 'POST') return jsonRes({ ok: false, error: 'method' }, 405, { Allow: 'POST' });
  const h = n => (req.headers.get(n) || '').trim();
  const tok = h('X-PG-Token'), mark = h('X-PG-Mark');
  const tokOk = new RegExp(`^${TOKEN_RE}$`).test(tok), markOk = /^[\x21-\x7e]{32}$/.test(mark);
  if (tokOk === markOk) return jsonRes({ ok: false, error: 'forbidden' }, 403);      // どちらか 1 つだけ。形が違うのも同じ返事
  const kind = h('X-PG-Kind'), trigger = h('X-PG-Trigger'), ver = h('X-PG-Version');
  if (kind !== (tokOk ? 'pc' : 'iphone') || !['manual', 'auto'].includes(trigger) || !/^[A-Za-z0-9._+-]{1,32}$/.test(ver))
    return jsonRes({ ok: false, error: 'bad header' }, 400);
  const body = await readCapped(req, LOG_MAX);
  if (!body) return jsonRes({ ok: false, error: 'too large', max: LOG_MAX }, 413);
  const b = new Uint8Array(body);
  if (b.length < 18 || b[0] !== 0x1f || b[1] !== 0x8b || b[2] !== 8) return jsonRes({ ok: false, error: 'not gzip' }, 415);
  let r;
  try {
    r = await env.LOGS.get(env.LOGS.idFromName('log')).receive({ tok: tokOk ? tok : '', mark: markOk ? mark : '', kind, trigger, ver }, body);
  } catch (e) {
    console.error('log receive failed', String(e));
    return jsonRes({ ok: false, error: 'unavailable' }, 503);
  }
  if (r.status === 201) return jsonRes({ ok: true }, 201);
  if (r.status === 429) return jsonRes({ ok: false, error: 'daily limit' }, 429, { 'Retry-After': String(r.retry) });
  if (r.status === 403) return jsonRes({ ok: false, error: 'forbidden' }, 403);
  return jsonRes({ ok: false, error: 'unavailable' }, 503);
}

export class LogStore extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS logs (id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, kid TEXT NOT NULL, uid TEXT NOT NULL,
      name TEXT NOT NULL, kind TEXT NOT NULL, trigger TEXT NOT NULL, ver TEXT NOT NULL, size INTEGER NOT NULL, data BLOB NOT NULL)`);
    this.sql.exec('CREATE INDEX IF NOT EXISTS logs_at ON logs(at)');
    this.sql.exec('CREATE TABLE IF NOT EXISTS counts (day TEXT NOT NULL, kid TEXT NOT NULL, kind TEXT NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (day, kid, kind))');
    this.sql.exec('CREATE TABLE IF NOT EXISTS seen (tok TEXT PRIMARY KEY, mark TEXT)');
    this.sql.exec('CREATE UNIQUE INDEX IF NOT EXISTS seen_mark ON seen(mark)');
    this.sql.exec('CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT)');
    this.total = this.sql.exec('SELECT COALESCE(SUM(size), 0) AS s FROM logs').one().s;
  }

  // 印から更新用の合言葉を引く。upd:<合言葉> の中身に mark がある。知らない印のときだけ、まだ読んでいない upd: を読み足す（15 分に 1 回まで）
  async tokenOfMark(mark) {
    const find = () => this.sql.exec('SELECT tok FROM seen WHERE mark = ?', mark).toArray()[0]?.tok;
    let t = find();
    if (t) return t;
    const last = Number(this.sql.exec("SELECT v FROM meta WHERE k = 'scan'").toArray()[0]?.v || 0);
    if (Date.now() - last < SCAN_EVERY) return null;
    this.sql.exec("INSERT INTO meta (k, v) VALUES ('scan', ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v", String(Date.now()));
    const names = [];
    let cursor;
    do {
      const l = await this.env.KITS.list({ prefix: 'upd:', cursor, limit: 1000 });
      for (const k of l.keys) names.push(k.name.slice(4));
      cursor = l.list_complete ? undefined : l.cursor;
    } while (cursor);
    const known = new Set(this.sql.exec('SELECT tok FROM seen').toArray().map(r => r.tok));
    const todo = names.filter(n => !known.has(n));
    for (let i = 0; i < todo.length; i += 20) {
      const vals = await Promise.all(todo.slice(i, i + 20).map(n => this.env.KITS.get(`upd:${n}`, 'json')));
      vals.forEach((v, j) => this.sql.exec('INSERT OR IGNORE INTO seen (tok, mark) VALUES (?, ?)', todo[i + j], /^[\x21-\x7e]{32}$/.test(v?.mark || '') ? v.mark : null));
    }
    return find() || null;
  }

  // 30 日たった物と、上限を超えた分を消す
  cleanup() {
    const old = this.sql.exec('SELECT COALESCE(SUM(size), 0) AS s FROM logs WHERE at < ?', Date.now() - LOG_KEEP_DAYS * 86400_000).one().s;
    this.sql.exec('DELETE FROM logs WHERE at < ?', Date.now() - LOG_KEEP_DAYS * 86400_000);
    this.total -= old;
    while (this.total > LOG_CAP_BYTES) {
      const r = this.sql.exec('SELECT id, size FROM logs ORDER BY id LIMIT 1').toArray()[0];
      if (!r) { this.total = 0; break; }
      this.sql.exec('DELETE FROM logs WHERE id = ?', r.id);
      this.total -= r.size;
    }
    this.sql.exec('DELETE FROM counts WHERE day < ?', new Date(Date.now() - 3 * 86400_000).toISOString().slice(0, 10));
  }

  async receive(m, body) {
    // 誰か。PC は合言葉、iPhone は印から合言葉を引く。止めた(revoked)人・知らない人は 403
    const tok = m.tok || (m.mark ? await this.tokenOfMark(m.mark) : null);
    const upd = tok ? await this.env.KITS.get(`upd:${tok}`, 'json') : null;
    if (!upd || upd.revoked || !upd.kid) return { status: 403 };
    if (m.mark && upd.mark !== m.mark) return { status: 403 };
    let who = null;
    try { who = await this.env.KITS.get(`taken:${upd.kid}`, 'json'); } catch { }
    // ---- ここから await なし：数える・入れる・消すを 1 かたまりにする ----
    const now = Date.now(), day = new Date(now).toISOString().slice(0, 10);
    const retry = Math.ceil((Date.parse(day) + 86400_000 - now) / 1000);
    const used = this.sql.exec('SELECT n FROM counts WHERE day = ? AND kid = ? AND kind = ?', day, upd.kid, m.kind).toArray()[0]?.n || 0;
    const all = this.sql.exec('SELECT COALESCE(SUM(n), 0) AS s FROM counts WHERE day = ?', day).one().s;
    if (used >= LOG_DAILY || all >= LOG_DAILY_ALL) return { status: 429, retry };
    this.sql.exec('INSERT INTO counts (day, kid, kind, n) VALUES (?, ?, ?, 1) ON CONFLICT(day, kid, kind) DO UPDATE SET n = n + 1', day, upd.kid, m.kind);
    this.sql.exec('INSERT INTO logs (at, kid, uid, name, kind, trigger, ver, size, data) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)',
      now, upd.kid, String(who?.uid || ''), String(who?.name || ''), m.kind, m.trigger, m.ver, body.byteLength, body);
    this.total += body.byteLength;
    this.cleanup();
    if (!(await this.ctx.storage.getAlarm())) await this.ctx.storage.setAlarm(now + 6 * 3600_000);
    return { status: 201 };
  }

  async alarm() {
    this.cleanup();
    await this.ctx.storage.setAlarm(Date.now() + 6 * 3600_000);
  }

  // 一覧。絞り込み：name（部分一致）・kind・trigger・ver。before=<id> でそれより古い物、after=<id> でそれより新しい物（古い順）
  list(f) {
    const w = [], a = [];
    if (f.name) { w.push("(name LIKE ? ESCAPE '\\' OR kid = ? OR uid = ?)"); a.push('%' + f.name.replace(/[\\%_]/g, c => '\\' + c) + '%', f.name, f.name); }
    for (const k of ['kind', 'trigger', 'ver']) if (f[k]) { w.push(`${k} = ?`); a.push(f[k]); }
    let order = 'DESC';
    if (f.after) { w.push('id > ?'); a.push(f.after); order = 'ASC'; }
    else if (f.before) { w.push('id < ?'); a.push(f.before); }
    const limit = Math.min(Math.max(f.limit || 100, 1), 500);
    const logs = this.sql.exec(`SELECT id, at, kid, uid, name, kind, trigger, ver, size FROM logs ${w.length ? 'WHERE ' + w.join(' AND ') : ''} ORDER BY id ${order} LIMIT ${limit}`, ...a).toArray();
    const people = this.sql.exec("SELECT name, kid, COUNT(*) AS n FROM logs GROUP BY kid ORDER BY name").toArray();
    const sum = this.sql.exec('SELECT COUNT(*) AS n, COALESCE(SUM(size), 0) AS bytes FROM logs').one();
    return { logs, people, count: sum.n, bytes: sum.bytes, keepDays: LOG_KEEP_DAYS, capBytes: LOG_CAP_BYTES };
  }
  get(id) {
    const r = this.sql.exec('SELECT id, at, kid, uid, name, kind, trigger, ver, size, data FROM logs WHERE id = ?', id).toArray()[0];
    return r || null;
  }
}

// ---- 管理ページの守り：Cloudflare Access が付ける Cf-Access-Jwt-Assertion を、この worker の中でも確かめる ----
// ACCESS_TEAM_DOMAIN（例 myteam.cloudflareaccess.com）と ACCESS_AUD（Access のアプリの AUD）は secret。どちらかが無ければ管理ページは 503 で閉じる。
// ACCESS_CERTS_URL は試験用（本番では置かない）：鍵の一覧を取る場所を変える
const jwks = { at: 0, tried: 0, keys: new Map() };
const b64u = s => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/')), c => c.charCodeAt(0));
const jsonPart = s => JSON.parse(new TextDecoder().decode(b64u(s)));

async function accessKey(env, kid) {
  const now = Date.now();
  if ((!jwks.keys.has(kid) || now - jwks.at > 3600_000) && now - jwks.tried > 60_000) {   // 取り直しは 1 分に 1 回まで
    jwks.tried = now;
    const r = await fetch(env.ACCESS_CERTS_URL || `https://${env.ACCESS_TEAM_DOMAIN}/cdn-cgi/access/certs`);
    if (r.ok) {
      const keys = new Map();
      for (const k of (await r.json()).keys || []) {
        if (k.kty !== 'RSA' || !k.kid) continue;
        keys.set(k.kid, await crypto.subtle.importKey('jwk', { kty: 'RSA', n: k.n, e: k.e, alg: 'RS256' }, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, false, ['verify']));
      }
      jwks.keys = keys;
      jwks.at = now;
    }
  }
  return jwks.keys.get(kid);
}

// 通れば { ok: true, who }。通らなければ { ok: false, why }。why はログ用（JWT の中身は書かない）
async function accessVerify(req, env) {
  if (!env.ACCESS_TEAM_DOMAIN || !env.ACCESS_AUD) return { ok: false, why: 'not configured', status: 503 };
  const jwt = req.headers.get('Cf-Access-Jwt-Assertion');
  if (!jwt) return { ok: false, why: 'no jwt' };
  try {
    const p = jwt.split('.');
    if (p.length !== 3) return { ok: false, why: 'shape' };
    const head = jsonPart(p[0]), c = jsonPart(p[1]);
    if (head.alg !== 'RS256' || !head.kid) return { ok: false, why: 'alg' };
    const key = await accessKey(env, head.kid);
    if (!key) return { ok: false, why: 'unknown kid' };
    if (!(await crypto.subtle.verify('RSASSA-PKCS1-v1_5', key, b64u(p[2]), new TextEncoder().encode(p[0] + '.' + p[1])))) return { ok: false, why: 'signature' };
    const now = Date.now() / 1000;
    if (typeof c.exp !== 'number' || c.exp < now - 30) return { ok: false, why: 'expired' };
    if (c.nbf !== undefined && c.nbf > now + 30) return { ok: false, why: 'not yet' };
    if (c.iss !== `https://${env.ACCESS_TEAM_DOMAIN}`) return { ok: false, why: 'iss' };
    if (![].concat(c.aud || []).includes(env.ACCESS_AUD)) return { ok: false, why: 'aud' };
    return { ok: true, who: c.email || c.common_name || 'service' };
  } catch (e) {
    return { ok: false, why: 'error' };
  }
}

async function gunzip(buf, cap) {
  const reader = new Blob([buf]).stream().pipeThrough(new DecompressionStream('gzip')).getReader();
  const parts = [];
  let n = 0, truncated = false;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (n + value.length > cap) { parts.push(value.subarray(0, cap - n)); n = cap; truncated = true; await reader.cancel(); break; }
    parts.push(value);
    n += value.length;
  }
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return { bytes: out, truncated };
}

function fileSafe(s) { return (String(s).replace(/[^A-Za-z0-9_-]/g, '') || 'user').slice(0, 24); }

async function admin(req, env, url) {
  if (req.method !== 'GET' && req.method !== 'HEAD') return jsonRes({ error: 'method' }, 405, { Allow: 'GET, HEAD' });
  const a = await accessVerify(req, env);
  if (!a.ok) {
    console.warn('admin denied:', a.why);
    return jsonRes({ error: a.status === 503 ? 'admin is not configured' : 'forbidden' }, a.status || 403);
  }
  const p = url.pathname;
  const store = () => env.LOGS.get(env.LOGS.idFromName('log'));
  if (p === '/admin' || p === '/admin/') return Response.redirect(url.origin + '/admin/logs', 302);
  if (p === '/admin/logs') return new Response(ADMIN_HTML, { headers: { ...SEC_HEADERS, 'Content-Type': 'text/html; charset=utf-8',
    'Content-Security-Policy': "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'" } });
  if (p === '/admin/api/logs') {
    const q = url.searchParams, num = k => (/^\d{1,12}$/.test(q.get(k) || '') ? Number(q.get(k)) : 0);
    const f = { name: (q.get('name') || '').slice(0, 64), kind: ['pc', 'iphone'].includes(q.get('kind')) ? q.get('kind') : '',
      trigger: ['manual', 'auto'].includes(q.get('trigger')) ? q.get('trigger') : '', ver: (q.get('ver') || '').slice(0, 32),
      before: num('before'), after: num('after'), limit: num('limit') };
    return jsonRes(await store().list(f));
  }
  const m = p.match(/^\/admin\/api\/logs\/(\d{1,12})$/);
  if (!m) return jsonRes({ error: 'not found' }, 404);
  const row = await store().get(Number(m[1]));
  if (!row) return jsonRes({ error: 'not found' }, 404);
  const { data, ...meta } = row;
  const base = `pg-${meta.kind}-${fileSafe(meta.name || meta.kid)}-${meta.id}`;
  const q = url.searchParams;
  if (q.get('raw')) return new Response(data, { headers: { ...SEC_HEADERS, 'Content-Type': 'application/gzip', 'Content-Disposition': `attachment; filename="${base}.log.gz"` } });
  if (q.get('text')) {
    let t;
    try { t = await gunzip(data, 16 * 1024 * 1024); } catch { return jsonRes({ error: 'not a valid gzip' }, 422); }
    return new Response(t.bytes, { headers: { ...SEC_HEADERS, 'Content-Type': 'text/plain; charset=utf-8', 'X-Truncated': t.truncated ? '1' : '0',
      'Content-Disposition': `${q.get('dl') ? 'attachment' : 'inline'}; filename="${base}.log.txt"` } });
  }
  return jsonRes(meta);
}

const ADMIN_HTML = `<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PocketGPU ログ</title>
<style>
:root{--bg:#fbfbfa;--fg:#1c1c1a;--mut:#6b6b66;--line:#dcdcd6;--row:#f1f1ee;--acc:#1f4fcf}
@media(prefers-color-scheme:dark){:root{--bg:#161615;--fg:#e9e9e6;--mut:#9b9b95;--line:#35352f;--row:#202020;--acc:#86a8ff}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.55 system-ui,"Hiragino Sans","Yu Gothic UI",sans-serif}
main{max-width:980px;margin:0 auto;padding:16px}
h1{font-size:1.15rem;margin:0 0 4px}
.sum{color:var(--mut);font-size:.88rem;margin-bottom:12px}
form{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:12px}
select,input,button{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:7px 9px;min-height:40px}
form>*{flex:1 1 140px;min-width:0}
button{cursor:pointer}
a{color:var(--acc)}
ul{list-style:none;margin:0;padding:0}
li button.row{display:grid;grid-template-columns:9.5em 1fr auto;gap:2px 12px;width:100%;text-align:left;border:0;border-bottom:1px solid var(--line);border-radius:0;padding:10px 4px;background:none}
li button.row:hover,li button.row[aria-pressed=true]{background:var(--row)}
.t{color:var(--mut);font-size:.88rem}.n{font-weight:600}.m{color:var(--mut);font-size:.88rem;text-align:right}
.sub{grid-column:2/4;color:var(--mut);font-size:.88rem}
@media(max-width:560px){li button.row{grid-template-columns:1fr auto}.t{grid-column:1/3}.sub{grid-column:1/3}}
#detail{margin-top:16px;border-top:2px solid var(--line);padding-top:12px}
#detail[hidden]{display:none}
pre{white-space:pre-wrap;word-break:break-all;background:var(--row);border:1px solid var(--line);border-radius:6px;padding:10px;font:12.5px/1.5 ui-monospace,Consolas,monospace;max-height:70vh;overflow:auto}
.act{display:flex;flex-wrap:wrap;gap:12px;margin:8px 0}
</style>
</head>
<body>
<main>
<h1>PocketGPU ログ</h1>
<div class="sum" id="sum">読み込み中…</div>
<form id="f">
<select id="name" aria-label="人"><option value="">全員</option></select>
<select id="kind" aria-label="種類"><option value="">pc と iPhone</option><option value="pc">pc</option><option value="iphone">iPhone</option></select>
<select id="trigger" aria-label="送り方"><option value="">手動と自動</option><option value="manual">手動</option><option value="auto">自動</option></select>
<input id="ver" type="search" placeholder="アプリの版" aria-label="アプリの版">
</form>
<ul id="list"></ul>
<p><button id="more" hidden>さらに読む</button></p>
<section id="detail" hidden>
<div class="t" id="dmeta"></div>
<div class="act"><a id="dgz" href="#">gzip のままダウンロード</a><a id="dtx" href="#">テキストでダウンロード</a></div>
<pre id="dpre"></pre>
</section>
</main>
<script>
const $ = s => document.querySelector(s);
let last = 0, shown = '';
const kb = n => n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(2) + ' MB';
const when = ms => new Date(ms).toLocaleString('ja-JP', { month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit' });
function query() {
  const p = new URLSearchParams();
  for (const k of ['name', 'kind', 'trigger', 'ver']) { const v = $('#' + k).value.trim(); if (v) p.set(k, v); }
  return p;
}
async function load(more) {
  const p = query();
  p.set('limit', '50');
  if (more && last) p.set('before', last);
  const r = await fetch('/admin/api/logs?' + p);
  if (!r.ok) { $('#sum').textContent = '読み込めませんでした（' + r.status + '）'; return; }
  const j = await r.json();
  if (!more) $('#list').textContent = '';
  for (const l of j.logs) {
    const li = document.createElement('li'), b = document.createElement('button');
    b.className = 'row'; b.type = 'button';
    for (const [c, t] of [['t', when(l.at)], ['n', l.name || l.kid], ['m', kb(l.size)], ['sub', (l.kind === 'pc' ? 'pc' : 'iPhone') + '・' + (l.trigger === 'auto' ? '自動' : '手動') + '・版 ' + l.ver]]) {
      const s = document.createElement('span'); s.className = c; s.textContent = t; b.appendChild(s);
    }
    b.onclick = () => { for (const x of document.querySelectorAll('button.row')) x.removeAttribute('aria-pressed'); b.setAttribute('aria-pressed', 'true'); history.replaceState(null, '', '#' + l.id); show(l); };
    li.appendChild(b); $('#list').appendChild(li);
    last = l.id;
  }
  $('#more').hidden = j.logs.length < 50;
  $('#sum').textContent = '全部で ' + j.count + ' 件・' + kb(j.bytes) + '（' + j.keepDays + ' 日たった物は消えます）' + (j.logs.length ? '' : '・該当なし');
  if (!more) {
    const sel = $('#name'), cur = sel.value;
    sel.length = 1;
    for (const x of j.people) { const o = document.createElement('option'); o.value = x.kid; o.textContent = (x.name || x.kid) + '（' + x.n + '）'; sel.appendChild(o); }
    sel.value = cur;
  }
}
async function show(l) {
  $('#detail').hidden = false;
  $('#dmeta').textContent = (l.name || l.kid) + '・' + new Date(l.at).toLocaleString('ja-JP') + '・' + l.kind + '・' + l.trigger + '・版 ' + l.ver + '・' + kb(l.size);
  $('#dgz').href = '/admin/api/logs/' + l.id + '?raw=1';
  $('#dtx').href = '/admin/api/logs/' + l.id + '?text=1&dl=1';
  const pre = $('#dpre');
  pre.textContent = '読み込み中…';
  shown = l.id;
  try {
    let text;
    if ('DecompressionStream' in window) {
      const r = await fetch('/admin/api/logs/' + l.id + '?raw=1');
      text = await new Response(r.body.pipeThrough(new DecompressionStream('gzip'))).text();
    } else {
      text = await (await fetch('/admin/api/logs/' + l.id + '?text=1')).text();
    }
    if (shown !== l.id) return;
    pre.textContent = text.length > 3e6 ? text.slice(0, 3e6) + '\\n…（先頭の 3 MB だけ表示しています。全体はダウンロードしてください）' : text;
  } catch (e) {
    pre.textContent = 'ほどけませんでした。gzip が壊れているかもしれません。元のファイルをダウンロードしてください。';
  }
  $('#detail').scrollIntoView({ block: 'nearest' });
}
for (const k of ['name', 'kind', 'trigger']) $('#' + k).onchange = () => load(false);
$('#ver').oninput = () => { clearTimeout($('#ver').t); $('#ver').t = setTimeout(() => load(false), 300); };
$('#f').onsubmit = e => { e.preventDefault(); load(false); };
$('#more').onclick = () => load(true);
load(false);
const open = location.hash.match(/^#(\\d{1,12})$/);
if (open) fetch('/admin/api/logs/' + open[1]).then(r => r.ok ? r.json() : null).then(l => { if (l) show(l); });
</script>
</body>
</html>`;

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    const base = url.origin;
    const isGet = req.method === 'GET' || req.method === 'HEAD';
    if (url.pathname === '/log') return receiveLog(req, env);
    if (url.pathname === '/admin' || url.pathname.startsWith('/admin/')) return admin(req, env, url);
    // /dl/ と /u/ の下で合言葉の形が合わないものは 404（今までは下の「PocketGPU kit」の 200 になっていた）
    if (isGet && url.pathname.startsWith('/dl/')) {
      const m = url.pathname.match(/^\/dl\/([A-Za-z0-9_-]{16,64})$/);
      return m ? download(m[1], req, env, ctx) : notFound();
    }
    if (isGet && url.pathname.startsWith('/u/')) {
      const um = url.pathname.match(U_PATH);
      return um ? updates(um, req, env, ctx, base) : notFound();
    }
    if (url.pathname === '/icon.png' && isGet)
      return new Response(Uint8Array.from(atob(ICON_PNG), c => c.charCodeAt(0)), { headers: { 'Content-Type': 'image/png', 'Cache-Control': 'public, max-age=86400' } });
    if (req.method !== 'POST') return new Response('PocketGPU kit', { status: 200 });
    const body = await req.text();
    if (!(await verified(req, body, env.DISCORD_PUBLIC_KEY))) return new Response('bad signature', { status: 401 });
    const i = JSON.parse(body);
    if (i.type === 1) return Response.json({ type: 1 });
    if (i.type === 2 && i.data?.name === 'kit') return kit(i, env, base);
    return reply('Unknown command. / 不明なコマンドです。');
  },
};
