"""Pick the provisioning profile for a bundle ID and derive signing entitlements from it.

usage: pick_profile.py <bundle-id> <wanted.entitlements> <outdir>
Writes outdir/profile.mobileprovision, outdir/entitlements.plist, outdir/identity.txt (SHA-1 of a
keychain identity whose certificate is in the profile). Fails with a hint if nothing matches.
"""
import datetime
import hashlib
import pathlib
import plistlib
import shutil
import subprocess
import sys

bundle_id, wanted_path, outdir = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
wanted = plistlib.loads(wanted_path.read_bytes())
cache = outdir.parent.parent / "profiles"  # build-ios/profiles: copies of profiles used before
dirs = [pathlib.Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles",
        pathlib.Path.home() / "Library/MobileDevice/Provisioning Profiles", cache]
# Xcode replaces a free team's profile when another device (e.g. this Mac) is provisioned, so prefer
# profiles that list a device other than this Mac; BOTW_DEVICE_UDID pins a specific phone.
hw = subprocess.run(["system_profiler", "SPHardwareDataType"], capture_output=True, text=True).stdout
mac_udid = next((l.split(":", 1)[1].strip() for l in hw.splitlines() if "Provisioning UDID" in l), "")
want_udid = __import__("os").environ.get("BOTW_DEVICE_UDID", "")
now = datetime.datetime.now(datetime.timezone.utc)
idents = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True,
                        text=True).stdout
best = None
for d in dirs:
    for f in sorted(d.glob("*.mobileprovision")) if d.is_dir() else []:
        raw = subprocess.run(["security", "cms", "-D", "-i", str(f)], capture_output=True).stdout
        if not raw:
            continue
        p = plistlib.loads(raw)
        ent = p.get("Entitlements", {})
        team = (p.get("TeamIdentifier") or [""])[0]
        if ent.get("application-identifier") not in (f"{team}.{bundle_id}", f"{team}.*"):
            continue
        exp = p["ExpirationDate"].replace(tzinfo=datetime.timezone.utc)
        if exp <= now:
            continue
        certs = [hashlib.sha1(c).hexdigest().upper() for c in p.get("DeveloperCertificates", [])]
        ident = next((c for c in certs if c in idents), None)
        if not ident:
            continue
        devs = p.get("ProvisionedDevices", [])
        if want_udid and want_udid not in devs and not p.get("ProvisionsAllDevices"):
            continue
        phone = any(d != mac_udid for d in devs) or bool(p.get("ProvisionsAllDevices"))
        key = (phone, exp)
        if best is None or key > best[0]:
            best = (key, f, p, ident, team)
if best is None:
    sys.exit(f"no unexpired profile for {bundle_id} with a usable identity; run ios/scripts/provision.sh")
(_, exp), f, p, ident, team = best
cache.mkdir(parents=True, exist_ok=True)
if f.parent != cache:
    shutil.copy(f, cache / f.name)
grant = p["Entitlements"]
ent = {"application-identifier": f"{team}.{bundle_id}", "com.apple.developer.team-identifier": team,
       "get-task-allow": bool(grant.get("get-task-allow", False))}
missing = []
for k, v in wanted.items():
    if k in ent:
        continue
    if k in grant:
        ent[k] = v
    else:
        missing.append(k)
outdir.mkdir(parents=True, exist_ok=True)
shutil.copy(f, outdir / "profile.mobileprovision")
(outdir / "entitlements.plist").write_bytes(plistlib.dumps(ent))
(outdir / "identity.txt").write_text(ident)
print(f"profile {p.get('Name')} ({f.name}), expires {exp:%Y-%m-%d}, devices {p.get('ProvisionedDevices', [])}")
if missing:
    print("WARNING: profile does not grant " + ", ".join(missing) + " (signed without them)")
