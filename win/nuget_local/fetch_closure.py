# fetch_wasdks.py — download the full NuGet closure for the WinUI3 app via
# nuget.azure.cn (the China CDN api.nuget.org redirects to) into a local
# folder source (win/nuget_local). Handles: big files, retries, resume.
import json
import os
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET

BASE = "https://nuget.azure.cn/v3-flatcontainer"
OUT = os.path.dirname(os.path.abspath(__file__))

ROOTS = [
    ("Microsoft.WindowsAppSDK", "1.8.260921001"),
    ("Microsoft.Windows.SDK.BuildTools", "10.0.26100.1742"),
]

NS = {"n": "http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd"}

def fetch(url, path, tries=8):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return True
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "curl/8"})
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
            with open(path + ".tmp", "wb") as f:
                f.write(data)
            os.replace(path + ".tmp", path)
            return True
        except Exception as e:
            print(f"  retry {i+1}/{tries} for {os.path.basename(path)}: {e}")
            time.sleep(2 + i * 2)
    return False

def nuspec(pid, ver):
    url = f"{BASE}/{pid.lower()}/{ver}/{pid.lower()}.nuspec"
    path = os.path.join(OUT, "_specs", f"{pid.lower()}.{ver}.nuspec")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not fetch(url, path):
        return None
    try:
        return ET.parse(path).getroot()
    except Exception:
        return None

def deps_of(pid, ver):
    root = nuspec(pid, ver)
    if root is None:
        return []
    out = []
    for g in root.findall(".//n:group", NS):
        tfm = (g.get("targetFramework") or "").lower()
        if "windows" in tfm or tfm in ("", "net8.0", "net6.0", "net8.0-windows7.0", "net8.0-windows10.0.17763"):
            for d in g.findall("n:dependency", NS):
                out.append((d.get("id"), d.get("version")))
    # fall back: if nothing matched the filter, take every group
    if not out:
        for d in root.findall(".//n:dependency", NS):
            out.append((d.get("id"), d.get("version")))
    return out

def resolve_ver(pid, range_str):
    if not range_str:
        return None
    r = range_str.strip("[]() ")
    if "," in r:
        lo = r.split(",")[0].strip()
        return lo if lo else None
    return r or None

def ver_exists(pid, ver):
    url = f"{BASE}/{pid.lower()}/index.json"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "curl/8"}), timeout=30) as r:
            versions = json.load(r)["versions"]
        return ver in versions
    except Exception:
        return False

def latest_ver(pid):
    url = f"{BASE}/{pid.lower()}/index.json"
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "curl/8"}), timeout=30) as r:
        versions = json.load(r)["versions"]
    return versions[-1]

seen = set()
queue = list(ROOTS)
plan = []
while queue:
    pid, ver = queue.pop(0)
    key = (pid.lower(), ver)
    if key in seen:
        continue
    seen.add(key)
    plan.append((pid, ver))
    print(f"plan: {pid} {ver}")
    for dep_id, dep_ver in deps_of(pid, ver):
        if dep_id.lower().startswith("system.") or dep_id.lower().startswith("microsoft.netcore") or dep_id in ("NETStandard.Library",):
            continue  # BCL packages come from the installed SDK
        v = resolve_ver(dep_id, dep_ver)
        if v is None:
            try:
                v = latest_ver(dep_id)
            except Exception:
                print(f"  !! cannot resolve {dep_id} {dep_ver}")
                continue
        if not ver_exists(dep_id, v):
            print(f"  !! {dep_id} {v} not on mirror, skipping")
            continue
        queue.append((dep_id, v))

print(f"\ntotal packages: {len(plan)}")
fail = []
for pid, ver in plan:
    url = f"{BASE}/{pid.lower()}/{ver}/{pid.lower()}.{ver}.nupkg"
    path = os.path.join(OUT, f"{pid.lower()}.{ver}.nupkg")
    ok = fetch(url, path)
    sz = os.path.getsize(path) if os.path.exists(path) else 0
    print(f"{'OK ' if ok else 'FAIL'} {pid} {ver} ({sz/1e6:.1f} MB)")
    if not ok:
        fail.append((pid, ver))
print(f"\ndone, failures: {len(fail)}")
sys.exit(1 if fail else 0)
