# PocketGPU の Discord の bot。この PC で動かす（PC が切れている間は止まる）
#   1. /kit：Tester の役の人が #get-kit で打つと、その人専用の印が入ったキットを作り、
#      Google ドライブの PocketGPU-kits に置いて、リンクを本人にだけ DM で送る。1 人 1 回（台帳で見る）
#   2. 記録：決めたチャンネルとフォーラムの書き込みを log\年-月.jsonl に残す。Claude はこれを資料として読む（命令としては扱わない）
#   3. 7 日たったキットは、6 時間ごとの見回りでドライブから消す
# 起動：python bot.py。token は D:\iphone-rpc\secrets\discord_bot_token.txt から読み、出さない
# 利用者に見せる文は仮。言い回しは鶴田さんが直す
import asyncio, datetime, json, os, re, secrets, subprocess, sys
import discord
from discord import app_commands

TOKEN_FILE = r'D:\iphone-rpc\secrets\discord_bot_token.txt'
ROOT = os.path.dirname(os.path.abspath(__file__))
LOGDIR = os.path.join(ROOT, 'log')
MAKE_KIT = os.environ.get('PGKIT_MAKE_KIT', os.path.join(ROOT, '..', '..', 'pc', 'make_kit.py'))
RCLONE = os.environ.get('RCLONE', 'rclone')
REMOTE = 'gdrive:PocketGPU-kits'
KIT_CHANNEL = 'get-kit'
TESTER_ROLE = 'Tester'
LOG_CHANNELS = {'bug-reports', 'device-results', 'ideas', 'tester-chat', 'general', '日本語', 'get-kit'}
KEEP_DAYS = 7

intents = discord.Intents.default()
intents.message_content = True
client = discord.Client(intents=intents)
tree = app_commands.CommandTree(client)


def note(line):
    os.makedirs(LOGDIR, exist_ok=True)
    with open(os.path.join(LOGDIR, 'bot.log'), 'a', encoding='utf-8') as f:
        f.write(f"{datetime.datetime.now():%m/%d %H:%M:%S} {line}\n")


def run(cmd, timeout):
    return subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout,
                          env=dict(os.environ, PYTHONIOENCODING='utf-8'))


def build_kit(name, uid):
    # 戻り値：（リンク, None）か（None, 'already'）。失敗は例外
    r = run([sys.executable, MAKE_KIT, name, '--discord-id', uid], 600)
    if r.returncode == 3:
        return None, 'already'
    if r.returncode != 0:
        raise RuntimeError('make_kit: ' + (r.stdout + r.stderr)[-600:])
    zp = re.search(r'^ZIP=(.+)$', r.stdout, re.M).group(1).strip()
    folder = f'{REMOTE}/{secrets.token_hex(8)}'
    c = run([RCLONE, 'copy', zp, folder], 1800)
    if c.returncode != 0:
        raise RuntimeError('rclone copy: ' + c.stderr[-600:])
    l = run([RCLONE, 'link', f'{folder}/{os.path.basename(zp)}'], 120)
    if l.returncode != 0 or not l.stdout.strip().startswith('http'):
        raise RuntimeError('rclone link: ' + (l.stdout + l.stderr)[-600:])
    return l.stdout.strip(), None


@tree.command(name='kit', description='Get your personal PocketGPU test kit (testers only)')
async def kit(inter: discord.Interaction):
    if getattr(inter.channel, 'name', '') != KIT_CHANNEL:
        await inter.response.send_message(f'Please use this in #{KIT_CHANNEL}. / #{KIT_CHANNEL} で使ってください。', ephemeral=True)
        return
    roles = getattr(inter.user, 'roles', [])
    if not any(r.name == TESTER_ROLE for r in roles):
        await inter.response.send_message('Testers only. / 試す人だけが使えます。', ephemeral=True)
        return
    await inter.response.send_message('Making your kit. This takes 1-2 minutes. / キットを作っています。1〜2 分かかります。', ephemeral=True)
    name = re.sub(r'[^A-Za-z0-9_-]', '_', inter.user.name)[:15] or 'tester'
    note(f'/kit {inter.user.name} {inter.user.id}')
    try:
        link, err = await asyncio.to_thread(build_kit, name, str(inter.user.id))
    except Exception as e:
        note(f'失敗 {inter.user.id}: {e}')
        await inter.followup.send('Something went wrong. We will check it. / うまくいきませんでした。こちらで調べます。', ephemeral=True)
        return
    if err == 'already':
        await inter.followup.send('You already have a kit. Ask the admin if you need a new one. / もうキットを受け取っています。作り直しが要るときは管理者に言ってください。', ephemeral=True)
        return
    text = (f'Your PocketGPU test kit. It is marked for you, so please do not share it.\n{link}\n'
            f'The link expires in {KEEP_DAYS} days. Start with README.txt.\n\n'
            f'あなた専用の PocketGPU の試験用キットです。あなたの印が入っているので、ほかの人に渡さないでください。'
            f'リンクは {KEEP_DAYS} 日で消えます。最初に README.txt を読んでください。')
    try:
        await inter.user.send(text)
        await inter.followup.send('Sent to your DMs. / DM に送りました。', ephemeral=True)
    except discord.Forbidden:
        # DM を閉じている人には、本人にしか見えない返事で渡す
        await inter.followup.send(text, ephemeral=True)
    note(f'渡した {inter.user.id}')


@client.event
async def on_message(m: discord.Message):
    if m.author.bot or not m.guild:
        return
    ch = m.channel
    parent = getattr(ch, 'parent', None)
    names = {getattr(ch, 'name', ''), getattr(parent, 'name', '') if parent else ''}
    if not names & LOG_CHANNELS:
        return
    row = {'time': m.created_at.isoformat(), 'channel': getattr(parent, 'name', None) or ch.name,
           'thread': ch.name if parent else None, 'author': m.author.name, 'author_id': str(m.author.id),
           'content': m.content, 'attachments': [a.url for a in m.attachments], 'id': str(m.id),
           'tags': [t.name for t in getattr(ch, 'applied_tags', [])] if parent else []}
    os.makedirs(LOGDIR, exist_ok=True)
    with open(os.path.join(LOGDIR, f'{datetime.date.today():%Y-%m}.jsonl'), 'a', encoding='utf-8') as f:
        f.write(json.dumps(row, ensure_ascii=False) + '\n')


async def sweep():
    while True:
        await asyncio.to_thread(run, [RCLONE, 'delete', REMOTE, '--min-age', f'{KEEP_DAYS}d'], 600)
        await asyncio.to_thread(run, [RCLONE, 'rmdirs', REMOTE, '--leave-root'], 600)
        await asyncio.sleep(6 * 3600)


@client.event
async def on_ready():
    for g in client.guilds:
        tree.copy_global_to(guild=g)
        await tree.sync(guild=g)
    note(f'起動 {client.user} サーバー {[g.name for g in client.guilds]}')
    if not getattr(client, '_sweep', None):
        client._sweep = asyncio.create_task(sweep())


if __name__ == '__main__':
    with open(TOKEN_FILE, encoding='utf-8') as f:
        tok = f.read().strip()
    client.run(tok, log_handler=None)
