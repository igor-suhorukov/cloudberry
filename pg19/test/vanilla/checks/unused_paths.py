#-------------------------------------------------------------------------
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
#-------------------------------------------------------------------------
"""Read the core patch series and classify every line it adds.

The rule the series is written to is that an unused hook leaves the code path
exactly as it was.  This asks, of each statement added to a function that
already existed, whether reaching it depends on a name the series introduces.
What it cannot account for is printed, and that is what needs reading by hand.

It is a reading aid rather than a proof: it understands C well enough to tell a
guarded block from a rewrapped line, and no better.

    PG_REPO=... PG_BASE=... PG_HEAD=... python3 unused_paths.py
"""

import os
import re
import subprocess
import sys

REPO = os.environ["PG_REPO"]
BASE = os.environ.get("PG_BASE", "origin/REL_19_STABLE")
HEAD = os.environ.get("PG_HEAD", "REL_19_STABLE_CLOUDBERRY")

DIRS = ["src/backend", "src/bin", "src/common"]

ALLOW_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "unused_paths.allow")


def read_allowlist():
    """Statements read by hand, with the reading; see unused_paths.allow."""
    allowed = {}
    if not os.path.exists(ALLOW_FILE):
        return allowed
    key, reason = None, []
    for raw in open(ALLOW_FILE):
        if raw.lstrip().startswith("#") or not raw.strip():
            continue
        if raw[0].isspace():                    # continuation of a reason
            if key:
                reason.append(raw.strip())
            continue
        if key:
            allowed[key] = " ".join(reason)
        path, fn, first = (x.strip() for x in raw.split(":", 2))
        key, reason = (path, fn), [first]
    if key:
        allowed[key] = " ".join(reason)
    return allowed


def git(*args):
    return subprocess.run(["git", "-C", REPO, *args],
                          capture_output=True, text=True, check=True).stdout


def introduced_names():
    """The names the series adds: hooks, flags, functions, callback types."""
    names = set()
    for line in git("diff", f"{BASE}..{HEAD}", "--", "src/include").splitlines():
        if not line.startswith("+"):
            continue
        for m in re.finditer(
                r"\bextern\s+(?:PGDLLIMPORT\s+)?[\w \t\*]*?(\w+)\s*[;(\[]", line):
            names.add(m.group(1))
        for m in re.finditer(r"\btypedef\s+.*?\(\*(\w+)\)", line):
            names.add(m.group(1))
    # File-scope definitions: the hook variables and the statics beside them.
    for line in git("diff", f"{BASE}..{HEAD}", "--", *DIRS).splitlines():
        m = re.match(r"\+(?:static\s+)?[\w \t\*]+?(\w+)\s*=\s*"
                     r"(?:NULL|false|0)\s*;\s*$", line)
        if m:
            names.add(m.group(1))
    names.discard("")
    return names


def hunks_of(sha):
    """Added and removed lines per hunk, with the enclosing function."""
    diff = git("show", sha, "-U3", "--format=", "--", *DIRS)
    out, path, cur = [], None, None
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            path = line[6:]
        elif line.startswith("@@"):
            m = re.match(r"@@ [^@]*@@ ?(.*)", line)
            cur = {"path": path,
                   "fn": (m.group(1).strip() if m else "") or "(file scope)",
                   "add": [], "del": []}
            out.append(cur)
        elif cur is None:
            continue
        elif line.startswith("+") and not line.startswith("+++"):
            cur["add"].append(line[1:])
        elif line.startswith("-") and not line.startswith("---"):
            cur["del"].append(line[1:])
    return out


NORM = lambda t: re.sub(r"\s+", "", t)
UNELSE = lambda t: re.sub(r"^\s*else\s+", "", t)

TYPE_ONLY = re.compile(r"^[A-Za-z_][\w \t]*\*?$")
FUNC_NAME = re.compile(r"^\w+\(")
CONTROL = re.compile(r"^\s*(?:\}\s*)?(?:else\s+)?(?:if|while|for)\b")
COMMENT = re.compile(r"^(/\*|\*|//)")
DECL = re.compile(r"^(?:const |static |unsigned |struct )*[\w][\w \t\*]*\s\*?\w+"
                  r"(\s*\[[^\]]*\])?\s*(=\s*[^;]*)?;$")


def complete(stmt):
    """Has this accumulated text become a whole statement?"""
    return (stmt.count("(") == stmt.count(")")
            and stmt.rstrip().endswith((";", "{", "}")))


def classify(hunk, guard_re, counts, unexplained, sha):
    lines = hunk["add"]
    removed = {NORM(d) for d in hunk["del"]}
    removed |= {NORM(UNELSE(d)) for d in hunk["del"]}

    in_func = False     # inside a function the series adds
    depth = 0
    guard_depth = None  # inside a block a guarded condition controls
    i = 0

    while i < len(lines):
        text = lines[i]
        stripped = text.strip()

        # A function the series adds, whose return type may be on its own line.
        if not in_func and TYPE_ONLY.match(stripped) and i + 1 < len(lines) \
                and FUNC_NAME.match(lines[i + 1].strip()):
            in_func, depth = True, 0

        if in_func:
            depth += text.count("{") - text.count("}")
            counts["new function"] += 1
            if depth <= 0 and "}" in text:
                in_func = False
            i += 1
            continue

        if guard_depth is not None:
            counts["inside a guarded block"] += 1
            guard_depth += text.count("{") - text.count("}")
            if guard_depth <= 0 and (";" in text or "}" in text):
                guard_depth = None
            i += 1
            continue

        if not stripped or COMMENT.match(stripped) or stripped.startswith("#"):
            counts["comment or blank"] += 1
            i += 1
            continue

        # Classify a whole statement rather than a line: a condition can name
        # the hook that gates it on its second line, as R3's does.
        j, stmt = i, text
        while j + 1 < len(lines) and not complete(stmt):
            j += 1
            stmt += " " + lines[j]
        span = lines[i:j + 1]
        n = len(span)

        if all(NORM(l) in removed or NORM(UNELSE(l)) in removed for l in span):
            counts["rewrapped existing code"] += n
        elif guard_re and guard_re.search(stmt):
            counts["guarded"] += n
            if CONTROL.match(stripped) and not stmt.rstrip().endswith(";"):
                guard_depth = max(stmt.count("{") - stmt.count("}"), 1)
        elif stripped in ("{", "}", "};") or DECL.match(stripped):
            counts["declaration or structure"] += n
        else:
            counts["unexplained"] += n
            unexplained.append((sha[:11], hunk["path"], hunk["fn"], span))
        i = j + 1


def main():
    names = introduced_names()
    guard_re = re.compile(r"\b(" + "|".join(re.escape(n) for n in sorted(names))
                          + r")\b") if names else None

    counts = dict.fromkeys(
        ["guarded", "inside a guarded block", "new function", "comment or blank",
         "declaration or structure", "rewrapped existing code", "unexplained"], 0)
    unexplained = []

    commits = git("log", "--format=%H %s", "--reverse",
                  f"{BASE}..{HEAD}").splitlines()
    for entry in commits:
        sha = entry.split(" ", 1)[0]
        for hunk in hunks_of(sha):
            if hunk["path"] and hunk["path"].endswith(".c"):
                classify(hunk, guard_re, counts, unexplained, sha)

    print(f"series {BASE}..{HEAD}: {len(commits)} commits, "
          f"{len(names)} names introduced")
    print("  added lines in files that already existed, by kind:")
    for k, v in counts.items():
        print(f"    {k:26} {v}")
    print()

    allowed = read_allowlist()

    def allow_reason(path, fn):
        name = fn.split("(")[0].strip()
        return allowed.get((path, name))

    accounted = [u for u in unexplained if allow_reason(u[1], u[2])]
    remaining = [u for u in unexplained if not allow_reason(u[1], u[2])]

    # Printed every run, never silently: a suppression nobody sees is worse
    # than no check.
    if accounted:
        print("read by hand, recorded in unused_paths.allow:")
        seen = set()
        for sha, path, fn, span in accounted:
            if (path, fn) in seen:
                continue
            seen.add((path, fn))
            print(f"  {path}  {fn}")
            print(f"      {allow_reason(path, fn)}")
        print()

    if remaining:
        print("added statements not gated by a name the series introduces:")
        for sha, path, fn, span in remaining:
            print(f"  {sha}  {path}  {fn}")
            for line in span:
                print(f"      {line.rstrip()}")
    return 1 if remaining else 0


if __name__ == "__main__":
    sys.exit(main())
