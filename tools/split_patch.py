#!/usr/bin/env python3
"""split_patch.py <mesa git tree> <base tag> <out dir>
The working tree carries the full xess-xmx change set with the debug-only parts between
/* XMX-DEBUG-BEGIN */ and /* XMX-DEBUG-END */ lines. Writes two patches:
  0001-anv-cm-kernel-injection.patch  base -> core (debug blocks removed)
  0002-anv-cm-debug-tools.patch       core -> full (markers removed)
and leaves the tree in the 'full' state without markers (so it still builds the same library)."""
import os, re, subprocess, sys

tree, base, outdir = sys.argv[1:4]
os.makedirs(outdir, exist_ok=True)
files = subprocess.run(["git", "-C", tree, "diff", "--name-only", base], capture_output=True, text=True, check=True).stdout.split()
BEGIN = re.compile(r"^\s*/\* XMX-DEBUG-BEGIN \*/\s*$")
END = re.compile(r"^\s*/\* XMX-DEBUG-END \*/\s*$")


def variant(text, keep_debug):
    out, inside = [], False
    for line in text.splitlines(keepends=True):
        if BEGIN.match(line):
            inside = True
            continue
        if END.match(line):
            inside = False
            continue
        if inside and not keep_debug:
            continue
        out.append(line)
    assert not inside, "unterminated debug block"
    return "".join(out)


orig = {f: open(os.path.join(tree, f), encoding="utf-8").read() for f in files}
env = dict(os.environ, GIT_AUTHOR_NAME="xess-xmx-linux", GIT_AUTHOR_EMAIL="xmx@localhost",
           GIT_COMMITTER_NAME="xess-xmx-linux", GIT_COMMITTER_EMAIL="xmx@localhost")
git = lambda *a: subprocess.run(["git", "-C", tree, *a], check=True, capture_output=True, text=True, env=env).stdout

cur = git("rev-parse", "--abbrev-ref", "HEAD").strip()
git("checkout", "-q", "-B", "xmx-split", base)
for f in files:
    open(os.path.join(tree, f), "w", encoding="utf-8").write(variant(orig[f], False))
git("add", *files)
git("commit", "-q", "-m", "anv: native C-for-Metal kernel injection for XeSS (xess-xmx-linux)")
for f in files:
    open(os.path.join(tree, f), "w", encoding="utf-8").write(variant(orig[f], True))
git("add", *files)
git("commit", "-q", "-m", "anv: debug tools for the C-for-Metal kernel injection (xess-xmx-linux)")
git("format-patch", "-q", "-2", "--no-signature", "-o", outdir)
for p in sorted(os.listdir(outdir)):
    print(p)
