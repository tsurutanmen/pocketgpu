# Discord のサーバーの中身を、bot に一度にそろえさせる台本。何度走らせても、もうある物は作り直さない
#   python setup_server.py            → 何を作るかを出すだけ（Discord には何も書かない）
#   python setup_server.py --apply    → 本当に作る
# bot の token は D:\iphone-rpc\secrets\discord_bot_token.txt から読み、画面にもログにも出さない
# bot がちょうど 1 つのサーバーに入っている前提（2 つ以上なら --guild で番号を渡す）
import argparse, json, sys, time, urllib.error, urllib.request

TOKEN_FILE = r'D:\iphone-rpc\secrets\discord_bot_token.txt'
API = 'https://discord.com/api/v10'

VIEW, SEND, HISTORY = 1 << 10, 1 << 11, 1 << 16
SEND_THREADS, MAKE_PUBLIC_THREADS, MAKE_PRIVATE_THREADS = 1 << 38, 1 << 35, 1 << 36
NO_TALK = SEND | SEND_THREADS | MAKE_PUBLIC_THREADS | MAKE_PRIVATE_THREADS
TEXT, CATEGORY = 0, 4

# まとまり → チャンネルの並び。read_only はあなたと bot だけが書ける。tester はまとまりごと Tester の役だけに見せる
LAYOUT = [
    ('info', 'はじめに', False, [('welcome', True), ('announcements', True), ('roadmap', True)]),
    ('community', 'みんな', False, [('general', False), ('ideas', False), ('日本語', False)]),
    ('testing', 'Testers', True, [('tester-chat', False), ('bug-reports', False), ('device-results', False)]),
]
TESTER_ROLE = 'Tester'


def token():
    with open(TOKEN_FILE, encoding='utf-8') as f:
        return f.read().strip()


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    while True:
        req = urllib.request.Request(API + path, data=data, method=method, headers={
            'Authorization': 'Bot ' + token(), 'Content-Type': 'application/json',
            'User-Agent': 'DiscordBot (https://tsurutalab.org, 1.0)'})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw = r.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(float(json.loads(e.read()).get('retry_after', 1)) + 0.2)
                continue
            raise SystemExit(f'{method} {path} が {e.code} で失敗：{e.read()[:300]!r}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--guild')
    a = ap.parse_args()

    guilds = call('GET', '/users/@me/guilds')
    if a.guild:
        guild = a.guild
    elif len(guilds) == 1:
        guild = guilds[0]['id']
    else:
        raise SystemExit('bot が入っているサーバーが 1 つではない：' + ', '.join(f"{g['name']}={g['id']}" for g in guilds))
    g = call('GET', f'/guilds/{guild}')
    print(f"サーバー：{g['name']}（{'書き込む' if a.apply else '見るだけ'}）")
    everyone = guild  # @everyone の役の番号はサーバーの番号と同じ

    roles = {r['name']: r['id'] for r in call('GET', f'/guilds/{guild}/roles')}
    if TESTER_ROLE in roles:
        tester = roles[TESTER_ROLE]
        print(f'役 {TESTER_ROLE}：もうある')
    else:
        print(f'役 {TESTER_ROLE}：作る')
        tester = call('POST', f'/guilds/{guild}/roles', {'name': TESTER_ROLE, 'color': 0x5AA9E6, 'hoist': True, 'mentionable': False})['id'] if a.apply else 'new'

    chans = call('GET', f'/guilds/{guild}/channels')
    by_name = {(c['name'], c['type']): c for c in chans}
    for pos, (key, cat_name, tester_only, items) in enumerate(LAYOUT):
        over = [{'id': everyone, 'type': 0, 'allow': '0', 'deny': str(VIEW)},
                {'id': tester, 'type': 0, 'allow': str(VIEW | SEND | HISTORY), 'deny': '0'}] if tester_only else []
        cat = by_name.get((cat_name, CATEGORY))
        if cat:
            print(f'まとまり {cat_name}：もうある')
        else:
            print(f'まとまり {cat_name}：作る' + ('・Tester だけに見せる' if tester_only else ''))
            cat = call('POST', f'/guilds/{guild}/channels', {'name': cat_name, 'type': CATEGORY, 'position': pos,
                                                            'permission_overwrites': over}) if a.apply else {'id': 'new'}
        for i, (name, read_only) in enumerate(items):
            if (name, TEXT) in by_name:
                print(f'  #{name}：もうある')
                continue
            ch_over = list(over)
            if read_only:
                ch_over.append({'id': everyone, 'type': 0, 'allow': '0', 'deny': str(NO_TALK)})
            print(f'  #{name}：作る' + ('・書けるのはあなたと bot だけ' if read_only else ''))
            if a.apply:
                call('POST', f'/guilds/{guild}/channels', {'name': name, 'type': TEXT, 'parent_id': cat['id'], 'position': i,
                                                          'permission_overwrites': ch_over})

    # 安全設定：認証は「中」＝メール確認済みで作って 5 分以上のアカウント・きわどい画像はみんなの分を見る・通知は名前を呼ばれた時だけ
    print('安全設定：認証レベルを中・きわどい画像の仕分けをみんなに・通知を名前を呼ばれた時だけ')
    if a.apply:
        call('PATCH', f'/guilds/{guild}', {'verification_level': 2, 'explicit_content_filter': 2, 'default_message_notifications': 1})

    rules = {r['name'] for r in call('GET', f'/guilds/{guild}/auto-moderation/rules')}
    for name, body in [
        ('Block spam', {'event_type': 1, 'trigger_type': 3, 'actions': [{'type': 1}], 'enabled': True}),
        ('Block mass mentions', {'event_type': 1, 'trigger_type': 5, 'trigger_metadata': {'mention_total_limit': 5},
                                 'actions': [{'type': 1}], 'enabled': True}),
    ]:
        if name in rules:
            print(f'AutoMod {name}：もうある')
            continue
        print(f'AutoMod {name}：作る')
        if a.apply:
            call('POST', f'/guilds/{guild}/auto-moderation/rules', dict(body, name=name))
    print('おわり' if a.apply else '見るだけで終わり。--apply で本当に作る')


if __name__ == '__main__':
    main()
