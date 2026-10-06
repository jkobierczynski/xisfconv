# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The Python interface of libxisfconv: files, images as NumPy arrays, conversion, verification.

Everything public here is re-exported by the package; see the package documentation.
"""

import atexit
import ctypes
import logging
import math
import operator
import os
import re
import sys
import threading
import time
import warnings
import weakref
from collections import namedtuple
from collections.abc import Mapping, MutableSequence
from ctypes import byref, c_char_p, c_int32, c_size_t, c_uint64, c_void_p

import numpy as np
import numpy.ma    # noqa: F401 - now, not during the first call: an import runs code that a signal can tear

from . import _lib

_library, library_path = _lib.load()
_log = logging.getLogger("xisfconv")

# ------------------------------------------------------------------------------------------
# Errors and warnings
# ------------------------------------------------------------------------------------------


class Error(Exception):
    """Base class of the errors of this package. ``status`` is the status code of the library."""

    status = _lib.ERR_INTERNAL


class ArgumentError(Error, ValueError):
    """An option is out of range, or a combination of options is not possible."""

    status = _lib.ERR_ARGUMENT


class FileError(Error, OSError):
    """A file cannot be opened, read, written or renamed."""

    status = _lib.ERR_IO


class InputNotFoundError(FileError, FileNotFoundError):
    """The input file does not exist."""


class FormatError(Error):
    """The file is malformed or truncated."""

    status = _lib.ERR_FORMAT


class UnsupportedError(Error):
    """A feature the library, or this build of it, does not implement; the file may be fine."""

    status = _lib.ERR_UNSUPPORTED


class ChecksumError(Error):
    """A checksum stored in the file does not match the data."""

    status = _lib.ERR_CHECKSUM


class ImageIndexError(Error, IndexError):
    """There is no image with this index."""

    status = _lib.ERR_INDEX


class OutputExistsError(Error, FileExistsError):
    """The output file exists and ``overwrite`` was not given."""

    status = _lib.ERR_EXISTS


class NotFoundError(Error, LookupError):
    """The image has no such thing: no astrometric solution, no saved stretch."""

    status = _lib.ERR_NOT_FOUND


class Cancelled(Error):
    """The operation was stopped from its progress function."""

    status = _lib.ERR_CANCELLED


class InternalError(Error):
    """A bug in the library or in this package."""

    status = _lib.ERR_INTERNAL


class XisfconvWarning(UserWarning):
    """Something the library wants the user to know about a result."""


_ERRORS = {
    _lib.ERR_ARGUMENT: ArgumentError,
    _lib.ERR_IO: FileError,
    _lib.ERR_FORMAT: FormatError,
    _lib.ERR_UNSUPPORTED: UnsupportedError,
    _lib.ERR_CHECKSUM: ChecksumError,
    _lib.ERR_INDEX: ImageIndexError,
    _lib.ERR_EXISTS: OutputExistsError,
    _lib.ERR_NOT_FOUND: NotFoundError,
    _lib.ERR_CANCELLED: Cancelled,
}


def _text(value):
    return value.decode("utf-8", "replace") if value else ""


def _said(value):
    """The text of a message of the library. Bytes of a file name that are not UTF-8 come out
    as os.fsdecode gives them, so that the name is found in the message."""
    return value.decode("utf-8", "surrogateescape") if value else ""


def _bytes(text, what="text"):
    """UTF-8 for the library. A NUL would silently end the string there."""
    data = text if isinstance(text, bytes) else str(text).encode("utf-8", "surrogateescape")
    if b"\0" in data:
        raise ValueError("%s contains a NUL character" % what)
    return data


def _path(path):
    """A file name as the library wants it: UTF-8 on Windows, the bytes of the name elsewhere."""
    name = os.fspath(path)
    if sys.platform == "win32":
        if isinstance(name, bytes):
            name = os.fsdecode(name)
        data = name.encode("utf-8", "surrogatepass")
    else:
        data = os.fsencode(name)
    if not data:
        raise ValueError("the file name is empty")
    if b"\0" in data:
        raise ValueError("the file name contains a NUL character")
    return data


# The messages of the library are those of the command line tool and name its options. Here they
# name the arguments of this package instead. (python/tests checks that no option is missing.)
_OPTION_WORDS = [
    ("--to fits|asdf|tiff|png|xisf", 'format="fits", "asdf", "tiff", "png" or "xisf"'),
    ("--stretch=unlinked", 'stretch="unlinked"'),
    ("--stretch=linked", 'stretch="linked"'),
    ("--stretch", "stretch"),
    ("--bits u8", 'sample_format="uint8"'),
    ("--bits u16", 'sample_format="uint16"'),
    ("--bits f32", 'sample_format="float32"'),
    ("--bits", "sample_format"),
    ("--codec zlib", 'codec="zlib"'),
    ("use --compress", "use codec=True"),
    ("add --compress", "add codec=True"),
    ("--compress", "codec"),
    ("--force", "overwrite=True"),
    ("--bounds expects lo:hi with hi > lo, e.g. 0:65535", "bounds expects (lower, upper) with upper above lower"),
    ("override with --bounds", "override with bounds=(lower, upper)"),
    ("--bounds", "bounds"),
    ("--image", "image"),
    ("--top-down", 'row_order="top-down"'),
    ("--no-verify", "verify=False"),
    ("add --in-place to replace it", "use rewrite_in_place() to replace it"),
    ("or directory with -o or -d", ""),
    ("--in-place", "rewrite_in_place()"),
    ("--sip-order", "sip_order"),
    ("--asdf-tree-json", "the tree as JSON"),
]
# An option is a word of its own: "--force" inside a file name ("a--force.xisf") is not one.
_BEFORE = r"(?<![^\s(\"'=,;:])"
_AFTER = r"(?![A-Za-z0-9_/\\-]|\.[A-Za-z0-9])"
_OPTION = re.compile(_BEFORE + r"--[a-z][a-z-]*" + _AFTER)
_OPTION_PATTERNS = [(re.compile((_BEFORE if option.startswith("-") else "") + re.escape(option) + _AFTER), argument)
                 for option, argument in _OPTION_WORDS]


def _wording(message, names=()):
    """A message of the library in the words of this package. `names`: file names the message
    may hold, which are left as they are."""
    if "-" not in message:
        return message
    for name in sorted({name for name in names if name and "-" in name}, key=len, reverse=True):
        # The text around the name is reworded, the name is not.
        if any(name in option for option, _ in _OPTION_WORDS) or name.strip("-") == "":
            # A name that is itself (part of) an option, a file called "--force" or "-": it is
            # taken for the name only where a message begins with it.
            if message.startswith(name) and message[len(name):len(name) + 1] in ("", " ", ":"):
                return name + _wording(message[len(name):], ())
            continue
        if name in message:
            return name.join(_wording(part, names) for part in message.split(name))
    for option, argument in _OPTION_PATTERNS:
        message = option.sub(lambda match, argument=argument: argument, message)
    # an option this list does not know yet: at least as an argument name
    return _OPTION.sub(lambda match: match.group(0)[2:].replace("-", "_"), message).rstrip()


def _warn(message):
    # Blame the caller of the package, not the package. (Frames of contextlib between frames of
    # the package belong to it: a function of the package used in a `with` statement.)
    level = 1
    blamed = 2
    frame = sys._getframe(1)
    while frame is not None:
        module = (frame.f_globals.get("__name__") or "").split(".")[0]
        level += 1
        if module == "xisfconv":
            blamed = level + 1        # the frame above this one
        elif module != "contextlib":
            break
        frame = frame.f_back
    warnings.warn(message, XisfconvWarning, stacklevel=blamed)


# ------------------------------------------------------------------------------------------
# Calling the library
# ------------------------------------------------------------------------------------------


class _Resource(weakref.ref):
    """What frees a handle of the library when the Python object that owns it goes away.

    It is a weak reference to the owner whose callback is the library's own free function.
    The interpreter calls that with this reference, and ctypes passes on the pointer kept in
    _as_parameter_: no Python code runs. That matters for Ctrl-C: a KeyboardInterrupt raised
    inside a finalizer written in Python is printed and forgotten.
    """

    __slots__ = ("_as_parameter_", "free", "keep")


# The references are kept here until they have done their work. No lock guards the set: adding
# and removing an element are single steps for the interpreter, and a lock could be asked for a
# second time by a finalizer or a signal handler that closes a file while it is held.
_resources = set()
_resources_limit = 64


def _own(owner, free, pointer, keep=None):
    """Makes `free(pointer)` happen when `owner` is collected; `keep` lives until then."""
    global _resources_limit
    resource = _Resource(owner, free)
    resource._as_parameter_ = pointer
    resource.free = free
    resource.keep = keep
    _resources.add(resource)
    if len(_resources) >= _resources_limit:
        try:
            done = [r for r in list(_resources) if r() is None]   # their owner is gone: they have done their work
        except RuntimeError:
            done = []                                             # the set changed meanwhile: next time
        for r in done:
            _resources.discard(r)
        _resources_limit = max(64, 2 * len(_resources))
    return resource


def _release(resource):
    """Frees the handle now, and makes sure it is not freed a second time when the owner goes."""
    try:
        _resources.remove(resource)
    except KeyError:
        return   # released already
    # Should the callback of the reference still run, it finds nothing to free. (Between taking
    # the pointer out and freeing it nothing is called: an interrupt there would lose the handle.)
    alive = resource() is not None
    pointer, resource._as_parameter_ = resource._as_parameter_, None
    if pointer is not None and alive:
        resource.free(pointer)


_local = threading.local()
_UNSET = object()
_DEPTH = 24     # how deep calls of the library may be nested in one thread

# The contexts that are inside a call of the library now. When Python ends, daemon threads are
# not waited for, and one that came back from the library into an interpreter that is being
# taken down, to report progress, would crash it. So the calls are stopped and waited for
# before that, and from then on no call reports progress.
_active = set()
_exiting = []


def _stop_at_exit():
    _exiting.append(True)
    patience = 3
    idle = {}
    while True:
        try:
            waiting = False
            for context in list(_active):
                pointer = context._resource._as_parameter_
                if pointer is None:                     # the context has been freed
                    _active.discard(context)
                elif _library.xisfconv_context_cancel(pointer):
                    waiting = True                      # the library is at work in it: it will stop
                else:
                    # Listed, but the library is not in it: its thread is about to enter the
                    # library, or was torn out of the call by an exception and left the entry.
                    idle[context] = idle.get(context, 0) + 1
                    if idle[context] > 200:             # two seconds: it is the second
                        _active.discard(context)
                    else:
                        waiting = True
            if not waiting:
                return
            time.sleep(0.01)
        except BaseException:      # noqa: BLE001 - Ctrl-C while waiting: the calls are still to be waited for
            patience -= 1
            if patience < 0:
                raise


atexit.register(_stop_at_exit)
if hasattr(os, "register_at_fork"):
    # the threads of those calls do not exist in a child process
    os.register_at_fork(after_in_child=_active.clear)


# ------------------------------------------------------------------------------------------
# Signals, and the progress function
# ------------------------------------------------------------------------------------------
# A Python signal handler runs when the interpreter next executes Python code in the main
# thread. While that thread is inside the library there is none: Ctrl-C would wait for the end
# of the call. So the library is given something to call between its steps (the host progress
# handler of xisfconv.h), also when the caller has no progress function.
#
# What it calls must not be an ordinary function. The handlers run at the first instruction
# of whatever Python code comes next; in a function that is before its `try` block, and an
# exception raised there goes back to the C code that called the function, which can only
# print it: Ctrl-C would be shown and forgotten. A generator, though, comes back to life in the
# middle of its code, inside its `try`. So the library calls the `send` of the generator below:
# the handlers run there, and what they raise is caught, kept, and raised by the call once the
# library has stopped its work and cleaned up. (The loop is inside the `try` and not around it
# for the same reason: the jump back to the top of a loop is such a moment too.)

def _reporter(state):
    """The generator whose `send` the library calls with a progress report. It answers "go on"
    until something is raised in it; that is put into `state` and answered with "stop", and
    the generator has done its work: the next call gets a new one. (After the exception is
    caught there is no call and no jump back up, the two things that would let a second handler
    in before the answer is given.) `state` is [the progress function of the call that is
    running or None, what was raised or None]."""
    answer = None
    reporting = False
    try:
        while True:
            report = (yield answer)[0]
            reporting = True
            progress = state[0]
            if progress is not None:
                progress(_text(report.stage), int(report.done), int(report.total))
            answer = _lib.HOST_GO_ON
            reporting = False
    except GeneratorExit as e:
        if not reporting:
            raise                     # the generator is being closed
        state[1] = e                  # the progress function raised this, of all things
    except BaseException as e:        # noqa: BLE001 - whatever it is, it is the caller's to see
        state[1] = e
    yield _lib.HOST_STOP


def _owned(lock):
    """True if this thread holds the lock (an RLock)."""
    return lock._is_owned()


class _Context:
    """A library context with its lock: a context serves one thread at a time.

    The messages of the library are kept by it and fetched after each call, and the library is
    stopped through the hooks above, so that no Python code of this package runs while the
    library works. Only a progress function is called from inside, see `call`.
    """

    def __init__(self, path=None, reading=True, steps=True):
        self.lock = threading.RLock()
        #: the calls made in this context have steps between which the library can be asked to
        #: stop (conversions, rewrites, verification; not the calls on an open file)
        self._steps = steps
        #: the file the calls are about, for error messages; `reading`: it is an input
        self.path = path
        self.reading = reading
        #: the name to show in messages if the library was given another (a temporary copy)
        self.shown = None
        #: another file the calls are about (the output of a conversion), which messages may name
        self.other = None
        #: the file that the context belongs to has been closed: its handle is gone
        self._closed = False
        #: what the library calls between its steps (see _reporter), and whether it has been told
        self._reporter = None
        self._callback = None
        self._state = [None, None]
        self._hooked = False
        self.pointer = _library.xisfconv_context_new()
        if not self.pointer:
            raise MemoryError("xisfconv: cannot create a context")
        _library.xisfconv_context_keep_messages(self.pointer, 1)
        self._resource = _own(self, _library.xisfconv_context_free, self.pointer)

    @classmethod
    def borrow(cls):
        """The context of this thread. The caller holds its lock while it uses it and says
        first what the calls are about: `with context.lock: context.about(path)`. A call made
        while this thread is in another one, from a progress function or from a signal handler,
        finds the lock held and gets another context, which becomes the thread's."""
        context = getattr(_local, "context", None)
        if context is None or _owned(context.lock):
            context = _local.context = cls()
        return context

    def about(self, path=None, reading=True, other=None):
        """Says which file the next calls are about (under the lock: a signal handler that
        used the package between `borrow` and the lock has used this context)."""
        self.path = path
        self.reading = reading
        self.shown = None
        self.other = other

    def _gone(self):
        """True if the context can no longer be used: its file has been closed, or the collector
        has taken it (it frees a context before it runs the finalizers of other objects, which
        may still hold the file; the reference to itself tells)."""
        resource = self._resource
        return self._closed or resource._as_parameter_ is None or resource() is None

    def release(self):
        """Frees the context now. Handles made from it may live on."""
        with self.lock:
            _active.discard(self)
            self._forget_reporter()
            _release(self._resource)

    def _forget_reporter(self):
        reporter, self._reporter, self._hooked = self._reporter, None, False
        if reporter is not None:
            reporter.close()      # (here, where an exception has somewhere to go; not when it is collected)

    def error(self, status):
        """The exception for a status of the library."""
        name = self._name()
        message = _wording(_said(_library.xisfconv_error_message(self.pointer)) or
                           _said(_library.xisfconv_status_text(status)), (name, self._name(self.other)))
        if name and name not in message:
            message = "%s: %s" % (name, message)
        if status == _lib.ERR_CANCELLED:
            if _library.xisfconv_context_host_progress_failed(self.pointer):
                message += (" (stopped because the progress report did not come back; if an exception was the"
                            " reason, Python has printed it)")
            elif _exiting:
                message += " (Python is ending)"
        if status == _lib.ERR_MEMORY:
            return MemoryError(message)
        kind = _ERRORS.get(status, InternalError)
        if kind is FileError and name and self.reading and self.shown is None:
            try:
                if not os.path.lexists(name):
                    kind = InputNotFoundError
            except (OSError, ValueError):
                pass
        error = kind(message)
        error.status = status
        return error

    def _name(self, path=_UNSET):
        """The file the calls are about, as text."""
        if path is _UNSET:
            path = self.path
        if path is None:
            return None
        try:
            return os.fsdecode(os.fspath(path))
        except TypeError:
            return str(path)

    def _messages(self):
        """What the library had to say during the last call, taken out of the context."""
        pointer = self.pointer
        count = _library.xisfconv_context_message_count(pointer)
        if not count:
            return ()
        out = []
        level, path, text = c_int32(), c_char_p(), c_char_p()
        for index in range(count):
            if _library.xisfconv_context_message(pointer, index, byref(level), byref(path), byref(text)) == _lib.OK:
                out.append((level.value, _said(path.value), _said(text.value)))
        _library.xisfconv_context_clear_messages(pointer)
        return out

    def _ask(self, progress):
        """Tells the library what to call between its steps during the next call. Returns
        True if it will call the reporter."""
        pointer = self.pointer
        # what a call that was torn off left behind is not this call's
        if _library.xisfconv_context_message_count(pointer):
            _library.xisfconv_context_clear_messages(pointer)
        if (not self._steps or _exiting or
                (progress is None and threading.current_thread() is not threading.main_thread())):
            # the call has no steps; or there is nothing to report to and no signal handler runs
            # in this thread; or Python is ending, and the library must not come back into it
            if self._hooked:
                _library.xisfconv_context_set_host_progress(pointer, _lib.HOST_PROGRESS_FN(), None)
                self._hooked = False
            return False
        reporter = self._reporter
        if reporter is None or reporter.gi_frame is None:
            self._forget_reporter()
            self._state = state = [None, None]
            reporter = _reporter(state)
            next(reporter)                # up to its first `yield`
            callback = _lib.HOST_PROGRESS_FN(reporter.send)
            _library.xisfconv_context_set_host_progress(pointer, callback, None)
            self._callback = callback     # (kept alive here for as long as the library has it)
            self._reporter = reporter
            self._hooked = True
        elif not self._hooked:
            _library.xisfconv_context_set_host_progress(pointer, self._callback, None)
            self._hooked = True
        self._state[0] = progress
        return True

    def _finish(self, status, state):
        """The end of a call: raises what stopped it or its error, hands on its messages."""
        messages = self._messages()
        failure = state[1]
        if failure is None:
            if status == _lib.OK:
                self._emit(messages)
                return
            failure = self.error(status)
        self._emit(messages, quiet=True)
        raise failure

    def call(self, function, *arguments, progress=None, undo=None, unstarted=None):
        """Calls a function of the library that returns a status.

        `progress(stage, done, total)` is called between the steps of the work; an exception
        it raises stops the work and is raised here. So is what a signal handler of the program
        raises while the library works (KeyboardInterrupt on Ctrl-C), if this is the main
        thread. `undo` frees what the call made if this raises although the call succeeded (a
        warning turned into an error, an interrupt after the last step); it must be harmless
        if the call made nothing. `unstarted` is called if this raises before the function was
        called at all (for a function that frees what it is given).
        """
        started = False
        try:
            with self.lock:
                if self._gone():
                    raise ValueError("the file is closed")
                # This thread holds the lock: a call that is running in the context is its own,
                # further down, and whoever is here was called from inside it.
                if _library.xisfconv_context_running(self.pointer):
                    raise RuntimeError("xisfconv: this file is in use by a call that has not returned yet "
                                       "(is it used from a signal handler or from a progress function?)")
                # A handler or progress function that uses the package runs inside the call
                # that is at work, each time deeper on the stack of C. That must end before
                # the stack does.
                depth = getattr(_local, "depth", 0)
                if depth >= _DEPTH:
                    raise RecursionError("xisfconv: calls of the library are nested %d deep (a signal handler or a "
                                         "progress function that uses the package runs inside itself)" % depth)
                state = self._state
                try:
                    _active.add(self)         # (before `_ask` looks whether Python is ending: see _stop_at_exit)
                    if self._ask(progress):
                        state = self._state
                    _local.depth = depth + 1
                    if self._gone():          # (here once more: a handler may have closed the file since)
                        raise ValueError("the file is closed")
                    started = True
                    status = function(*arguments)
                    _local.depth = depth
                    _active.discard(self)
                    self._finish(status, state)
                except BaseException as error:
                    # what stopped the call, if something else is raised in its place now (a
                    # second signal): it is not forgotten
                    first = state[1]
                    if first is not None and error is not first and error.__context__ is None:
                        error.__context__ = first
                    raise
                finally:
                    # the reporter of a call that was stopped has done its work (the library
                    # has it until the next call brings a new one), and what stopped the call
                    # is not kept beyond it
                    if state[1] is not None:
                        self._reporter = None
                    state[0] = state[1] = None
                    _local.depth = depth
                    _active.discard(self)
        except BaseException:
            if not started:
                if unstarted is not None:
                    unstarted()
            elif undo is not None:
                undo()
            raise

    def quick(self, function, *arguments):
        """The same for the small accessors, which have nothing to say and take no time."""
        with self.lock:
            if self._gone():
                raise ValueError("the file is closed")
            if _library.xisfconv_context_running(self.pointer):
                raise RuntimeError("xisfconv: this file is in use by a call that has not returned yet "
                                   "(is it used from a signal handler or from a progress function?)")
            status = function(*arguments)
            if status != _lib.OK:
                raise self.error(status)

    def _emit(self, messages, quiet=False):
        names = (self._name(), self._name(self.other))
        for level, path, message in messages:
            message = _wording(message, names + (path,))
            if self.shown is not None and path:
                path = self.shown   # the library was given a copy of the file
            if level == _lib.MESSAGE_WARNING:
                text = "%s: %s" % (path, message) if path else message
                if quiet:
                    # an error is on its way: a warning turned into an exception must not hide it
                    try:
                        _warn(text)
                    except Warning:
                        pass
                else:
                    _warn(text)
            else:
                _log.info("%s: %s", path, message) if path else _log.info("%s", message)


# ------------------------------------------------------------------------------------------
# Names of the choices
# ------------------------------------------------------------------------------------------

_FORMATS = {"xisf": _lib.FORMAT_XISF, "fits": _lib.FORMAT_FITS, "fit": _lib.FORMAT_FITS, "fts": _lib.FORMAT_FITS,
            "asdf": _lib.FORMAT_ASDF, "tiff": _lib.FORMAT_TIFF, "tif": _lib.FORMAT_TIFF, "png": _lib.FORMAT_PNG}
_FORMAT_NAMES = {_lib.FORMAT_XISF: "xisf", _lib.FORMAT_FITS: "fits", _lib.FORMAT_ASDF: "asdf",
                 _lib.FORMAT_TIFF: "tiff", _lib.FORMAT_PNG: "png"}
_CODECS = {"none": _lib.CODEC_NONE, "zlib": _lib.CODEC_ZLIB, "lz4": _lib.CODEC_LZ4, "lz4hc": _lib.CODEC_LZ4HC,
           "zstd": _lib.CODEC_ZSTD, "default": _lib.CODEC_DEFAULT}
_CHECKSUMS = {"none": _lib.CHECKSUM_NONE, "sha1": _lib.CHECKSUM_SHA1, "sha-1": _lib.CHECKSUM_SHA1,
              "sha256": _lib.CHECKSUM_SHA256, "sha-256": _lib.CHECKSUM_SHA256, "sha512": _lib.CHECKSUM_SHA512,
              "sha-512": _lib.CHECKSUM_SHA512, "sha3-256": _lib.CHECKSUM_SHA3_256, "sha3-512": _lib.CHECKSUM_SHA3_512}
_ROWS = {"top-down": _lib.ROWS_TOP_DOWN, "bottom-up": _lib.ROWS_BOTTOM_UP}
_ROW_NAMES = {_lib.ROWS_TOP_DOWN: "top-down", _lib.ROWS_BOTTOM_UP: "bottom-up"}
_STRETCHES = {"none": _lib.STRETCH_NONE, "auto": _lib.STRETCH_AUTO, "linked": _lib.STRETCH_LINKED,
              "unlinked": _lib.STRETCH_UNLINKED, "stored": _lib.STRETCH_STORED}
_COLOR_NAMES = {_lib.COLOR_GRAY: "gray", _lib.COLOR_RGB: "rgb", _lib.COLOR_OTHER: "other"}
_VERDICTS = {_lib.VERDICT_OK: "ok", _lib.VERDICT_NOT_FULLY_CHECKED: "not fully checked", _lib.VERDICT_FAILED: "failed"}

_SAMPLES = {"uint8": _lib.SAMPLE_UINT8, "uint16": _lib.SAMPLE_UINT16, "uint32": _lib.SAMPLE_UINT32,
            "uint64": _lib.SAMPLE_UINT64, "float32": _lib.SAMPLE_FLOAT32, "float64": _lib.SAMPLE_FLOAT64}
_SAMPLE_ALIASES = {"u8": "uint8", "u16": "uint16", "u32": "uint32", "u64": "uint64", "f32": "float32",
                   "f64": "float64"}
_SAMPLE_NAMES = {number: name for name, number in _SAMPLES.items()}


def _choice(value, table, what, default=None):
    if value is None:
        if default is None:
            raise ValueError("%s is needed" % what)
        return default
    if not isinstance(value, str):
        raise TypeError("%s must be a string, not %s" % (what, type(value).__name__))
    key = value.strip().lower().replace("_", "-")
    try:
        return table[key]
    except KeyError:
        raise ValueError("unknown %s %r; one of: %s" % (what, value, ", ".join(sorted(set(table))))) from None


def _sample_format(value, none=_lib.SAMPLE_AS_STORED):
    """A sample format from a name ("uint16", "u16") or anything NumPy takes for a dtype."""
    if value is None:
        return none
    if isinstance(value, str):
        name = _SAMPLE_ALIASES.get(value.strip().lower(), value.strip().lower())
        if name in _SAMPLES:
            return _SAMPLES[name]
    try:
        name = np.dtype(value).name
    except TypeError:
        raise ValueError("unknown sample format %r" % (value,)) from None
    if name not in _SAMPLES:
        raise ValueError("sample format %s is not available; one of: %s" % (name, ", ".join(_SAMPLES)))
    return _SAMPLES[name]


def _dtype(sample_format):
    return np.dtype(_SAMPLE_NAMES[sample_format])


def _rows(value, default=_lib.ROWS_DEFAULT):
    return _choice(value, _ROWS, "row order", default) if value is not None else default


def _channels_last(value):
    if value == "last":
        return True
    if value == "first":
        return False
    raise ValueError("channels must be 'last' or 'first', not %r" % (value,))


def _bounds(value):
    """(use, lower, upper) for the option structures."""
    if value is None:
        return 0, 0.0, 1.0
    try:
        lower, upper = value
        lower, upper = float(lower), float(upper)
    except (TypeError, ValueError):
        raise ValueError("bounds must be a pair (lower, upper), not %r" % (value,)) from None
    if not upper > lower:
        raise ValueError("the upper bound must be above the lower bound")
    return 1, lower, upper


def _image_choice(value):
    if value is None:
        return _lib.ALL_IMAGES
    index = operator.index(value)
    if index < 0:
        raise ImageIndexError("image index %d is negative" % index)
    return index


def _subblock(value, default):
    if value is None:
        return default
    size = int(value)
    if size <= 0:
        raise ValueError("the subblock size must be positive")
    return size


# ------------------------------------------------------------------------------------------
# Keywords
# ------------------------------------------------------------------------------------------

Card = namedtuple("Card", "name value comment", defaults=(None, ""))
Card.__doc__ = """One FITS card: ``name``, ``value`` (str, bool, int, float or None) and ``comment``.
A COMMENT or HISTORY card has its text as value."""

_COMMENTARY = ("COMMENT", "HISTORY", "")
# (the digits 0 to 9 only: \d would take the digits of every script)
_NUMBER = r"[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[EeDd][+-]?[0-9]+)?"
_INTEGER = re.compile(r"[+-]?[0-9]+\Z")
_REAL = re.compile(_NUMBER + r"\Z")
_COMPLEX = re.compile(r"\(\s*(%s)\s*,\s*(%s)\s*\)\Z" % (_NUMBER, _NUMBER))


def _parse_value(raw, text):
    """The Python value of a FITS-formatted value."""
    raw = raw.strip()
    if not raw:
        return None
    if raw[0] == "'":
        return text
    if raw == "T":
        return True
    if raw == "F":
        return False
    if _INTEGER.match(raw):
        try:
            return int(raw)
        except ValueError:   # more digits than Python converts
            return raw
    if _REAL.match(raw):
        return float(raw.replace("D", "E").replace("d", "e"))
    pair = _COMPLEX.match(raw)
    if pair:
        return complex(float(pair.group(1).replace("D", "E").replace("d", "e")),
                       float(pair.group(2).replace("D", "E").replace("d", "e")))
    return raw   # whatever else a file holds: as it is written


def _make_card(name, value=None, comment=""):
    if not isinstance(name, str):
        raise TypeError("a keyword name must be a string, not %s" % type(name).__name__)
    name = name.strip()   # blanks around a name mean nothing in FITS
    if isinstance(value, np.generic):
        value = value.item()
    elif type(value).__name__ == "Undefined":   # astropy's empty value
        value = None
    comment = "" if comment is None else str(comment)
    if name.strip().upper() in _COMMENTARY:
        # the text of a COMMENT or HISTORY card is its value, as in astropy; it is also taken
        # from the comment, and from both if both are given: such a card has one text
        if value is None or value == "":
            value = comment
        elif comment:
            value = "%s %s" % (value, comment)
        value, comment = str(value), ""
    elif value is not None and not isinstance(value, (str, bool, int, float, complex)):
        raise TypeError("keyword %s: a value must be a string, a number, True, False or None, not %s" %
                        (name, type(value).__name__))
    return Card(name, value, comment)


class Keywords(MutableSequence):
    """FITS keywords of an image: a list of :class:`Card`, in order.

    Indexing with a number gives a card; with a name, the value of the first card of that
    name (the case does not matter)::

        keywords["EXPTIME"]          # 300.0
        keywords.get("FILTER", "L")
        keywords["OBJECT"] = "M 31"  # replaces the value, or adds a card
        keywords.append("HISTORY", "calibrated")

    It is made from another ``Keywords``, a list of ``(name, value)`` or ``(name, value,
    comment)``, a dict (a value may be a ``(value, comment)`` pair), or an astropy ``Header``.

    Keywords read from a file remember how their values were written, so that they go back
    out unchanged as long as the value is not replaced.
    """

    __slots__ = ("_cards", "_raw", "fit_summary")

    def __init__(self, cards=None):
        self._cards = []
        self._raw = []
        #: for keywords from :meth:`FileImage.wcs_keywords`: one line on the quality of a fit
        self.fit_summary = ""
        if cards is None:
            return
        if isinstance(cards, (str, bytes)):
            raise TypeError("keywords are a list of cards, a dict or a header, not a string")
        if isinstance(cards, Keywords):
            self._cards = list(cards._cards)
            self._raw = list(cards._raw)
            self.fit_summary = cards.fit_summary
        elif hasattr(cards, "cards") and hasattr(cards, "comments"):   # an astropy Header
            for card in cards.cards:
                self.append(card.keyword, card.value, card.comment)
        elif isinstance(cards, Mapping) or (hasattr(cards, "keys") and hasattr(cards, "__getitem__")):
            for name in cards.keys():
                value = cards[name]
                if isinstance(value, tuple) and len(value) == 2:
                    self.append(name, value[0], value[1])
                else:
                    self.append(name, value)
        else:
            for card in cards:
                self.append(card)

    # --- as a sequence of cards -----------------------------------------------------------

    def __len__(self):
        return len(self._cards)

    def _find(self, name):
        key = name.strip().upper()
        for index, card in enumerate(self._cards):
            if card.name.upper() == key:
                return index
        return -1

    def __getitem__(self, key):
        if isinstance(key, str):
            index = self._find(key)
            if index < 0:
                raise KeyError(key)
            return self._cards[index].value
        if isinstance(key, slice):
            out = Keywords()
            out._cards = self._cards[key]
            out._raw = self._raw[key]
            return out
        return self._cards[key]

    def __setitem__(self, key, value):
        if isinstance(key, str):
            index = self._find(key)
            pair = isinstance(value, tuple) and len(value) == 2      # (value, comment)
            if index < 0:
                self.append(key, value[0], value[1]) if pair else self.append(key, value)
            else:
                old = self._cards[index]
                self._cards[index] = _make_card(old.name, value[0], value[1]) if pair \
                    else _make_card(old.name, value, old.comment)
                self._raw[index] = None
            return
        if isinstance(key, slice):
            raise TypeError("Keywords does not take slice assignment")
        self._cards[key] = _make_card(*value) if not isinstance(value, str) else _make_card(value)
        self._raw[key] = None

    def __delitem__(self, key):
        if isinstance(key, str):
            wanted = key.strip().upper()
            keep = [i for i, card in enumerate(self._cards) if card.name.upper() != wanted]
            if len(keep) == len(self._cards):
                raise KeyError(key)
            self._cards = [self._cards[i] for i in keep]
            self._raw = [self._raw[i] for i in keep]
            return
        del self._cards[key]
        del self._raw[key]

    def insert(self, index, card):
        self._cards.insert(index, _make_card(*card) if not isinstance(card, str) else _make_card(card))
        self._raw.insert(index, None)

    def append(self, name, value=None, comment=""):
        """Adds a card: ``append("EXPTIME", 300.0, "seconds")`` or ``append(card)``."""
        if isinstance(name, str):
            self._cards.append(_make_card(name, value, comment))
        else:
            self._cards.append(_make_card(*name))
        self._raw.append(None)

    def __contains__(self, item):
        if isinstance(item, str):
            return self._find(item) >= 0
        return item in self._cards

    def __iter__(self):
        return iter(self._cards)

    def __eq__(self, other):
        if isinstance(other, Keywords):
            return self._cards == other._cards
        if isinstance(other, (list, tuple)):
            return self._cards == list(other)
        return NotImplemented

    __hash__ = None

    def __repr__(self):
        if len(self._cards) <= 6:
            return "Keywords(%r)" % ([tuple(c) for c in self._cards],)
        return "<Keywords: %d cards, %s ...>" % (len(self._cards), ", ".join(c.name for c in self._cards[:6]))

    # --- by name --------------------------------------------------------------------------

    def get(self, name, default=None):
        """The value of the first card with this name, or ``default``."""
        index = self._find(name)
        return default if index < 0 else self._cards[index].value

    def cards(self, name):
        """Every card with this name: all the HISTORY lines, for one."""
        key = name.strip().upper()
        return [card for card in self._cards if card.name.upper() == key]

    def names(self):
        """The names, in order, each once."""
        seen = {}
        for card in self._cards:
            seen.setdefault(card.name, None)
        return list(seen)

    def to_dict(self):
        """``{name: value}`` of the first card of each name, without COMMENT and HISTORY."""
        out = {}
        for card in self._cards:
            if card.name.strip().upper() not in _COMMENTARY:
                out.setdefault(card.name, card.value)
        return out

    def copy(self):
        return Keywords(self)

    def fits_text(self):
        """The keywords as the cards of a FITS header: 80 characters each, one after the other,
        without the END card. This is how the FITS writer of the library formats them: long
        strings on CONTINUE cards, HIERARCH for names that do not fit a standard card, text
        reduced to printable ASCII, and without the cards that describe how a FITS file stores
        its data (SIMPLE, BITPIX, NAXIS, BZERO and the like)."""
        context = _Context.borrow()
        with context.lock:
            context.about()
            handle = self._to_handle(context)
            try:
                text = c_void_p()
                length = c_size_t()
                context.call(_library.xisfconv_keywords_fits_text, handle, byref(text), byref(length))
                return ctypes.string_at(text.value, length.value).decode("ascii", "replace") if text.value else ""
            finally:
                _library.xisfconv_keywords_free(handle)

    # --- to and from the library ----------------------------------------------------------

    @classmethod
    def _from_handle(cls, context, handle):
        out = cls()
        name, value, comment, text = c_char_p(), c_char_p(), c_char_p(), c_char_p()
        with context.lock:
            for index in range(_library.xisfconv_keywords_count(handle)):
                context.quick(_library.xisfconv_keywords_get, handle, index, byref(name), byref(value), byref(comment))
                raw = _text(value.value)
                card_name = _text(name.value).strip()
                card_comment = _text(comment.value)
                if card_name.upper() in _COMMENTARY:
                    # text only; a file may have it as the value, as the comment, or as both
                    words = " ".join(part for part in (raw.strip(), card_comment) if part)
                    out._cards.append(Card(card_name, words, ""))
                    out._raw.append(None)
                    continue
                unquoted = None
                if raw.lstrip().startswith("'"):
                    context.quick(_library.xisfconv_keywords_get_text, handle, index, byref(text))
                    unquoted = _text(text.value)
                out._cards.append(Card(card_name, _parse_value(raw, unquoted), card_comment))
                out._raw.append(raw)
        return out

    def _to_handle(self, context):
        """A keyword list of the library; the caller frees it."""
        handle = c_void_p()
        context.quick(_library.xisfconv_keywords_new, context.pointer, byref(handle))
        try:
            for card, raw in zip(self._cards, self._raw):
                name = _bytes(card.name, "a keyword name")
                comment = _bytes(card.comment, "a keyword comment") if card.comment else None
                value = card.value
                if card.name.strip().upper() in _COMMENTARY:
                    context.quick(_library.xisfconv_keywords_append, handle, name, None,
                                  _bytes("" if value is None else str(value), "a keyword text"))
                elif raw is not None:
                    context.quick(_library.xisfconv_keywords_append, handle, name, _bytes(raw), comment)
                elif value is None:
                    context.quick(_library.xisfconv_keywords_append, handle, name, None, comment)
                elif isinstance(value, bool):
                    context.quick(_library.xisfconv_keywords_append, handle, name, b"T" if value else b"F", comment)
                elif isinstance(value, int):
                    context.quick(_library.xisfconv_keywords_append, handle, name, str(value).encode(), comment)
                elif isinstance(value, float):
                    if not math.isfinite(value):
                        raise ValueError("keyword %s: FITS has no way to write %r" % (card.name, value))
                    context.quick(_library.xisfconv_keywords_append_number, handle, name, value, comment)
                elif isinstance(value, complex):
                    if not (math.isfinite(value.real) and math.isfinite(value.imag)):
                        raise ValueError("keyword %s: FITS has no way to write %r" % (card.name, value))
                    context.quick(_library.xisfconv_keywords_append, handle, name,
                                  ("(%r, %r)" % (value.real, value.imag)).encode(), comment)
                else:
                    context.quick(_library.xisfconv_keywords_append_string, handle, name,
                                  _bytes(value, "a keyword value"), comment)
        except BaseException:
            _library.xisfconv_keywords_free(handle)
            raise
        return handle


def _keywords_result(context, function, *arguments):
    """Calls a function that makes a keyword list and a summary line; returns them as Keywords."""
    handle = c_void_p()
    summary = c_char_p()
    context.call(function, *arguments, byref(handle), byref(summary),
                 undo=lambda: _library.xisfconv_keywords_free(handle))
    try:
        out = Keywords._from_handle(context, handle)
        out.fit_summary = _text(summary.value)
    finally:
        _library.xisfconv_keywords_free(handle)
    return out


def wcs_flip_rows(keywords, height):
    """WCS keywords converted between the bottom-up and the top-down pixel convention
    (CRPIX2, CD/PC/CDELT, SIP coefficients), as new :class:`Keywords`. ``height`` is the number
    of rows of the image. Twice gives the original values back."""
    height = int(height)
    if height <= 0:
        raise ValueError("the image height is needed")
    context = _Context.borrow()
    with context.lock:
        context.about()
        handle = Keywords(keywords)._to_handle(context)
        try:
            context.call(_library.xisfconv_wcs_flip_rows, handle, height)
            return Keywords._from_handle(context, handle)
        finally:
            _library.xisfconv_keywords_free(handle)


# ------------------------------------------------------------------------------------------
# Stretch
# ------------------------------------------------------------------------------------------

StretchParams = namedtuple("StretchParams", "shadows midtones highlights low high", defaults=(0.0, 1.0))
StretchParams.__doc__ = """One histogram transformation in PixInsight's STF form, on values normalized to [0,1]:
``x1 = clip((x - shadows) / (highlights - shadows))``, ``x2 = MTF(midtones, x1)``,
``y = clip((x2 - low) / (high - low))``."""


def _planar(data, channels):
    """The array as the library takes it: [channels, height, width], contiguous, native byte
    order. Returns (array, channel count)."""
    last = _channels_last(channels)
    array = np.asanyarray(data)
    if isinstance(array, np.ma.MaskedArray):
        raise TypeError("a masked array cannot be written; fill it first (array.filled(value))")
    array = np.asarray(array)
    if array.dtype.name not in _SAMPLES:
        raise TypeError("the samples must be uint8, uint16, uint32, uint64, float32 or float64, not %s; "
                        "convert the array first" % array.dtype.name)
    if array.ndim == 2:
        planar = array[np.newaxis]
    elif array.ndim == 3:
        planar = np.moveaxis(array, -1, 0) if last else array
    else:
        raise ValueError("an image is a 2-D array, or a 3-D array with the channels %s; this array has %d "
                         "dimension(s)" %
                         ("last" if last else "first", array.ndim))
    if 0 in planar.shape:
        raise ValueError("the image is empty: shape %r" % (array.shape,))
    if not planar.flags.c_contiguous or not planar.dtype.isnative:
        planar = np.ascontiguousarray(planar, dtype=planar.dtype.newbyteorder("="))
    return planar, planar.shape[0]


def _arranged(planar, channels):
    """[channels, height, width] as the caller wants it: 2-D for one channel, else the channels
    last (a view) or first."""
    if planar.shape[0] == 1:
        return planar[0]
    return np.moveaxis(planar, 0, -1) if _channels_last(channels) else planar


def _stretch_array(params):
    try:
        items = [StretchParams(*p) for p in params]
    except TypeError:
        raise TypeError("the stretch is a list of StretchParams, one per colour channel") from None
    if not items:
        raise ValueError("the stretch is empty")
    array = (_lib.StretchParams * len(items))()
    for target, item in zip(array, items):
        target.shadows, target.midtones, target.highlights, target.low, target.high = (float(v) for v in item)
    return array


def auto_stretch(data, *, linked=True, bounds=None, channels="last", color_channels=None):
    """PixInsight-style auto-STF of an image: shadows at median - 2.8 MADN, background to 0.25.

    Returns a list of :class:`StretchParams`, one per colour channel. ``linked`` shares the
    averaged statistics between the channels, which keeps the colour balance. ``bounds`` is the
    range of floating point samples (0 to 1 if not given); integers use their full range.
    ``color_channels`` defaults to all channels, at most 3 (a fourth is taken as alpha).
    """
    planar, count = _planar(data, channels)
    _, lower, upper = _bounds(bounds)
    colors = min(count, 3) if color_channels is None else int(color_channels)
    if not 1 <= colors <= count:
        raise ValueError("color_channels must be between 1 and the number of channels")
    out = (_lib.StretchParams * colors)()
    context = _Context.borrow()
    with context.lock:
        context.about()
        context.call(_library.xisfconv_auto_stretch, context.pointer, planar.ctypes.data_as(c_void_p), planar.shape[2],
                     planar.shape[1], count, _SAMPLES[planar.dtype.name], lower, upper, colors, int(bool(linked)), out)
        return [StretchParams(p.shadows, p.midtones, p.highlights, p.low, p.high) for p in out]


def apply_stretch(data, params, *, bounds=None, channels="last"):
    """Applies a stretch: returns float32 samples in [0,1], in the shape of ``data``.

    ``params`` is a list of :class:`StretchParams` for the first channels; further (alpha)
    channels are only normalized. ``bounds`` as for :func:`auto_stretch`.
    """
    planar, count = _planar(data, channels)
    _, lower, upper = _bounds(bounds)
    array = _stretch_array(params)
    if len(array) > count:
        raise ValueError("%d stretches for an image of %d channel(s)" % (len(array), count))
    out = np.empty(planar.shape, np.float32)
    context = _Context.borrow()
    with context.lock:
        context.about()
        context.call(_library.xisfconv_apply_stretch, context.pointer, planar.ctypes.data_as(c_void_p), planar.shape[2],
                     planar.shape[1], count, _SAMPLES[planar.dtype.name], lower, upper, array, len(array),
                     out.ctypes.data_as(c_void_p))
        if np.ndim(data) == 2:
            return out[0]
        return np.moveaxis(out, 0, -1) if _channels_last(channels) else out


# ------------------------------------------------------------------------------------------
# Images in memory
# ------------------------------------------------------------------------------------------


class Image:
    """An image in memory: the pixels and what describes them. This is what
    :func:`read_image` returns and what :func:`write` takes.

    data
        2-D ``[height, width]``, or 3-D with the channels last ``[height, width, channels]``
        or first ``[channels, height, width]``, as ``channels`` says. uint8, uint16, uint32,
        uint64, float32 or float64.
    keywords
        :class:`Keywords`, or anything it is made from; may be None.
    name
        XISF id, FITS EXTNAME, TIFF description.
    bounds
        ``(lower, upper)``: the range of floating point samples. None: 0:1 when the data fits,
        else 0:65535 when it fits, else minimum and maximum.
    icc_profile
        bytes, written to XISF, TIFF and PNG.
    row_order
        "top-down" (the first row is the top of the image) or "bottom-up", of ``data``.
    channels
        "last" or "first".
    wcs_row_order
        The row order the WCS keywords describe, if it is not that of ``data``. An image read
        from an XISF file has "bottom-up" here: that is how PixInsight writes WCS keywords.
    properties
        XISF properties of an image that was read, as a dict. They are not written.
    color_space
        "gray", "rgb" or "other", of an image that was read. Writing goes by the number of
        channels: 3 are RGB.
    """

    __slots__ = ("data", "keywords", "name", "bounds", "icc_profile", "row_order", "channels", "wcs_row_order",
                 "properties", "color_space")

    def __init__(self, data, *, keywords=None, name=None, bounds=None, icc_profile=None, row_order="top-down",
                 channels="last", wcs_row_order=None, properties=None, color_space=None):
        self.data = data
        self.keywords = keywords if keywords is None or isinstance(keywords, Keywords) else Keywords(keywords)
        self.name = name
        self.bounds = bounds
        self.icc_profile = icc_profile
        self.row_order = row_order
        self.channels = channels
        self.wcs_row_order = wcs_row_order
        self.properties = {} if properties is None else properties
        self.color_space = color_space

    def __repr__(self):
        array = np.asanyarray(self.data)
        return "<xisfconv.Image %s%s %s, rows %s, channels %s>" % (
            repr(self.name) + " " if self.name else "", "x".join(str(n) for n in array.shape), array.dtype.name,
            self.row_order, self.channels)


# ------------------------------------------------------------------------------------------
# Files
# ------------------------------------------------------------------------------------------


class Properties(Mapping):
    """The XISF properties of an image, or of a file: ``{id: value}``, read when asked for.

    Scalars are bool, int, float or str; numeric vectors and matrices are float64 arrays.
    A property whose value this library does not read is None. :meth:`type` and
    :meth:`comment` give the XISF type name and the comment of a property.
    For FITS and ASDF files it is empty.
    """

    def __init__(self, file, image):
        self._file = file
        self._image = image
        self._ids = None

    def _load(self):
        if self._ids is None:
            file = self._file
            ids = {}
            name = c_char_p()
            with file._context.lock:
                pointer = file._pointer()
                for index in range(_library.xisfconv_property_count(pointer, self._image)):
                    file._context.quick(_library.xisfconv_property_get, pointer, self._image, index, byref(name), None,
                                        None, None, None)
                    ids.setdefault(_text(name.value), index)
            self._ids = ids
        return self._ids

    def _get(self, key):
        index = self._load()[key]
        file = self._file
        kind, value, comment, block = c_char_p(), c_char_p(), c_char_p(), c_int32()
        with file._context.lock:
            file._context.quick(_library.xisfconv_property_get, file._pointer(), self._image, index, None, byref(kind),
                                byref(value), byref(comment), byref(block))
            return _text(kind.value), _text(value.value), _text(comment.value), bool(block.value)

    def __len__(self):
        return len(self._load())

    def __iter__(self):
        return iter(self._load())

    def __contains__(self, key):
        return key in self._load()

    def __getitem__(self, key):
        kind, value, _, block = self._get(key)
        if block:
            return self._read_block(key, kind)
        text = value.strip()
        try:
            if kind == "Boolean":
                return text.lower() in ("1", "true")
            if kind.startswith(("Int", "UInt")):
                return int(text)
            if kind.startswith("Float"):
                return float(text)
        except ValueError:
            pass
        return value

    def _read_block(self, key, kind):
        file = self._file
        rows, columns = c_size_t(), c_size_t()
        name = _bytes(key)
        with file._context.lock:
            pointer = file._pointer()
            try:
                file._context.call(_library.xisfconv_property_read_f64, pointer, self._image, name, None, 0,
                                   byref(rows), byref(columns))
                out = np.empty((rows.value, columns.value), np.float64)
                file._context.call(_library.xisfconv_property_read_f64, pointer, self._image, name,
                                   out.ctypes.data_as(c_void_p), out.size, byref(rows), byref(columns))
            except (NotFoundError, UnsupportedError):
                return None   # a type that is not read as numbers
        return out if kind.endswith("Matrix") else out.reshape(-1)

    def type(self, key):
        """The XISF type name: "Float64", "String", "F64Matrix", "TimePoint"."""
        return self._get(key)[0]

    def comment(self, key):
        return self._get(key)[2]

    def __repr__(self):
        return "<xisfconv.Properties: %d>" % len(self)


class FileImage:
    """One image of an open :class:`File`. The attributes describe it; :meth:`read` gives the
    pixels, :meth:`read_image` the pixels with their keywords."""

    def __init__(self, file, index):
        self._file = file
        #: number of the image in the file, from 0
        self.index = index

    def __eq__(self, other):
        if isinstance(other, FileImage):
            return self._file is other._file and self.index == other.index
        return NotImplemented

    def __hash__(self):
        return hash((id(self._file), self.index))

    # --- description ----------------------------------------------------------------------

    def _info(self):
        file = self._file
        info = _lib.struct(_lib.ImageInfo, _library.xisfconv_image_info_init)
        with file._context.lock:
            file._context.quick(_library.xisfconv_image_info_get, file._pointer(), self.index, byref(info))
        return info

    def _string(self, function, *arguments):
        file = self._file
        with file._context.lock:
            return _text(function(file._pointer(), self.index, *arguments))

    @property
    def name(self):
        """XISF: the id; FITS and ASDF: EXTNAME or HDUNAME, or the place of a plain array in the
        tree. "" if the image has none."""
        return self._string(_library.xisfconv_image_name)

    @property
    def width(self):
        return int(self._info().width)

    @property
    def height(self):
        return int(self._info().height)

    @property
    def channels(self):
        """Number of channels (planes)."""
        return int(self._info().channels)

    @property
    def shape(self):
        """Shape of the array :meth:`read` returns by default: ``(height, width)``, or
        ``(height, width, channels)``."""
        info = self._info()
        if info.channels == 1:
            return (int(info.height), int(info.width))
        return (int(info.height), int(info.width), int(info.channels))

    @property
    def dtype(self):
        """NumPy dtype of the samples :meth:`read` returns by default. For FITS and ASDF it
        depends on the data and is None until the image has been read once."""
        info = self._info()
        if not info.data_known or not info.convertible:
            return None
        return _dtype(info.sample_format)

    @property
    def bounds(self):
        """``(lower, upper)``: the range of floating point samples; None for integers, and for
        FITS and ASDF until the image has been read once."""
        info = self._info()
        if not info.data_known or info.sample_format not in (_lib.SAMPLE_FLOAT32, _lib.SAMPLE_FLOAT64):
            return None
        return (info.lower_bound, info.upper_bound)

    @property
    def color_space(self):
        """"gray", "rgb" or "other" (XISF: CIELab and others, read as raw channels)."""
        return _COLOR_NAMES.get(self._info().color_space, "other")

    @property
    def row_order(self):
        """The order the rows are stored in: "top-down" or "bottom-up"."""
        return _ROW_NAMES.get(self._info().row_order, "top-down")

    @property
    def row_order_declared(self):
        """False if the row order is assumed (FITS or ASDF without ROWORDER)."""
        return bool(self._info().row_order_declared)

    @property
    def wcs_row_order(self):
        """The row order the WCS keywords in :attr:`keywords` describe. XISF: always
        "bottom-up", although the rows are stored top-down."""
        return _ROW_NAMES.get(self._info().wcs_row_order, "bottom-up")

    @property
    def readable(self):
        """False if the pixels cannot be read; :attr:`unsupported_reason` says why."""
        return bool(self._info().convertible)

    @property
    def unsupported_reason(self):
        return self._string(_library.xisfconv_image_unsupported_reason)

    @property
    def has_icc_profile(self):
        return bool(self._info().has_icc_profile)

    @property
    def has_stored_stretch(self):
        """True if the XISF image has a saved STF that is not the identity."""
        return bool(self._info().has_stored_stretch)

    @property
    def has_astrometric_solution(self):
        """True if the image has PixInsight solution properties or WCS keywords."""
        return bool(self._info().has_astrometric_solution)

    @property
    def cfa(self):
        """Colour filter array of an XISF image: ``(pattern, width, height)`` such as
        ``("RGGB", 2, 2)``, for the rows as stored; None if there is none."""
        info = self._info()
        if not info.has_cfa:
            return None
        pattern = self.detail("cfaPattern") or _text(info.cfa_pattern)
        return (pattern, int(info.cfa_width), int(info.cfa_height))

    @property
    def resolution(self):
        """``(horizontal, vertical, unit)`` in pixels per "inch" or "cm"; None if not stated."""
        info = self._info()
        if info.resolution_unit not in (1, 2):
            return None
        return (info.resolution_x, info.resolution_y, "inch" if info.resolution_unit == 1 else "cm")

    @property
    def bitpix(self):
        """FITS and ASDF: BITPIX of the stored data; 0 for XISF."""
        return int(self._info().bitpix)

    @property
    def bscale(self):
        return self._info().bscale

    @property
    def bzero(self):
        return self._info().bzero

    @property
    def source_index(self):
        """FITS: the number of the HDU; ASDF: the running number of the array."""
        return int(self._info().source_index)

    @property
    def plain_array(self):
        """ASDF: True for an array that is not an HDU of a FITS-tagged node."""
        return bool(self._info().plain_array)

    def detail(self, name):
        """A detail of the image as text, "" if it has none. XISF: "sampleFormat", "colorSpace",
        "pixelStorage", "byteOrder", "location", "compression", "subblocks", "checksum",
        "imageType", "orientation", "cfaPattern", "cfaName", "resolutionUnit". FITS:
        "tileCompression", "mapping". ASDF: "source", "storage", "mapping"."""
        return self._string(_library.xisfconv_image_detail, _bytes(name))

    @property
    def keywords(self):
        """The FITS keywords the file holds for this image, as :class:`Keywords` (a copy)."""
        file = self._file
        handle = c_void_p()
        with file._context.lock:
            file._context.quick(_library.xisfconv_image_keywords, file._pointer(), self.index, byref(handle))
            return Keywords._from_handle(file._context, handle)

    @property
    def properties(self):
        """The XISF properties of the image: :class:`Properties`."""
        return Properties(self._file, self.index)

    @property
    def icc_profile(self):
        """The ICC profile as bytes, or None."""
        file = self._file
        size = c_size_t()
        with file._context.lock:
            pointer = file._pointer()
            try:
                file._context.call(_library.xisfconv_read_icc_profile, pointer, self.index, None, 0, byref(size))
            except NotFoundError:
                return None
            buffer = ctypes.create_string_buffer(max(size.value, 1))
            file._context.call(_library.xisfconv_read_icc_profile, pointer, self.index, buffer, size.value,
                               byref(size))
            return buffer.raw[:size.value]

    @property
    def stored_stretch(self):
        """The STF saved in an XISF image: a list of :class:`StretchParams`, one per colour
        channel; None if the image has none."""
        file = self._file
        count = c_size_t()
        with file._context.lock:
            pointer = file._pointer()
            try:
                file._context.quick(_library.xisfconv_stored_stretch, pointer, self.index, None, 0, byref(count))
            except NotFoundError:
                return None
            array = (_lib.StretchParams * max(count.value, 1))()
            file._context.quick(_library.xisfconv_stored_stretch, pointer, self.index, array, count.value,
                                byref(count))
        return [StretchParams(p.shadows, p.midtones, p.highlights, p.low, p.high) for p in array[:count.value]]

    # --- astrometry and headers -----------------------------------------------------------

    def wcs_keywords(self, row_order="bottom-up", *, sip_order=3):
        """The WCS keywords of the image (CTYPE, CRVAL, CRPIX, CD, SIP terms) for pixel rows
        in ``row_order``, as :class:`Keywords`.

        If the image carries WCS keywords, those are converted. An XISF image with a
        PixInsight solution and no WCS keywords gets them built from the solution; its spline
        distortion is fitted with SIP polynomials of order ``sip_order`` (2 to 7; 0: linear
        only), and ``fit_summary`` of the result says how well. :class:`NotFoundError` if the
        image has no solution.
        """
        file = self._file
        with file._context.lock:
            return _keywords_result(file._context, _library.xisfconv_wcs_keywords, file._pointer(), self.index,
                                    _rows(row_order, _lib.ROWS_BOTTOM_UP), int(sip_order))

    def fits_keywords(self, row_order="bottom-up", *, property_keywords=True, wcs=True, sip_order=3):
        """The cards of the image as a FITS header, for pixel rows in ``row_order``: what
        :func:`convert` writes to a FITS file, without its HISTORY lines.

        For an XISF image: its FITS keywords; keywords derived from properties where it has
        none (``property_keywords``: OBJECT, EXPTIME, FOCALLEN, DATE-OBS and others); BAYERPAT
        and WCS keywords for ``row_order``; WCS keywords built from a PixInsight solution
        (``wcs``, ``sip_order``). For FITS and ASDF: the cards of the image, converted if
        ``row_order`` is not the order of the stored rows.
        """
        file = self._file
        with file._context.lock:
            return _keywords_result(file._context, _library.xisfconv_fits_keywords, file._pointer(), self.index,
                                    _rows(row_order, _lib.ROWS_BOTTOM_UP), int(bool(property_keywords)),
                                    int(bool(wcs)), int(sip_order))

    # --- pixels ---------------------------------------------------------------------------

    def read(self, sample_format=None, *, row_order="top-down", channels="last", verify=True, bounds=None):
        """Reads the pixels into a NumPy array.

        The array is ``[height, width]`` for one channel, else ``[height, width, channels]``
        (``channels="last"``, a view of planar memory) or ``[channels, height, width]``
        (``channels="first"``). Row 0 is the top of the image with ``row_order="top-down"``,
        the bottom with "bottom-up" (the FITS convention); None gives the rows as stored.

        ``sample_format`` converts the samples: a name ("uint16", "float32"; also "u16",
        "f32") or a NumPy dtype. The conversion rescales, it does not cast: integers to
        integers over the full ranges, integers to floating point normalized to [0,1],
        floating point to integers with ``bounds`` (default: those of the image) mapped to
        the full integer range. Floating point to floating point leaves the values unchanged.

        ``verify=False`` skips the checksums of the file.
        """
        file = self._file
        options = _lib.struct(_lib.ReadOptions, _library.xisfconv_read_options_init)
        options.sample_format = _sample_format(sample_format)
        options.row_order = _rows(row_order)
        options.verify_checksums = int(bool(verify))
        options.use_bounds, options.lower_bound, options.upper_bound = _bounds(bounds)
        _channels_last(channels)
        context = file._context
        with context.lock:
            pointer = file._pointer()
            # FITS and ASDF: the sample format depends on the data, so the pixels are read
            # now and handed over below. XISF: nothing happens here.
            context.call(_library.xisfconv_load_pixels, pointer, self.index, options.verify_checksums)
            size = c_uint64()
            context.call(_library.xisfconv_pixels_size, pointer, self.index, byref(options), byref(size))
            info = self._info()
            sample = options.sample_format or info.sample_format
            planar = np.empty((int(info.channels), int(info.height), int(info.width)), _dtype(sample))
            if planar.nbytes != size.value:
                raise InternalError("the library asks for %d bytes for an image of %d" % (size.value, planar.nbytes))
            context.call(_library.xisfconv_read_pixels, pointer, self.index, byref(options),
                         planar.ctypes.data_as(c_void_p), planar.nbytes)
        return _arranged(planar, channels)

    def read_image(self, sample_format=None, *, row_order="top-down", channels="last", verify=True, bounds=None,
                   properties=True):
        """Reads the pixels and what describes them: an :class:`Image`, which :func:`write`
        takes as it is. The options are those of :meth:`read`; ``properties=False`` leaves the
        XISF properties out.

        The keywords are those of the file. BAYERPAT is turned over if the rows are handed
        over in the other order than they are stored in, and added if the file states its
        colour filter array without it. WCS keywords stay as they are in the file, and
        ``wcs_row_order`` of the image names the row order they describe.

        Not everything an XISF file holds is in an :class:`Image`: its properties are read but
        not written, and the saved screen stretch and the resolution are not carried at all.
        :func:`rewrite` copies an XISF file with everything in it."""
        data = self.read(sample_format, row_order=row_order, channels=channels, verify=verify, bounds=bounds)
        info = self._info()
        stored = _ROW_NAMES.get(info.row_order, "top-down")
        image_bounds = None
        if data.dtype.kind == "f":
            if info.sample_format in (_lib.SAMPLE_FLOAT32, _lib.SAMPLE_FLOAT64):
                image_bounds = (info.lower_bound, info.upper_bound)
            else:
                image_bounds = (0.0, 1.0)   # integers read as floating point are normalized
        # The keywords are those of the file. Their WCS part describes wcs_row_order whatever
        # order the rows are handed over in. BAYERPAT describes the rows as stored and is turned
        # over with them; a colour filter array that the file states without that keyword gets
        # it, so that writing the image states it again.
        delivered = row_order or stored
        keywords = self.keywords
        turn = delivered != stored and "BAYERPAT" in keywords
        from_cfa = "BAYERPAT" not in keywords and bool(info.has_cfa)
        if turn or from_cfa:
            derived = self.fits_keywords(delivered, property_keywords=from_cfa, wcs=False)
            pattern = derived.cards("BAYERPAT")
            if pattern and from_cfa:
                keywords.append(pattern[0])
            elif pattern and pattern[0].value != keywords["BAYERPAT"]:
                keywords["BAYERPAT"] = pattern[0].value
        values = {}
        if properties:
            mapping = self.properties
            for key in mapping:
                try:
                    values[key] = mapping[key]
                except Error as e:   # one property that cannot be read does not cost the image
                    _warn("%s: property %s is left out: %s" % (self._file.path, key, e))
                    values[key] = None
        return Image(data, keywords=keywords, name=self.name or None, bounds=image_bounds,
                     icc_profile=self.icc_profile, row_order=delivered, channels=channels,
                     wcs_row_order=_ROW_NAMES.get(info.wcs_row_order), color_space=self.color_space,
                     properties=values)

    def __repr__(self):
        try:
            info = self._info()
        except ValueError:
            return "<xisfconv.FileImage %d of a closed file>" % self.index
        kind = "not readable" if not info.convertible else \
            _SAMPLE_NAMES[info.sample_format] if info.data_known else "samples not read yet"
        return "<xisfconv.FileImage %d %r: %dx%d, %d channel(s), %s>" % (
            self.index, self.name, info.width, info.height, info.channels, kind)


class File:
    """An open XISF, FITS or ASDF file: a sequence of :class:`FileImage`.

    Made by :func:`open`. Opening reads the headers; pixels are read when asked for::

        with xisfconv.open("m31.xisf") as f:
            print(f.format, len(f))
            for image in f:
                print(image.name, image.shape, image.dtype)
            data = f[0].read()

    The images of a FITS file are its primary HDU and its IMAGE extensions that hold pixels
    (tile-compressed ones too); those of an ASDF file are the HDUs of FITS-tagged nodes and
    then every other 2-D or 3-D numeric array.
    """

    # (what a file that could not be opened has)
    _handle = None
    _resource = None
    _context = None
    _count = 0
    path = None

    def __init__(self, path, *, _shown=None):
        # (_shown: the name for messages, if `path` is a temporary copy of what the caller gave)
        #: the name the file was opened with
        self.path = path
        name = _path(path)
        # (Nothing that is done with an open file has steps: opening, reading an image and
        # reading keywords are each one piece of work for the library. Should that change, the
        # context needs `steps`, or Ctrl-C will not stop those calls.)
        self._context = _Context(path if _shown is None else _shown, steps=False)
        self._context.shown = _shown
        handle = c_void_p()
        # What closes the handle is made first, with nothing to close, and given the handle in
        # one step once there is one. Whatever interrupts this, the handle is closed once.
        self._resource = resource = _own(self, _library.xisfconv_close, None)
        try:
            self._context.call(_library.xisfconv_open, self._context.pointer, name, byref(handle))
            resource._as_parameter_ = handle.value
        except BaseException:
            if resource._as_parameter_ is None:     # nobody owns the handle yet
                _library.xisfconv_close(handle)
            raise
        self._handle = handle.value
        self._count = int(_library.xisfconv_image_count(self._handle))

    @property
    def _name(self):
        # what to call the file in messages
        if self._context is not None:
            return self._context._name()
        try:
            return os.fsdecode(os.fspath(self.path))
        except TypeError:
            return str(self.path)

    def _pointer(self):
        # The handle is gone once the file is closed, and also once the collector has taken the
        # file: it frees the handle before it runs the finalizers of other objects, which may
        # still hold the file. The reference this object has to itself tells.
        if self._handle is None or self._resource is None or self._resource() is None:
            raise ValueError("the file is closed")
        return self._handle

    def close(self):
        """Closes the file. Reading from it afterwards raises ValueError."""
        if self._handle is None:
            return
        context = self._context
        with context.lock:
            if self._handle is None:
                return
            if not context._gone() and _library.xisfconv_context_running(context.pointer):
                # the library is reading from the file, further down in this thread: a signal
                # handler or a progress function would close it under the call
                raise RuntimeError("xisfconv: the file is in use and cannot be closed now "
                                   "(is it closed from a signal handler or from a progress function?)")
            context._closed = True
            self._handle = None
            _release(self._resource)
            context.release()

    @property
    def closed(self):
        return self._handle is None or self._resource is None or self._resource() is None

    def __enter__(self):
        self._pointer()
        return self

    def __exit__(self, kind, value, traceback):
        self.close()
        return False

    def __len__(self):
        return self._count

    def __iter__(self):
        # (made when asked for: a file that kept its images would be a reference cycle, and be
        # closed only when the cycle collector comes round)
        return (FileImage(self, index) for index in range(self._count))

    def __getitem__(self, key):
        """An image by number, or by name. A name that no image has is a KeyError."""
        if isinstance(key, str):
            for image in self:
                if image.name == key:
                    return image
            raise KeyError(key)
        if isinstance(key, slice):
            return tuple(FileImage(self, index) for index in range(self._count)[key])
        index = operator.index(key)
        if index < 0:
            index += self._count
        if not 0 <= index < self._count:
            raise ImageIndexError("%s: image index %d out of range (the file has %d)" % (self._name, key, self._count))
        return FileImage(self, index)

    @property
    def images(self):
        """The images as a tuple."""
        return tuple(self)

    @property
    def format(self):
        """"xisf", "fits" or "asdf"."""
        with self._context.lock:
            return _FORMAT_NAMES.get(_library.xisfconv_file_format(self._pointer()), "")

    @property
    def size(self):
        """Size of the file in bytes."""
        with self._context.lock:
            return int(_library.xisfconv_file_size(self._pointer()))

    @property
    def properties(self):
        """The file-level XISF properties (the Metadata element): :class:`Properties`."""
        return Properties(self, _lib.FILE_PROPERTIES)

    @property
    def skipped(self):
        """One line for each part of the file that is not an image this library reads: FITS
        tables, ASDF arrays of another kind."""
        with self._context.lock:
            pointer = self._pointer()
            return [_text(_library.xisfconv_skipped_text(pointer, i))
                    for i in range(_library.xisfconv_skipped_count(pointer))]

    @property
    def header_text(self):
        """The header as text: the XML header (XISF), the non-structural cards of the image
        HDUs, one per line (FITS), or the YAML tree (ASDF)."""
        text = c_void_p()
        length = c_size_t()
        with self._context.lock:
            self._context.call(_library.xisfconv_header_text, self._pointer(), byref(text), byref(length))
            return ctypes.string_at(text.value, length.value).decode("utf-8", "replace") if text.value else ""

    def detail(self, name):
        """A detail of the file as text, "" if it has none. XISF: "version". ASDF: "format"."""
        with self._context.lock:
            return _text(_library.xisfconv_file_detail(self._pointer(), _bytes(name)))

    def __repr__(self):
        if self.closed:
            return "<xisfconv.File %r, closed>" % (self.path,)
        return "<xisfconv.File %r: %s, %d image(s)>" % (self.path, self.format, self._count)


def open(path):   # noqa: A001 - the name is the point, as in gzip.open
    """Opens an XISF, FITS or ASDF file for reading: a :class:`File`.

    FITS and ASDF are recognized by their signature; any other file is taken for XISF, so
    that the XISF reader says what is wrong with it.
    """
    return File(path)


def detect_format(path):
    """"xisf", "fits" or "asdf", from the first bytes of the file. :class:`FormatError` if it
    is none of them."""
    context = _Context.borrow()
    with context.lock:
        context.about(path)
        out = c_int32()
        context.call(_library.xisfconv_detect_format, context.pointer, _path(path), byref(out))
        return _FORMAT_NAMES[out.value]


def _one_image(file, image):
    if isinstance(image, str):
        return file[image]
    if len(file) == 0:
        raise FormatError("%s holds no image" % (file._name,))
    try:
        operator.index(image)
    except TypeError:
        raise TypeError("an image is chosen by its number or its name, not by %s" % type(image).__name__) from None
    return file[image]


def read(path, image=0, *, sample_format=None, row_order="top-down", channels="last", verify=True, bounds=None):
    """Reads one image of a file into a NumPy array: ``xisfconv.read("m31.xisf")``.

    ``image`` is the number of the image in the file, or its name. For the array and the
    options see :meth:`FileImage.read`.
    """
    with File(path) as file:
        return _one_image(file, image).read(sample_format, row_order=row_order, channels=channels, verify=verify,
                                            bounds=bounds)


def read_image(path, image=0, *, sample_format=None, row_order="top-down", channels="last", verify=True, bounds=None,
               properties=True):
    """Reads one image of a file with its keywords, name, bounds, ICC profile and properties:
    an :class:`Image`. See :meth:`FileImage.read_image`."""
    with File(path) as file:
        return _one_image(file, image).read_image(sample_format, row_order=row_order, channels=channels,
                                                  verify=verify, bounds=bounds, properties=properties)


# ------------------------------------------------------------------------------------------
# Writing
# ------------------------------------------------------------------------------------------


def _codec(value, default=_lib.CODEC_NONE):
    if value is None or value is False:
        return default
    if value is True:
        return _lib.CODEC_DEFAULT
    return _choice(value, _CODECS, "codec")


def _checksum(value, default=_lib.CHECKSUM_NONE):
    if value is None or value is False:
        return default
    return _choice(value, _CHECKSUMS, "checksum algorithm")


def _output_format(value):
    if value is None:
        return _lib.FORMAT_AUTO
    return _choice(value, _FORMATS, "format")


def write(path, images, *, format=None, codec=None, checksum=None, stored_row_order=None, subblock_size=None,
          wcs=True, overwrite=False, keywords=None, name=None, bounds=None, icc_profile=None, row_order="top-down",
          channels="last", wcs_row_order=None, progress=None):
    """Writes images to an XISF, FITS, ASDF, TIFF or PNG file.

    ``images`` is a NumPy array, an :class:`Image`, or a list of them: XISF images, FITS HDUs,
    the HDU list of an ASDF file, TIFF pages. PNG holds one image. An array of 3 channels is
    written as RGB, of 1 as grayscale, of any other number as a stack of planes.

    About the file:

    format
        "xisf", "fits", "asdf", "tiff" or "png"; None: from the extension of ``path``.
    codec
        None: no compression. "zlib" or "zstd" for XISF and ASDF; any codec means Deflate
        for TIFF. ``True`` or "default": the usual codec of the format.
        FITS: the images are written tile-compressed and without loss, in the format of
        fpack: ``True`` uses RICE_1 for integers and GZIP_2 for floating point, "zlib" gzip
        for both. A ``path`` that ends in ".fz" (``image.fits.fz``) is written that way
        whatever ``codec`` says. Images of 64-bit integers stay uncompressed, with a warning.
    checksum
        XISF: "sha1", "sha256", "sha512" (also "sha3-256" and "sha3-512", which PixInsight
        does not open).
    stored_row_order
        FITS and ASDF: the row order in the file. None: "bottom-up", the FITS convention.
        XISF, TIFF and PNG are always stored top-down.
    wcs
        To XISF: also write PixInsight's astrometric solution properties from WCS keywords.
    overwrite
        Replace an existing file. Without it :class:`OutputExistsError`.
    progress
        A function ``progress(stage, done, total)``, called when the file is written (once: a
        file is written in one step, except tile-compressed FITS, which reports as it goes).
        An exception it raises stops the work.

    About a NumPy array (an :class:`Image` brings its own): ``keywords``, ``name``, ``bounds``,
    ``icc_profile``, ``row_order`` (of the array: "top-down" or "bottom-up"), ``channels``
    ("last" or "first") and ``wcs_row_order``, as described for :class:`Image`. Keywords
    describe the array as it is given; BAYERPAT and WCS keywords are converted when the rows
    are stored in the other order.

    The file is written under the name ``path + ".part"`` and renamed when it is complete.
    """
    if isinstance(images, Image) or not isinstance(images, (list, tuple)):
        images = [images]
    if not images:
        raise ValueError("there is no image to write")
    prepared = []
    for item in images:
        if not isinstance(item, Image):
            item = Image(item, keywords=keywords, name=name, bounds=bounds, icc_profile=icc_profile,
                         row_order=row_order, channels=channels, wcs_row_order=wcs_row_order)
        prepared.append(item)

    options = _lib.struct(_lib.WriteOptions, _library.xisfconv_write_options_init)
    options.format = _output_format(format)
    options.codec = _codec(codec)
    options.checksum = _checksum(checksum)
    options.row_order = _rows(stored_row_order)
    options.subblock_size = _subblock(subblock_size, options.subblock_size)
    options.wcs = int(bool(wcs))
    options.overwrite = int(bool(overwrite))

    context = _Context.borrow()
    with context.lock:
        context.about(path, reading=False)
        writer = c_void_p()
        context.call(_library.xisfconv_writer_new, context.pointer, _path(path), byref(options), byref(writer),
                     undo=lambda: _library.xisfconv_writer_discard(writer))
        try:
            for item in prepared:
                planar, count = _planar(item.data, item.channels)
                image = _lib.struct(_lib.Image, _library.xisfconv_image_init)
                image.pixels = planar.ctypes.data
                image.width = planar.shape[2]
                image.height = planar.shape[1]
                image.channels = count
                image.sample_format = _SAMPLES[planar.dtype.name]
                image.row_order = _choice(item.row_order, _ROWS, "row order")
                image.use_bounds, image.lower_bound, image.upper_bound = _bounds(item.bounds)
                image.name = _bytes(item.name, "the image name") if item.name else None
                image.wcs_row_order = _rows(item.wcs_row_order)
                profile = None
                if item.icc_profile is not None:
                    if not isinstance(item.icc_profile, (bytes, bytearray, memoryview)):
                        raise TypeError("an ICC profile is given as bytes, not as %s" % type(item.icc_profile).__name__)
                    profile = bytes(item.icc_profile)
                if profile:
                    image.icc_profile = ctypes.cast(ctypes.c_char_p(profile), c_void_p)
                    image.icc_profile_size = len(profile)
                handle = None
                if item.keywords is not None and len(item.keywords):
                    handle = Keywords(item.keywords)._to_handle(context)
                    image.keywords = handle
                try:
                    context.call(_library.xisfconv_writer_add_image, writer, byref(image))
                finally:
                    if handle is not None:
                        _library.xisfconv_keywords_free(handle)
                del planar, profile
        except BaseException:
            _library.xisfconv_writer_discard(writer)
            raise
        # finish frees the writer whether or not it succeeds
        context.call(_library.xisfconv_writer_finish, writer, progress=progress,
                     unstarted=lambda: _library.xisfconv_writer_discard(writer))


# ------------------------------------------------------------------------------------------
# Converting, rewriting, verifying
# ------------------------------------------------------------------------------------------


def convert(input, output, *, format=None, sample_format=None, image=None, stretch=None, codec=None, checksum=None,
            subblock_size=None, row_order=None, property_keywords=True, wcs=True, sip_order=3, verify=True, bounds=None,
            overwrite=False, progress=None):   # noqa: A002 - the names of the command line
    """Converts a file, as the command line tool does: XISF to FITS, ASDF, TIFF or PNG; FITS
    and ASDF to XISF, to each other, or to TIFF or PNG; FITS to FITS to pack a file
    (``codec=True``: tile-compressed) or to unpack one. (XISF to XISF is :func:`rewrite`.)

    format
        Of the output; None: from the extension of ``output``.
    sample_format
        ``--bits``: "uint8", "uint16", "uint32", "uint64", "float32" or "float64"; None: as
        stored.
    image
        Convert only this image (a number); None: all.
    stretch
        ``--stretch``, for viewing: "auto" (the STF saved in the file, else linked),
        "linked", "unlinked" or "stored".
    codec, checksum, subblock_size
        As for :func:`write`.
    row_order
        XISF input: the row order written to FITS or ASDF (None: "bottom-up"). FITS or ASDF
        input: the row order the file is stored in (None: what ROWORDER says, else
        "bottom-up").
    property_keywords
        From XISF: derive missing FITS keywords from properties.
    wcs, sip_order
        Translate the astrometric solution; order of the SIP fit from XISF (2 to 7, 0: linear).
    verify
        Verify the checksums of the input.
    bounds
        ``--bounds``: the range of floating point FITS or ASDF input; None: automatic.
    overwrite
        ``--force``.
    progress
        A function ``progress(stage, done, total)`` called from time to time; an exception
        it raises stops the conversion, leaves no partly written file, and is passed on.
    """
    options = _lib.struct(_lib.ConvertOptions, _library.xisfconv_convert_options_init)
    options.output_format = _output_format(format)
    options.sample_format = _sample_format(sample_format)
    options.image = _image_choice(image)
    options.stretch = _lib.STRETCH_AUTO if stretch is True else \
        _choice(stretch, _STRETCHES, "stretch", _lib.STRETCH_NONE) if stretch else _lib.STRETCH_NONE
    options.codec = _codec(codec)
    options.checksum = _checksum(checksum)
    options.subblock_size = _subblock(subblock_size, options.subblock_size)
    options.row_order = _rows(row_order)
    options.property_keywords = int(bool(property_keywords))
    options.wcs = int(bool(wcs))
    options.sip_order = int(sip_order)
    options.verify_checksums = int(bool(verify))
    options.use_bounds, options.lower_bound, options.upper_bound = _bounds(bounds)
    options.overwrite = int(bool(overwrite))
    context = _Context.borrow()
    with context.lock:
        context.about(input, other=output)
        context.call(_library.xisfconv_convert, context.pointer, _path(input), _path(output), byref(options),
                    progress=progress)


RewriteResult = namedtuple("RewriteResult", "input_size output_size blocks compressed decompressed kept checksums "
                                            "checksums_removed read_back changed")
RewriteResult.__doc__ = """What :func:`rewrite` did: sizes in bytes; the number of data blocks written, compressed
with the requested codec, now stored uncompressed, and copied as they were; checksums computed and removed;
``read_back``: the output was read back and matched; ``changed``: False if the input already stored everything
as requested."""


def _rewrite_options(codec, checksum, image, verify, read_back, subblock_size, overwrite):
    options = _lib.struct(_lib.RewriteOptions, _library.xisfconv_rewrite_options_init)
    if codec is True:
        options.codec = _lib.CODEC_DEFAULT
    elif codec is not None:
        options.codec = _choice(codec, dict(_CODECS, keep=_lib.CODEC_KEEP), "codec")
    if checksum is not None:
        options.checksum = _choice(checksum, dict(_CHECKSUMS, keep=_lib.CHECKSUM_KEEP), "checksum algorithm")
    options.image = _image_choice(image)
    options.verify_input = int(bool(verify))
    options.read_back = int(bool(read_back))
    options.subblock_size = _subblock(subblock_size, options.subblock_size)
    options.overwrite = int(bool(overwrite))
    return options


def _rewrite_result(result):
    return RewriteResult(int(result.input_size), int(result.output_size), int(result.blocks), int(result.compressed),
                         int(result.decompressed), int(result.kept), int(result.checksums),
                         int(result.checksums_removed), bool(result.read_back), bool(result.changed))


def rewrite(input, output, *, codec=None, checksum=None, image=None, verify=True, read_back=True, subblock_size=None,
            overwrite=False, progress=None):   # noqa: A002
    """Writes an XISF file again with its data blocks stored another way: another compression,
    checksums added or removed, one image of several. Returns a :class:`RewriteResult`.

    codec
        None or "keep": leave every block as it is stored. "none", "zlib", "zstd", or
        "default" (Zstandard, or zlib in a build without it).
    checksum
        None or "keep": keep what the file has. "none" removes them; "sha1", "sha256",
        "sha512" (and "sha3-256", "sha3-512") adds or replaces them.
    image
        Keep only this image; None: all.
    verify
        Verify the checksums of the input.
    read_back
        Read the output back and compare every block with the input.
    """
    options = _rewrite_options(codec, checksum, image, verify, read_back, subblock_size, overwrite)
    result = _lib.struct(_lib.RewriteResult, _library.xisfconv_rewrite_result_init)
    context = _Context.borrow()
    with context.lock:
        context.about(input, other=output)
        context.call(_library.xisfconv_rewrite, context.pointer, _path(input), _path(output), byref(options),
                     byref(result), progress=progress)
        return _rewrite_result(result)


def rewrite_in_place(path, *, codec=None, checksum=None, image=None, verify=True, subblock_size=None, overwrite=False,
                     progress=None):
    """Replaces an XISF file by its rewritten self. The new file is written next to it, read
    back and compared, flushed to the disk, and only then renamed over the original. A file
    that is already stored as requested is left alone (``changed`` is False). The options are
    those of :func:`rewrite`; ``overwrite`` only decides whether a leftover ``path + ".part"``
    of an interrupted run may be overwritten."""
    options = _rewrite_options(codec, checksum, image, verify, True, subblock_size, overwrite)
    result = _lib.struct(_lib.RewriteResult, _library.xisfconv_rewrite_result_init)
    context = _Context.borrow()
    with context.lock:
        context.about(path)
        context.call(_library.xisfconv_rewrite_in_place, context.pointer, _path(path), byref(options), byref(result),
                     progress=progress)
        return _rewrite_result(result)


def stored_as_requested(path, *, codec=None, checksum=None, image=None):
    """True if every data block of the XISF file is already stored the way the options of
    :func:`rewrite` ask, judged by the header alone."""
    options = _rewrite_options(codec, checksum, image, True, True, None, False)
    out = c_int32()
    context = _Context.borrow()
    with context.lock:
        context.about(path)
        context.call(_library.xisfconv_stored_as_requested, context.pointer, _path(path), byref(options), byref(out))
        return bool(out.value)


class Report:
    """The result of :func:`verify`.

    verdict
        "ok", "not fully checked" (intact as far as could be told, but some part could not
        be checked) or "failed".
    ok
        True if the verdict is "ok".
    failed
        True if the verdict is "failed".
    format
        "xisf", "fits" or "asdf".
    summary
        One line: "3 data blocks".
    verified
        Number of checksums that are present and match.
    unchecked
        Number of blocks or HDUs without a checksum.
    problems
        What is wrong, one line each.
    not_checked
        What could not be checked, one line each.
    """

    __slots__ = ("path", "verdict", "format", "summary", "verified", "unchecked", "problems", "not_checked")

    @property
    def ok(self):
        return self.verdict == "ok"

    @property
    def failed(self):
        return self.verdict == "failed"

    def __repr__(self):
        return "<xisfconv.Report %r: %s, %s>" % (self.path, self.verdict, self.summary)


def verify(path, *, progress=None):
    """Reads a file completely without converting anything and reports whether it is intact:
    a :class:`Report`. A damaged or unreadable file is not an exception here: its report says
    "failed" and why."""
    context = _Context.borrow()
    with context.lock:
        context.about(path)
        handle = c_void_p()
        context.call(_library.xisfconv_verify, context.pointer, _path(path), byref(handle), progress=progress,
                     undo=lambda: _library.xisfconv_report_free(handle))
        try:
            report = Report()
            report.path = path
            report.verdict = _VERDICTS.get(_library.xisfconv_report_verdict(handle), "failed")
            report.format = _FORMAT_NAMES.get(_library.xisfconv_report_format(handle), "")
            report.summary = _text(_library.xisfconv_report_summary(handle))
            report.verified = int(_library.xisfconv_report_verified(handle))
            report.unchecked = int(_library.xisfconv_report_unchecked(handle))
            report.problems = [_text(_library.xisfconv_report_problem(handle, i))
                               for i in range(_library.xisfconv_report_problem_count(handle))]
            report.not_checked = [_text(_library.xisfconv_report_not_checked(handle, i))
                                  for i in range(_library.xisfconv_report_not_checked_count(handle))]
        finally:
            _library.xisfconv_report_free(handle)
        return report


# ------------------------------------------------------------------------------------------
# The library
# ------------------------------------------------------------------------------------------


def library_version():
    """Version of the shared library libxisfconv in use, as text."""
    return _text(_library.xisfconv_version())


def codec_available(codec, writing=False):
    """True if the library can read (or, with ``writing``, write) the codec: "zlib", "lz4",
    "lz4hc" or "zstd". Zstandard depends on how the library was built; LZ4 is read only."""
    return bool(_library.xisfconv_codec_available(_choice(codec, _CODECS, "codec"), int(bool(writing))))
