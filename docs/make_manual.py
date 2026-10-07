#!/usr/bin/env python3
"""Makes docs/manual.html, the manual of the library for C, C++ and Python.

The manual is one file that needs nothing else. It is put together from

    docs/manual.in.html        the text, with marks where the rest goes
    examples/first.*, tour.*   the example programs: the manual shows the lines between their marks
    include/xisfconv.h         the reference of the C API is this header, set as a page
    python/xisfconv            the reference of the Python package is its signatures and docstrings
    docs/manual-output.json    what the example programs printed when they ran on the example frame
    docs/manual-frame.jpg      the picture the tour made of that frame

    python docs/make_manual.py                  writes docs/manual.html      (needs Pygments, and the
                                                package importable with astropy: from a checkout,
                                                PYTHONPATH=python and XISFCONV_LIBRARY=<the library>)
    python docs/make_manual.py --check          says whether docs/manual.html was made from all this as
                                                it is now (needs the package importable; the tests run it)
    python docs/make_manual.py --run FRAME.xisf --solved SOLVED.xisf [--programs build]
                                                runs the example programs on a frame and keeps what
                                                they print (needs Pillow for the picture); then writes
                                                the manual
    python docs/make_manual.py --keep-output    after a change to an example that does not change what
                                                it prints: the manual is not made with output that was
                                                printed by other programs than it shows, unless told so

The example frame itself is not in the repository: what the programs printed is.

SPDX-License-Identifier: LGPL-3.0-or-later
Copyright (C) 2026 Jurgen Kobierczynski
"""
import argparse
import base64
import hashlib
import html
import inspect
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEMPLATE = os.path.join(HERE, "manual.in.html")
OUTPUT = os.path.join(HERE, "manual.html")
RESULTS = os.path.join(HERE, "manual-output.json")
FRAME = os.path.join(HERE, "manual-frame.jpg")
HEADER = os.path.join(ROOT, "include", "xisfconv.h")
EXAMPLES = os.path.join(ROOT, "examples")

LANGUAGES = (("c", "C"), ("cpp", "C++"), ("py", "Python"))
SUFFIX = {"c": ".c", "cpp": ".cpp", "py": ".py"}
CHAPTERS = ("inspect", "pixels", "stretch", "write", "convert", "rewrite", "units", "wcs", "progress")
PYTHON_CHAPTERS = ("astropy", "compat")


def read_text(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def sources():
    """The files the manual is made from, in a fixed order."""
    files = [TEMPLATE, os.path.abspath(__file__), RESULTS, FRAME, HEADER]
    files += [os.path.join(EXAMPLES, name + SUFFIX[lang]) for name in ("first", "tour") for lang, _ in LANGUAGES]
    return files


def example_digests():
    """{file: checksum} of the example programs: what ties the output that is kept to the programs it came from."""
    found = {}
    for name in ("first", "tour"):
        for lang, _ in LANGUAGES:
            with open(os.path.join(EXAMPLES, name + SUFFIX[lang]), "rb") as f:
                found[name + SUFFIX[lang]] = hashlib.sha256(f.read().replace(b"\r\n", b"\n")).hexdigest()
    return found


def own_doc(thing):
    """The docstring a function, a class or a property has itself (not one it inherits: those of Python's own
    classes change from version to version)."""
    if not (callable(thing) or isinstance(thing, (property, type)) or inspect.ismodule(thing)):
        return ""                                    # (a value: what it has is the docstring of its type)
    text = getattr(thing, "__doc__", None)
    return inspect.cleandoc(text) if isinstance(text, str) and text.strip() else ""


def digest():
    """A checksum of what the manual is made from: the files (line ends do not count), and of the Python
    package what the reference shows, its signatures and docstrings. The same on every platform."""
    h = hashlib.sha256()
    for path in sources():
        with open(path, "rb") as f:
            data = f.read()
        if not path.endswith(".jpg"):
            data = data.replace(b"\r\n", b"\n")
        h.update(os.path.relpath(path, ROOT).replace(os.sep, "/").encode() + b"\0" + hashlib.sha256(data).digest())
    h.update(PythonReference(None).interface().encode())
    return h.hexdigest()


def check():
    if not os.path.exists(OUTPUT):
        print("docs/manual.html is missing: python docs/make_manual.py makes it")
        return 1
    found = re.search(r'<meta name="xisfconv-manual-sources" content="([0-9a-f]+)">', read_text(OUTPUT))
    if not found or found.group(1) != digest():
        print("docs/manual.html was not made from the sources as they are now: an example, the header, a signature\n"
              "or a docstring of the Python package, or docs/manual.in.html changed. This makes it again:\n"
              "    python docs/make_manual.py        (needs Pygments; it says what else it needs)")
        return 1
    print("docs/manual.html is up to date")
    return 0


# ------------------------------------------------------------------------------------------------
# Running the example programs
# ------------------------------------------------------------------------------------------------

def chapters_of(text):
    """{chapter: lines} of what a tour printed."""
    found, name = {}, None
    for line in text.replace("\r\n", "\n").split("\n"):
        if line.startswith("== "):
            name = line[3:].strip()
            found[name] = []
        elif name is not None:
            found[name].append(line.rstrip())
    return {key: "\n".join(lines).strip("\n") for key, lines in found.items()}


def run_programs(frame, programs, solved):
    """Runs the first programs and the tours on the frame, in a directory of their own where the frame has its
    plain name, and keeps what they printed."""
    from PIL import Image                 # (asked for now, not after the programs have run)

    def program(name, lang):
        source = os.path.join(EXAMPLES, name + SUFFIX[lang])
        if lang == "py":
            return [sys.executable, source]
        for candidate in ("xisfconv_%s_%s" % (name, lang), "xisfconv_%s_%s.exe" % (name, lang)):
            for directory in (programs, os.path.join(programs, "Release")):
                built = os.path.join(directory, candidate)
                if os.path.exists(built):
                    if os.path.getmtime(built) < os.path.getmtime(source):
                        sys.exit("%s is older than examples/%s%s: build it again first (cmake --build %s)" % (
                            built, name, SUFFIX[lang], programs))
                    return [os.path.abspath(built)]
        sys.exit("no xisfconv_%s_%s in %s: build with -DXISFCONV_BUILD_TESTS=ON" % (name, lang, programs))

    # The programs run in other directories: what PYTHONPATH and XISFCONV_LIBRARY name from here is named from anywhere.
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    if env.get("PYTHONPATH"):
        env["PYTHONPATH"] = os.pathsep.join(os.path.abspath(part) if part else part for part in env["PYTHONPATH"].split(os.pathsep))
    if env.get("XISFCONV_LIBRARY") and os.path.exists(env["XISFCONV_LIBRARY"]):
        env["XISFCONV_LIBRARY"] = os.path.abspath(env["XISFCONV_LIBRARY"])

    def said(command, where):
        done = subprocess.run(command, cwd=where, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
        if done.returncode != 0:
            sys.exit("%s failed (%d):\n%s%s" % (" ".join(command), done.returncode, done.stdout, done.stderr))
        return done.stdout

    for name in ("first", "tour"):        # every program is there and up to date, before any of them runs
        for lang, _ in LANGUAGES:
            program(name, lang)

    results = {"frame": os.path.basename(frame), "frame_bytes": os.path.getsize(frame), "first": {}, "tour": {},
               "solved": {}, "files": [], "examples": example_digests()}
    def bring(source, where):
        try:
            os.symlink(os.path.abspath(source), os.path.join(where, os.path.basename(source)))
        except (OSError, NotImplementedError, AttributeError):
            shutil.copyfile(source, os.path.join(where, os.path.basename(source)))

    work = tempfile.mkdtemp(prefix="xisfconv-manual-")
    try:
        if solved:
            results["solved_frame"] = os.path.basename(solved)
        for lang, _ in LANGUAGES:
            where = os.path.join(work, lang)
            os.makedirs(os.path.join(where, "out"))
            bring(frame, where)
            results["first"][lang] = said(program("first", lang) + [results["frame"]], where).strip("\n")
            results["tour"][lang] = chapters_of(said(program("tour", lang) + [results["frame"], "out"], where))
            if solved:
                bring(solved, where)
                again = said(program("tour", lang) + [results["solved_frame"], "out", "wcs"], where)
                results["solved"][lang] = chapters_of(again)["wcs"]
            print("ran the %s programs" % lang)
        shown = os.path.join(work, "c", "out")
        results["files"] = [[name, os.path.getsize(os.path.join(shown, name))] for name in sorted(os.listdir(shown))]

        # the picture: preview.png of the tour, as a JPEG to keep the page small
        picture = Image.open(os.path.join(shown, "preview.png")).convert("L")
        picture.save(FRAME, quality=80, optimize=True, progressive=True)
        results["picture"] = list(picture.size)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    keep_results(results)


def keep_results(results):
    with open(RESULTS, "w", encoding="utf-8", newline="\n") as f:
        json.dump(results, f, indent=1, sort_keys=True, ensure_ascii=False)
        f.write("\n")


# ------------------------------------------------------------------------------------------------
# Code: the marked parts of the examples, with colours and with links into the reference
# ------------------------------------------------------------------------------------------------

MARKS = {"c": re.compile(r"^\s*/\* \[(/?)(\w+)\] \*/\s*$"), "cpp": re.compile(r"^\s*// \[(/?)(\w+)\]\s*$"),
         "py": re.compile(r"^\s*# \[(/?)(\w+)\]\s*$")}


def marked(lang, name="tour"):
    """{mark: the lines between [mark] and [/mark]} of an example."""
    parts, current = {}, None
    for line in read_text(os.path.join(EXAMPLES, name + SUFFIX[lang])).split("\n"):
        mark = MARKS[lang].match(line)
        if mark and mark.group(1):
            if mark.group(2) != current:
                sys.exit("%s%s: [/%s] closes [%s]" % (name, SUFFIX[lang], mark.group(2), current))
            current = None
        elif mark:
            current = mark.group(2)
            parts[current] = []
        elif current is not None:
            parts[current].append(line)
    return {key: "\n".join(lines).strip("\n") for key, lines in parts.items()}


def whole(lang, name):
    """An example file without the licence lines of its first comment."""
    text = read_text(os.path.join(EXAMPLES, name + SUFFIX[lang]))
    text = re.sub(r"^#!.*\n", "", text)
    return re.sub(r"\n[ */#]*\n[ */#]*SPDX-License-Identifier:.*\n[ */#]*Copyright.*(?=\n)", "", text, count=1).strip("\n")


class Painter:
    """Pygments, with the class names of this page."""

    def __init__(self):
        try:
            import pygments
            from pygments.formatters import HtmlFormatter
            from pygments.lexers import BashLexer, CLexer, CMakeLexer, CppLexer, PythonLexer
        except ImportError:
            sys.exit("Pygments is needed to make the manual: pip install pygments")
        self._highlight = pygments.highlight
        self._formatter = HtmlFormatter(nowrap=True, classprefix="t-")
        self._lexers = {"c": CLexer(), "cpp": CppLexer(), "py": PythonLexer(), "sh": BashLexer(), "cmake": CMakeLexer()}
        self.c_names = set()
        self.py_names = set()

    def paint(self, code, lang):
        if lang not in self._lexers:
            return html.escape(code)
        painted = self._highlight(code, self._lexers[lang], self._formatter).rstrip("\n")
        if lang in ("c", "cpp"):
            painted = re.sub(r'<span class="t-n\w*">(\w+)</span>', self._c_link, painted)
        elif lang == "py":
            dot = r'<span class="t-o">\.</span>'
            painted = re.sub(r'(<span class="t-n\w*">xisfconv</span>%s<span class="t-n\w*">(astropy|xisf)</span>%s)'
                             r'(<span class="t-n\w*">(\w+)</span>)' % (dot, dot), self._py_link, painted)
            painted = re.sub(r'(<span class="t-n\w*">xisfconv</span>%s)()(<span class="t-n\w*">(\w+)</span>)' % dot,
                             self._py_link, painted)
        return painted

    def _c_link(self, match):
        name = match.group(1)
        return '<a href="#c-%s">%s</a>' % (name, match.group(0)) if name in self.c_names else match.group(0)

    def _py_link(self, match):
        name = (match.group(2) + "." if match.group(2) else "") + match.group(4)
        if name not in self.py_names:
            return match.group(0)
        return '%s<a href="#py-%s">%s</a>' % (match.group(1), name, match.group(3))

    def block(self, code, lang, label=None, cls="code"):
        head = '<span class="code-label">%s</span>' % html.escape(label) if label else ""
        return '<div class="%s">%s<pre><code>%s</code></pre></div>' % (cls, head, self.paint(code, lang))


def output_block(text, label="prints"):
    return '<div class="output"><span class="code-label">%s</span><pre><samp>%s</samp></pre></div>' % (
        html.escape(label), html.escape(text))


# ------------------------------------------------------------------------------------------------
# The C reference: the header, set as a page
# ------------------------------------------------------------------------------------------------

def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def comment_text(raw):
    """The lines of a comment that begins a line, without its marks."""
    lines = raw.split("\n")
    lines[0] = re.sub(r"^\s*/\*+ ?", "", lines[0])
    lines[-1] = re.sub(r"\s*\*+/\s*$", "", lines[-1])
    return [lines[0]] + [re.sub(r"^\s*\* ?", "", line) for line in lines[1:]]


def defined_names(code):
    """The names a piece of the header declares: functions, types, constants of enumerations, macros."""
    names = []
    names += re.findall(r"^#\s*define\s+(\w+)", code, re.M)
    names += re.findall(r"^\s{4}(XISFCONV_\w+)\s*=", code, re.M)
    names += re.findall(r"\(\*(xisfconv_\w+)\)\s*\(", code)
    names += re.findall(r"^typedef\s+(?:struct\s+)?[\w ]+?\b(xisfconv_\w+);", code, re.M)
    names += re.findall(r"^}\s*(xisfconv_\w+);", code, re.M)
    names += re.findall(r"^XISFCONV_API\s+[\w \*]+?\b(xisfconv_\w+)\(", code, re.M)
    seen = []
    for name in names:
        if name not in seen:
            seen.append(name)
    return seen


def parse_header():
    """[(section title, [entry])] with entry = {"doc": lines or None, "code": text or None, "names": [...]}."""
    text = read_text(HEADER)
    lines = text.split("\n")
    start = next(i for i, line in enumerate(lines) if line.startswith("/* ----"))
    end = next(i for i, line in enumerate(lines) if line.startswith("#ifdef __cplusplus") and i > start)
    preface = comment_text("\n".join(lines[:next(i for i, line in enumerate(lines) if line.startswith(" */")) + 1]))

    sections, entries, i = [], None, start
    pending = None                                   # a comment waiting for the declaration behind it
    while i < end:
        line = lines[i]
        if line.startswith("/* ----"):               # a banner: the title of a section, and sometimes a word about it
            j = i + 1
            while not lines[j].rstrip().endswith("*/"):
                j += 1
            about = [re.sub(r"^\s*\* ?", "", text) for text in lines[i + 2:j]]
            entries = []
            sections.append((re.sub(r"^\s*\* ?", "", lines[i + 1]).strip(), entries))
            if any(text.strip() for text in about):
                entries.append({"doc": about, "code": None, "names": []})
            i = j + 1
            continue
        if not line.strip():
            if pending is not None:
                entries.append({"doc": pending, "code": None, "names": []})
                pending = None
            i += 1
            continue
        part = re.match(r"^/\* --- (.+?) -+ \*/$", line)
        if part:                                     # a part of a section
            if pending is not None:
                entries.append({"doc": pending, "code": None, "names": []})
                pending = None
            entries.append({"heading": part.group(1), "doc": None, "code": None, "names": []})
            i += 1
            continue
        if line.startswith("/*"):
            if pending is not None:
                entries.append({"doc": pending, "code": None, "names": []})
            j = i
            while "*/" not in lines[j]:
                j += 1
            pending = comment_text("\n".join(lines[i:j + 1]))
            i = j + 1
            continue
        j, depth = i, 0                              # declarations, up to an empty line or the next comment
        while j < end:
            depth += lines[j].count("{") - lines[j].count("}")
            j += 1
            if depth == 0 and (not lines[j].strip() or lines[j].startswith("/*")) and not lines[j - 1].rstrip().endswith(","):
                break
        code = "\n".join(lines[i:j])
        entries.append({"doc": pending, "code": code, "names": defined_names(code)})
        pending = None
        i = j
    return preface, sections


def plain_declaration(code):
    """A declaration as a reader wants it: without the export macro, the lines that continue it moved to match."""
    out, shift = [], 0
    for line in code.split("\n"):
        if line.startswith("XISFCONV_API "):
            line, shift = line[len("XISFCONV_API "):], len("XISFCONV_API ")
        elif shift and line.startswith(" " * shift):
            line = line[shift:]
        else:
            shift = 0
        out.append(line)
    return "\n".join(out)


class CReference:
    def __init__(self, painter):
        self.painter = painter
        self.preface, self.sections = parse_header()
        self.names = [name for _, entries in self.sections for entry in entries for name in entry["names"]]
        painter.c_names = set(self.names)

    def inline(self, text):
        text = html.escape(text, quote=False)
        text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)

        def name(match):
            word = match.group(0)
            if word in self.painter.c_names:
                return '<a href="#c-%s"><code>%s</code></a>' % (word, word)
            return "<code>%s</code>" % word
        return re.sub(r"(?<![\w>#-])(?:xisfconv_\w+|XISFCONV_\w+)\b(?![^<]*</code>)", name, text)

    def doc(self, lines):
        """A comment as text: its paragraphs, and what it sets out in columns as it is set out."""
        out, prose, table = [], [], []

        def flush():
            if prose:
                out.append("<p>%s</p>" % self.inline(" ".join(prose)))
                prose.clear()
            if table:
                out.append('<pre class="laid">%s</pre>' % self.inline("\n".join(textwrap.dedent("\n".join(table)).split("\n"))))
                table.clear()
        for line in lines:
            if not line.strip():
                flush()
            elif line.startswith("  "):
                if prose:
                    flush()
                table.append(line)
            else:
                if table:
                    flush()
                prose.append(line.strip())
        flush()
        return "".join(out)

    def conventions(self):
        """The rules at the top of the header, which are set there as a list of name and text."""
        at = self.preface.index("Conventions") + 2
        items, intro = [], []
        for line in self.preface[1:at - 2]:
            if "SPDX" in line or "Copyright" in line or line.startswith("xisfconv.h"):
                continue
            intro.append(line)
        for line in self.preface[at:]:
            found = re.match(r"^(\w+)\s{2,}(\S.*)$", line)
            if found:
                items.append([found.group(1), [found.group(2)]])
            elif line.strip() and items:
                items[-1][1].append(line.strip())
        rows = "".join("<dt>%s</dt><dd>%s</dd>" % (html.escape(term), self.inline(" ".join(text))) for term, text in items)
        return '%s<dl class="rules">%s</dl>' % (self.doc(intro), rows)

    def declaration(self, entry):
        painted = self.painter.paint(plain_declaration(entry["code"]), "c")
        for name in entry["names"]:                  # the place where a name is declared is where links to it lead
            target = '<a href="#c-%s"><span class="t-n">%s</span></a>' % (name, name)
            if target in painted:
                painted = painted.replace(target, '<span class="t-n def" id="c-%s">%s</span>' % (name, name), 1)
            else:
                painted = re.sub(r"(?<![\w-])%s(?![\w-])" % re.escape(name), '<span class="def" id="c-%s">%s</span>' % (name, name),
                                 painted, count=1)
            painted = painted.replace('<a href="#c-%s"><span class="t-nc">%s</span></a>' % (name, name),
                                      '<span class="t-nc">%s</span>' % name)
        return '<pre class="decl"><code>%s</code></pre>' % painted

    def render(self):
        out = ['<h3 id="c-conventions">Conventions</h3>', self.conventions()]
        functions = sorted(name for name in self.names if name.startswith("xisfconv_") and
                           re.search(r"\b%s\(" % name, read_text(HEADER)))
        out.append('<details class="index"><summary>All %d functions by name</summary><ul class="columns">%s</ul></details>' % (
            len(functions), "".join('<li><a href="#c-%s"><code>%s</code></a></li>' % (name, name) for name in functions)))
        for title, entries in self.sections:
            out.append('<h3 id="c-%s">%s</h3>' % (slug(title), html.escape(title)))
            for entry in entries:
                if "heading" in entry:
                    out.append("<h4>%s</h4>" % html.escape(entry["heading"]))
                    continue
                if entry["code"] is None:
                    out.append('<div class="note">%s</div>' % self.doc(entry["doc"]))
                    continue
                out.append('<div class="entry">%s%s</div>' % (
                    self.declaration(entry), '<div class="doc">%s</div>' % self.doc(entry["doc"]) if entry["doc"] else ""))
        return "\n".join(out)


# ------------------------------------------------------------------------------------------------
# The Python reference: signatures and docstrings
# ------------------------------------------------------------------------------------------------

PYTHON_GROUPS = (
    ("Reading", ("open", "read", "read_image", "detect_format", "File", "FileImage", "Properties")),
    ("Writing", ("write", "Image", "Keywords", "Card", "PropertyDict")),
    ("Whole files", ("convert", "rewrite", "rewrite_in_place", "stored_as_requested", "RewriteResult", "verify", "Report")),
    ("Stretch and astrometry", ("auto_stretch", "apply_stretch", "StretchParams", "wcs_flip_rows")),
    ("The library", ("library_version", "library_path", "codec_available")),
)


# The values of the package, which have no docstring of their own.
VALUES = {"library_path": "The path of the shared library the package loaded, a ``str``. (The environment variable "
                          "``XISFCONV_LIBRARY`` names another one than the library that comes with the package.)"}


class PythonReference:
    def __init__(self, painter):
        import xisfconv
        import xisfconv.astropy
        import xisfconv.xisf
        self.painter = painter
        self.package = xisfconv
        self.errors = [getattr(xisfconv, name) for name in xisfconv.__all__
                       if isinstance(getattr(xisfconv, name), type) and issubclass(getattr(xisfconv, name), (Exception, Warning))]
        grouped = {name for _, names in PYTHON_GROUPS for name in names} | {cls.__name__ for cls in self.errors}
        missing = sorted(set(xisfconv.__all__) - grouped)
        if missing:
            sys.exit("the Python reference has no place for: %s (PYTHON_GROUPS in docs/make_manual.py)" % ", ".join(missing))
        names = set(grouped) | {"astropy", "xisf"}
        for _, group in PYTHON_GROUPS:
            for name in group:
                thing = getattr(xisfconv, name)
                if isinstance(thing, type):
                    names |= {"%s.%s" % (name, member) for member, _ in self.members(thing)}
        names |= {"astropy." + name for name in xisfconv.astropy.__all__}
        names |= {"xisf.XISF"} | {"xisf.XISF." + member for member, _ in self.members(xisfconv.xisf.XISF)}
        self.names = names
        if painter is not None:
            painter.py_names = names

    def interface(self):
        """What the reference shows of the package, as text: every signature and docstring, and which exception
        a status is. If this is the same, the reference is."""
        import xisfconv.astropy
        import xisfconv.xisf
        from xisfconv import _core
        lines = [own_doc(self.package), own_doc(xisfconv.astropy), own_doc(xisfconv.xisf)]

        def tell(name, thing):
            if isinstance(thing, type):
                lines.append("%s\n%s" % (self.class_signature(name, thing), own_doc(thing)))
                lines.append(repr(self.also(thing)) if issubclass(thing, (Exception, Warning)) else "")
                for member, value in self.members(thing):
                    function = value.__func__ if isinstance(value, (staticmethod, classmethod)) else value
                    tell("%s.%s %s" % (name, member, type(value).__name__), function)
            else:
                lines.append("%s\n%s" % (self.signature(name, thing) if callable(thing) else name, own_doc(thing)))
        for name in sorted(self.package.__all__):
            tell(name, getattr(self.package, name))
        for name in xisfconv.astropy.__all__:
            tell("astropy." + name, getattr(xisfconv.astropy, name))
        tell("xisf.XISF", xisfconv.xisf.XISF)
        lines.append(repr(sorted((status, cls.__name__) for status, cls in _core._ERRORS.items())))
        return "\n\0".join(lines)

    @staticmethod
    def members(cls):
        """The public attributes a class defines itself, in the order of its source."""
        found, told = [], own_doc(cls)
        for name, value in vars(cls).items():
            if name.startswith("_") or re.search(r"^%s$" % re.escape(name), told, re.M):
                continue                             # (private, or described in the docstring of the class)
            if isinstance(value, (property, staticmethod, classmethod)) or inspect.isfunction(value):
                found.append((name, value))
        return found

    # -- docstrings: the few forms of reStructuredText the package writes them in -----------------

    def inline(self, text, scope):
        text = html.escape(text, quote=False)

        def role(match):
            kind, target = match.group(1), match.group(2).lstrip("~")
            shown = target.split(".")[-1] if match.group(2).startswith("~") else target
            if kind in ("func", "meth") and not shown.endswith(")"):
                shown += "()"
            for candidate in (target[9:] if target.startswith("xisfconv.") else None, scope + target if scope else None, target,
                              "%s.%s" % (scope.split(".")[0], target) if scope else None):
                if candidate and candidate in self.painter.py_names:
                    return '<a href="#py-%s"><code>%s</code></a>' % (candidate, shown)
            return "<code>%s</code>" % shown
        text = re.sub(r":(func|class|meth|attr|mod|exc|data):`([^`]+)`", role, text)
        text = re.sub(r"``([^`]+)``", r"<code>\1</code>", text)
        return re.sub(r"(?<![\w`])`([^`<]+)`(?![\w`])", r"<code>\1</code>", text)

    def blocks(self, lines, scope):
        def indent(line):
            return len(line) - len(line.lstrip())

        def body(start, deeper_than):
            stop = start
            while stop < len(lines) and (not lines[stop].strip() or indent(lines[stop]) > deeper_than):
                stop += 1
            return stop

        out, i = [], 0
        while i < len(lines):
            if not lines[i].strip():
                i += 1
                continue
            at = indent(lines[i])
            if lines[i].lstrip().startswith("- "):                      # a list
                items = []
                while i < len(lines) and lines[i].strip() and indent(lines[i]) == at and lines[i].lstrip().startswith("- "):
                    stop = i + 1
                    while stop < len(lines) and lines[stop].strip() and indent(lines[stop]) > at:
                        stop += 1
                    item = [lines[i][at + 2:]] + textwrap.dedent("\n".join(lines[i + 1:stop])).split("\n")
                    items.append("<li>%s</li>" % self.blocks(item, scope))
                    i = stop
                    while i < len(lines) and not lines[i].strip() and i + 1 < len(lines) and lines[i + 1].lstrip().startswith("- "):
                        i += 1
                out.append("<ul>%s</ul>" % "".join(items))
                continue
            j = i
            while j < len(lines) and lines[j].strip() and indent(lines[j]) == at:
                j += 1
            text = " ".join(line.strip() for line in lines[i:j])
            if j - i == 1 and j < len(lines) and lines[j].strip() and indent(lines[j]) > at:   # a term and what it means
                stop = body(j, at)
                meaning = self.blocks(textwrap.dedent("\n".join(lines[j:stop])).split("\n"), scope)
                item = "<dt>%s</dt><dd>%s</dd>" % (self.inline(text, scope), meaning)
                if out and out[-1].endswith("</dl>"):
                    out[-1] = out[-1][:-5] + item + "</dl>"
                else:
                    out.append("<dl>%s</dl>" % item)
                i = stop
                continue
            if text.endswith("::"):                                      # code follows
                stop = body(j, at)
                code = textwrap.dedent("\n".join(lines[j:stop])).strip("\n")
                text = text[:-2].rstrip()
                if text:
                    out.append("<p>%s:</p>" % self.inline(text.rstrip(":"), scope))
                out.append(self.painter.block(code, "py", cls="code small"))
                i = stop
                continue
            out.append("<p>%s</p>" % self.inline(text, scope))
            i = j
        return "".join(out)

    def doc(self, thing, scope=""):
        text = own_doc(thing)
        return '<div class="doc">%s</div>' % self.blocks(text.split("\n"), scope) if text else ""

    def signature(self, name, function, drop_self=False):
        try:
            parameters = list(inspect.signature(function).parameters.values())
        except (TypeError, ValueError):
            return name + "(...)"
        if drop_self and parameters and parameters[0].name in ("self", "cls"):
            parameters = parameters[1:]
        parameters = [p for p in parameters if not p.name.startswith("_")]
        parts, star = [], False
        for p in parameters:
            if p.kind is p.KEYWORD_ONLY and not star:
                parts.append("*")
                star = True
            if p.kind is p.VAR_POSITIONAL:
                star = True
            parts.append(str(p))
        one = "%s(%s)" % (name, ", ".join(parts))
        if len(one) <= 96:
            return one
        lines, line = [], name + "("
        for k, part in enumerate(parts):
            piece = part + (", " if k + 1 < len(parts) else ")")
            if len(line) + len(piece.rstrip()) > 96:
                lines.append(line.rstrip())
                line = " " * (len(name) + 1)
            line += piece
        return "\n".join(lines + [line])

    def class_signature(self, name, cls):
        """A class as it is made: a named tuple from its fields, any other from the arguments of its own __init__."""
        if getattr(cls, "_fields", ()):
            defaults = getattr(cls, "_field_defaults", {})
            return "%s(%s)" % (name, ", ".join("%s=%r" % (field, defaults[field]) if field in defaults else field
                                               for field in cls._fields))
        return self.signature(name, vars(cls)["__init__"], True) if "__init__" in vars(cls) else name

    @staticmethod
    def also(cls):
        """The exceptions of Python itself that an error of the package is as well."""
        names = [base.__name__ for base in cls.__mro__[1:] if base.__module__ == "builtins" and
                 base not in (object, BaseException, Exception)]
        return [name for k, name in enumerate(names) if name not in ("OSError", "LookupError", "Warning") or k == 0]

    def entry(self, anchor, shown, thing, scope, drop_self=False, kind=""):
        if callable(thing) and not isinstance(thing, type):
            shown = self.signature(shown, thing, drop_self)
        painted = self.painter.paint(shown, "py").replace("\n", "\n")
        label = '<span class="kind">%s</span>' % kind if kind else ""
        return '<div class="entry"><pre class="decl" id="py-%s"><code>%s%s</code></pre>%s</div>' % (
            anchor, label, painted, self.doc(thing, scope))

    def class_entry(self, prefix, anchor, cls):
        shown = self.class_signature(prefix + cls.__name__, cls)
        out = ['<div class="entry class"><pre class="decl" id="py-%s"><code><span class="kind">class</span>%s</code></pre>%s' % (
            anchor, self.painter.paint(shown, "py"), self.doc(cls, anchor + "."))]
        members, bare = [], []
        for name, value in self.members(cls):
            target = "%s.%s" % (anchor, name)
            if isinstance(value, property):
                if not own_doc(value):
                    bare.append('<code class="def" id="py-%s">%s</code>' % (target, name))
                    continue
                members.append(self.entry(target, "%s.%s" % (cls.__name__, name), value, anchor + "."))
            else:
                function = value.__func__ if isinstance(value, (staticmethod, classmethod)) else value
                kind = "static" if isinstance(value, staticmethod) else ""
                members.append(self.entry(target, "%s.%s" % (cls.__name__, name), function, anchor + ".",
                                          drop_self=not isinstance(value, staticmethod), kind=kind))
        if bare:
            members.append('<p class="fields">And, as their names say: %s.</p>' % ", ".join(bare))
        if members:
            out.append('<div class="members">%s</div>' % "".join(members))
        out.append("</div>")
        return "".join(out)

    def error_table(self):
        rows = []
        for cls in sorted(self.errors, key=lambda c: (issubclass(c, Warning), c.__name__ != "Error", c.__name__)):
            also = self.also(cls)
            rows.append('<tr id="py-%s"><td><code>%s</code></td><td>%s</td><td>%s</td></tr>' % (
                cls.__name__, cls.__name__, ", ".join("<code>%s</code>" % name for name in also) or "",
                self.blocks(own_doc(cls).split("\n"), "")))
        return ('<table class="grid"><thead><tr><th>Class</th><th>Also a</th><th>Raised when</th></tr></thead>'
                "<tbody>%s</tbody></table>" % "".join(rows))

    def render(self):
        import xisfconv.astropy
        import xisfconv.xisf
        out = [self.doc(self.package)]
        for title, names in PYTHON_GROUPS:
            out.append('<h3 id="py-%s">%s</h3>' % (slug(title), html.escape(title)))
            for name in names:
                thing = getattr(self.package, name)
                if isinstance(thing, type):
                    out.append(self.class_entry("xisfconv.", name, thing))
                elif name in VALUES:
                    out.append('<div class="entry"><pre class="decl" id="py-%s"><code>%s</code></pre><div class="doc">%s</div></div>' % (
                        name, self.painter.paint("xisfconv." + name, "py"), self.blocks([VALUES[name]], "")))
                elif callable(thing):
                    out.append(self.entry(name, "xisfconv." + name, thing, ""))
                else:
                    sys.exit("xisfconv.%s is a value: the Python reference needs a line about it (VALUES)" % name)
        out.append('<h3 id="py-errors">Errors and warnings</h3>')
        out.append(self.error_table())
        out.append('<h3 id="py-astropy">xisfconv.astropy</h3>')
        out.append(self.doc(xisfconv.astropy, "astropy."))
        for name in xisfconv.astropy.__all__:
            out.append(self.entry("astropy." + name, "xisfconv.astropy." + name, getattr(xisfconv.astropy, name), "astropy."))
        out.append('<h3 id="py-xisf">xisfconv.xisf</h3>')
        out.append(self.doc(xisfconv.xisf, "xisf."))
        out.append(self.class_entry("xisfconv.xisf.", "xisf.XISF", xisfconv.xisf.XISF))
        return "\n".join(out)

    def status_table(self, c_reference):
        """Status codes of the C API, what they mean, and the exception each one is in Python."""
        from xisfconv import _core
        header = read_text(HEADER)
        block = header[header.index("typedef int32_t xisfconv_status;"):header.index("xisfconv_status_text")]
        rows = []
        for name, value, comment in re.findall(r"^\s{4}(XISFCONV_\w+)\s*=\s*(\d+),?[ \t]*(?:/\*(.*?)\*/)?", block, re.M | re.S):
            meaning = " ".join(comment.split()) if comment else "success"
            cls = _core._ERRORS.get(int(value))
            if name == "XISFCONV_OK":
                python = "no exception"
            elif name == "XISFCONV_ERR_MEMORY":
                python = "<code>MemoryError</code>"
            elif name == "XISFCONV_ERR_BUFFER":
                python = "not raised: the package sizes the buffers"
            elif name == "XISFCONV_ERR_INTERNAL":
                python = '<a href="#py-InternalError"><code>InternalError</code></a>'
            else:
                python = '<a href="#py-%s"><code>%s</code></a>' % (cls.__name__, cls.__name__)
                if cls is _core.FileError:
                    python += ', <a href="#py-InputNotFoundError"><code>InputNotFoundError</code></a> if the input is not there'
            rows.append('<tr><td><a href="#c-%s"><code>%s</code></a></td><td class="num">%s</td><td>%s</td><td>%s</td></tr>' % (
                name, name, value, c_reference.inline(meaning[0].upper() + meaning[1:]), python))
        if len(rows) != len(re.findall(r"^\s{4}XISFCONV_\w+\s*=", block, re.M)):
            sys.exit("the table of the statuses does not have every status of the header")
        return ('<table class="grid"><thead><tr><th>Status</th><th>Value</th><th>Meaning</th><th>In Python</th></tr></thead>'
                "<tbody>%s</tbody></table>" % "".join(rows))


# ------------------------------------------------------------------------------------------------
# The page
# ------------------------------------------------------------------------------------------------

def tabs(panels, painter, group):
    """One panel per language: with scripts the reader's language shows, without them all three do."""
    out = ['<div class="langs" data-group="%s">' % group]
    for lang, title in LANGUAGES:
        if lang in panels:
            out.append('<section class="lang" data-lang="%s"><h4 class="lang-name">%s</h4>%s</section>' % (lang, title, panels[lang]))
    out.append("</div>")
    return "".join(out)


def size_text(count):
    return "{:,}".format(count).replace(",", " ")


def build(keep_output=False):
    with open(RESULTS, encoding="utf-8") as f:
        results = json.load(f)
    # The manual shows what the examples printed beside the examples: the two have to belong together.
    changed = sorted(name for name, checksum in example_digests().items() if results.get("examples", {}).get(name) != checksum)
    if changed and not keep_output:
        sys.exit("%s changed since the examples ran on the frame; docs/manual-output.json holds what they printed then.\n"
                 "Run them again:   python docs/make_manual.py --run FRAME.xisf --solved SOLVED.xisf\n"
                 "or, if the change does not change what they print for that frame (a comment, a name):\n"
                 "                  python docs/make_manual.py --keep-output" % ", ".join("examples/" + name for name in changed))
    if changed:
        results["examples"] = example_digests()
        keep_results(results)
        print("kept the output of %s as it was" % ", ".join(changed))
    painter = Painter()
    c_reference = CReference(painter)
    python_reference = PythonReference(painter)
    parts = {lang: marked(lang) for lang, _ in LANGUAGES}
    version = ".".join(re.search(r"#define XISFCONV_VERSION_%s (\d+)" % part, read_text(HEADER)).group(1)
                       for part in ("MAJOR", "MINOR", "PATCH"))
    file_name = {"c": "tour.c", "cpp": "tour.cpp", "py": "tour.py"}

    def expand(match):
        words = match.group(1).split()
        kind, arguments = words[0], words[1:]
        if kind == "version":
            return version
        if kind == "sources":
            return digest()
        if kind == "frame":
            with open(FRAME, "rb") as f:
                return "data:image/jpeg;base64," + base64.b64encode(f.read()).decode()
        if kind == "frame-size":
            return 'width="%d" height="%d"' % tuple(results["picture"])
        if kind == "tabs":                               # a chapter of the tour in the three languages
            name = arguments[0]
            panels = {}
            for lang, _ in LANGUAGES:
                if name not in parts[lang]:
                    sys.exit("%s has no [%s]" % (file_name[lang], name))
                panel = painter.block(parts[lang][name], lang, "%s, %s" % (file_name[lang], name))
                if "quiet" not in arguments:
                    panel += output_block(results["tour"][lang][name])
                panels[lang] = panel
            return tabs(panels, painter, name)
        if kind == "code":                               # one marked part: code LANG NAME [FILE]
            lang, name = arguments[0], arguments[1]
            return painter.block(parts[lang][name], lang, "%s, %s" % (file_name[lang], name))
        if kind == "output":
            return output_block(results["tour"][arguments[0]][arguments[1]])
        if kind == "solved":
            return tabs({lang: output_block(results["solved"][lang], "prints, for %s" % results["solved_frame"])
                         for lang, _ in LANGUAGES}, painter, "solved")
        if kind == "first":
            return tabs({lang: painter.block(whole(lang, "first"), lang, "first" + SUFFIX[lang]) +
                         output_block(results["first"][lang]) for lang, _ in LANGUAGES}, painter, "first")
        if kind == "files":
            rows = "".join('<tr><td><code>%s</code></td><td class="num">%s</td><td>%s</td></tr>' % (
                name, size_text(size), FILE_NOTES.get(name, "")) for name, size in results["files"])
            return ('<table class="grid"><thead><tr><th>File</th><th>Bytes</th><th>Written by</th></tr></thead>'
                    "<tbody>%s</tbody></table>" % rows)
        if kind == "status-table":
            return python_reference.status_table(c_reference)
        if kind == "c-reference":
            return c_reference.render()
        if kind == "python-reference":
            return python_reference.render()
        sys.exit("docs/manual.in.html: unknown mark <!--@%s-->" % match.group(1))

    page = read_text(TEMPLATE)

    def painted_block(match):                            # <pre data-lang="sh">...</pre> of the text itself
        return painter.block(html.unescape(match.group(2)).strip("\n"), match.group(1), cls="code plain")
    page = re.sub(r'<pre data-lang="(\w+)">(.*?)</pre>', painted_block, page, flags=re.S)
    page = re.sub(r"<!--@([\w -]+?)-->", expand, page)

    def contents(match):
        items, open_part = [], False
        for level, anchor, title in re.findall(r'<h([23]) id="([\w.-]+)"[^>]*>(.*?)</h\1>', page, re.S):
            title = re.sub(r"<[^>]+>", "", title)
            if level == "2":
                if open_part:
                    items.append("</ul></li>")
                items.append('<li><a href="#%s">%s</a><ul>' % (anchor, title))
                open_part = True
            else:
                items.append('<li><a href="#%s">%s</a></li>' % (anchor, title))
        return "<ul>%s</ul></li></ul>" % "".join(items) if open_part else ""
    page = page.replace("<!--@@contents-->", contents(None))

    broken = sorted({target for target in re.findall(r'href="#([^"]+)"', page)
                     if 'id="%s"' % target not in page})
    if broken:
        sys.exit("links to nowhere in the manual: %s" % ", ".join(broken))
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(page)
    print("wrote docs/manual.html, %s bytes" % size_text(os.path.getsize(OUTPUT)))


FILE_NOTES = {
    "stretched.png": "Stretch: the whole frame, stretched, 8 bits",
    "crop.xisf": "Write: 512 × 512 pixels with keywords and properties, zlib, SHA-256",
    "frame.fits": "Convert: the frame as FITS",
    "preview.png": "Convert: the picture at the top of this page",
    "smaller.xisf": "Rewrite: the frame with its blocks compressed, SHA-1",
    "unit.xish": "Units: the header of a distributed unit",
    "unit.xisb": "Units: the data blocks of that unit",
    "packed.xisf": "Units: the unit in one file again",
    "there.fits": "Progress: the frame as FITS once more, for the notes of the conversion",
    "back.xisf": "Progress: that file converted back to XISF",
}


def main():
    parser = argparse.ArgumentParser(description="Makes docs/manual.html.")
    parser.add_argument("--check", action="store_true", help="only say whether docs/manual.html is up to date")
    parser.add_argument("--run", metavar="FRAME", help="run the example programs on this XISF file first")
    parser.add_argument("--solved", metavar="FRAME", help="with --run: the plate-solved frame for the astrometry chapter")
    parser.add_argument("--programs", metavar="DIR", default=os.path.join(ROOT, "build"),
                        help="with --run: where xisfconv_tour_c and the others were built")
    parser.add_argument("--keep-output", action="store_true",
                        help="an example changed in a way that does not change what it prints: keep what is kept")
    options = parser.parse_args()
    if options.run and not options.solved:
        parser.error("--run needs --solved as well: the astrometry chapter shows what a plate-solved frame gives")
    if os.path.isdir(os.path.join(ROOT, "python", "xisfconv")) and "xisfconv" not in sys.modules:
        try:
            import xisfconv  # noqa: F401            (the installed package, or the one PYTHONPATH names)
        except ImportError:
            sys.path.insert(0, os.path.join(ROOT, "python"))
    try:
        import xisfconv.astropy  # noqa: F401
    except ImportError as e:
        sys.exit("The manual has the reference of the Python package, with xisfconv.astropy: the package has to be importable,\n"
                 "and astropy installed (%s). From a checkout: PYTHONPATH=python XISFCONV_LIBRARY=build-shared/libxisfconv.so" % e)
    if options.check:
        return check()
    if options.run:
        run_programs(options.run, options.programs, options.solved)
    build(options.keep_output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
