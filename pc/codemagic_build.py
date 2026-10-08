# Codemagic で iPhone の受け手を組み、IPA を取ってきて配り口（D:\iphone-rpc\ipa\serve）に置く。
#   python codemagic_build.py            master を組む
#   python codemagic_build.py --fetch ID 組み終わった ID の IPA を取ってくるだけ
# トークンは D:\iphone-rpc\secrets\codemagic_token.txt（リポジトリの外）。記録には出さない
import argparse, json, os, sys, time, urllib.request, zipfile, plistlib, shutil

API = "https://api.codemagic.io"
APP = "6ac774f493969192923e8eaa"   # iphone-rpc
WORKFLOW = "ios-app"               # codemagic.yaml の workflows の名前
TOKEN_FILE = r"D:\iphone-rpc\secrets\codemagic_token.txt"
IPA_DIR = r"D:\iphone-rpc\ipa"


def token():
    return open(TOKEN_FILE, encoding="ascii").read().strip()


def call(method, path, body=None):
    req = urllib.request.Request(API + path, method=method, headers={"x-auth-token": token(), "Content-Type": "application/json"},
                                 data=json.dumps(body).encode() if body is not None else None)
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode() or "{}")


def wait(bid):
    t0 = time.time()
    last = None
    while True:
        b = call("GET", f"/builds/{bid}").get("build", {})
        st = b.get("status")
        if st != last:
            print(f"{time.strftime('%H:%M:%S')} {st}", flush=True)
            last = st
        if st in ("finished", "failed", "canceled", "timeout", "skipped"):
            return b
        if time.time() - t0 > 3600:
            print("1 時間待っても終わらない")
            return b
        time.sleep(15)


def fetch(b):
    arts = [a for a in b.get("artefacts", []) if a.get("name", "").endswith(".ipa")]
    if not arts:
        print("IPA が無い")
        return 1
    tmp = os.path.join(IPA_DIR, "codemagic_tmp.ipa")
    req = urllib.request.Request(arts[0]["url"], headers={"x-auth-token": token()})
    with urllib.request.urlopen(req, timeout=300) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    z = zipfile.ZipFile(tmp)
    info = plistlib.loads(z.read([n for n in z.namelist() if n.endswith(".app/Info.plist")][0]))
    z.close()
    ver, num = info.get("CFBundleShortVersionString"), info.get("CFBundleVersion")
    d = os.path.join(IPA_DIR, "dl" + ver.replace(".", ""))  # 0.7.29 → dl0729（Actions のときと同じ名前）
    os.makedirs(d, exist_ok=True)
    name = f"RpcServer-{ver}.ipa"
    shutil.move(tmp, os.path.join(d, name))
    shutil.copy(os.path.join(d, name), os.path.join(IPA_DIR, "serve", name))
    print(f"{ver}（{num}）を {d} と配り口に置いた：http://127.0.0.1:8766/{name}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", help="組み終わった build の ID")
    ap.add_argument("--branch", default="master")
    a = ap.parse_args()
    if a.fetch:
        return fetch(call("GET", f"/builds/{a.fetch}").get("build", {}))
    r = call("POST", "/builds", {"appId": APP, "workflowId": WORKFLOW, "branch": a.branch})
    bid = r.get("buildId")
    print("build", bid, flush=True)
    if not bid:
        print(r)
        return 1
    b = wait(bid)
    if b.get("status") != "finished":
        print("組めなかった：", b.get("status"))
        return 1
    return fetch(b)


if __name__ == "__main__":
    sys.exit(main())
