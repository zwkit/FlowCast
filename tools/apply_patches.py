#!/usr/bin/env python
"""Apply the airplay-server UxPlay patches to the UxPlay source tree.

Usage: python apply_patches.py <patches_dir> <uxplay_root>

Requires the third-party `patch` module (not in the stdlib):
    pip install patch

Note: the UxPlay tree under app/src/main/cpp/third_party/UxPlay is vendored
with all patches in cpp/patches/UxPlay already applied, so this script is only
needed when re-creating the tree from a pristine upstream checkout.
"""
import os, sys, glob
import patch

def main():
    patches_dir, uxplay_root = sys.argv[1], sys.argv[2]
    patches = sorted(glob.glob(os.path.join(patches_dir, "*.patch")))
    if not patches:
        print("No patches found in", patches_dir)
        sys.exit(2)
    os.chdir(uxplay_root)
    ok_all = True
    for pf in patches:
        p = patch.fromfile(pf)
        if p is None:
            print("PARSE FAIL:", os.path.basename(pf))
            ok_all = False
            continue
        print("=== Applying", os.path.basename(pf))
        try:
            res = p.apply()
            print("    result:", res)
            if not res:
                ok_all = False
        except Exception as e:
            print("    ERROR:", e)
            ok_all = False
    print("ALL_OK" if ok_all else "HAS_FAILURES")
    sys.exit(0 if ok_all else 1)

if __name__ == "__main__":
    main()
