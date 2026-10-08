# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The Python interface of libxisfconv: files, images as NumPy arrays, conversion, verification.

Everything public here is re-exported by the package; see the package documentation.
"""

import atexit
import ctypes
import datetime
import logging
import math
import numbers
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


class NotAllowedError(Error, PermissionError):
    """The header of a distributed XISF unit names a file it is not followed to: one outside
    its own directory (``external_files="anywhere"`` allows that), or any file at all with
    ``external_files="none"``."""

    status = _lib.ERR_NOT_ALLOWED


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
    _lib.ERR_NOT_ALLOWED: NotAllowedError,
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
    ("--codec zstd", 'codec="zstd"'),
    ("use --compress", "use codec=True"),
    ("add --compress", "add codec=True"),
    ("--compress", "codec"),
    ("--force", "overwrite=True"),
    ("--bounds expects lo:hi with hi > lo, e.g. 0:65535", "bounds expects (lower, upper) with upper above lower"),
    ("override with --bounds", "override with bounds=(lower, upper)"),
    ("--bounds", "bounds"),
    ("--image", "image"),
    ("--top-down", 'row_order="top-down"'),
    ("--no-properties", "properties=False"),
    ("--debayer", "debayer=True"),
    ("--bin and --resize", "bin, resize and scale"),
    ("--bin", "bin"),
    ("--resize", "resize"),
    ("--no-verify", "verify=False"),
    ("add --in-place to replace it", "use rewrite_in_place() to replace it"),
    ("add --in-place to replace the input", "use rewrite_in_place() to replace the input"),
    ("--external-files anywhere", 'external_files="anywhere"'),
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

    def follow(self, external_files):
        """Which files the header of an XISF unit may name for its data: "header-dir" (None),
        "anywhere" or "none"."""
        which = _lib.EXTERNAL_HEADER_DIRECTORY if external_files is None else \
            _choice(external_files, _EXTERNAL, "external_files")
        _library.xisfconv_context_set_external_files(self.pointer, which)

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

    def about(self, path=None, reading=True, other=None, external_files=None):
        """Says which file the next calls are about (under the lock: a signal handler that
        used the package between `borrow` and the lock has used this context), and which files
        the header of an XISF unit is followed to: the context serves one call after another,
        and each says it for itself."""
        self.follow(external_files)
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
                 _lib.FORMAT_TIFF: "tiff", _lib.FORMAT_PNG: "png", _lib.FORMAT_DNG: "dng"}
_CODECS = {"none": _lib.CODEC_NONE, "zlib": _lib.CODEC_ZLIB, "lz4": _lib.CODEC_LZ4, "lz4hc": _lib.CODEC_LZ4HC,
           "zstd": _lib.CODEC_ZSTD, "default": _lib.CODEC_DEFAULT}
_CHECKSUMS = {"none": _lib.CHECKSUM_NONE, "sha1": _lib.CHECKSUM_SHA1, "sha-1": _lib.CHECKSUM_SHA1,
              "sha256": _lib.CHECKSUM_SHA256, "sha-256": _lib.CHECKSUM_SHA256, "sha512": _lib.CHECKSUM_SHA512,
              "sha-512": _lib.CHECKSUM_SHA512, "sha3-256": _lib.CHECKSUM_SHA3_256, "sha3-512": _lib.CHECKSUM_SHA3_512}
_EXTERNAL = {"header-dir": _lib.EXTERNAL_HEADER_DIRECTORY, "anywhere": _lib.EXTERNAL_ANYWHERE, "none": _lib.EXTERNAL_NONE}
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
# XISF properties
# ------------------------------------------------------------------------------------------

_ELEMENT_DTYPES = {
    _lib.ELEMENT_INT8: np.dtype(np.int8), _lib.ELEMENT_UINT8: np.dtype(np.uint8),
    _lib.ELEMENT_INT16: np.dtype(np.int16), _lib.ELEMENT_UINT16: np.dtype(np.uint16),
    _lib.ELEMENT_INT32: np.dtype(np.int32), _lib.ELEMENT_UINT32: np.dtype(np.uint32),
    _lib.ELEMENT_INT64: np.dtype(np.int64), _lib.ELEMENT_UINT64: np.dtype(np.uint64),
    _lib.ELEMENT_FLOAT32: np.dtype(np.float32), _lib.ELEMENT_FLOAT64: np.dtype(np.float64),
    _lib.ELEMENT_COMPLEX32: np.dtype(np.complex64), _lib.ELEMENT_COMPLEX64: np.dtype(np.complex128),
}
# NumPy's names for the types of XISF: of the elements of vectors and matrices, and of scalars
_ELEMENT_NAMES = {"int8": "I8", "uint8": "UI8", "int16": "I16", "uint16": "UI16", "int32": "I32", "uint32": "UI32",
                  "int64": "I64", "uint64": "UI64", "float32": "F32", "float64": "F64", "complex64": "C32",
                  "complex128": "C64"}
_SCALAR_NAMES = {"int8": "Int8", "uint8": "UInt8", "int16": "Int16", "uint16": "UInt16", "int32": "Int32",
                 "uint32": "UInt32", "int64": "Int64", "uint64": "UInt64", "float32": "Float32", "float64": "Float64",
                 "complex64": "Complex32", "complex128": "Complex64"}
_WHOLE_TYPES = frozenset(("Int8", "UInt8", "Byte", "Int16", "Short", "UInt16", "UShort", "Int32", "Int", "UInt32",
                          "UInt", "Int64", "UInt64"))
_REAL_TYPES = {"Float32": np.float32, "Float": np.float32, "Float64": np.float64, "Double": np.float64}
_COMPLEX_TYPES = {"Complex32": np.float32, "Complex64": np.float64}
# the types the library checks a value of, and those of XISF that are too wide for it: a value
# of one of these is written as the text it has
_CHECKED_TYPES = _WHOLE_TYPES | frozenset(_REAL_TYPES) | frozenset(_COMPLEX_TYPES) | {"Boolean", "String", "TimePoint"}
_WIDE_TYPES = frozenset(("Int128", "UInt128", "Float128", "Complex128"))
_SOLUTION = "PCL:AstrometricSolution:"

_elements = {}


def _element_of(kind):
    """(dtype, matrix) of the XISF type of a vector or a matrix; (None, False) for any other."""
    try:
        return _elements[kind]
    except KeyError:
        pass
    matrix = c_int32()
    try:
        element = _library.xisfconv_property_element(_bytes(kind), byref(matrix))
    except ValueError:
        element = _lib.ELEMENT_NONE
    found = (_ELEMENT_DTYPES.get(element), bool(matrix.value))
    if len(_elements) < 256:
        _elements[kind] = found
    return found


def _scalar_value(kind, value):
    """The Python value of a property that is not a vector or a matrix, from its text."""
    text = value.strip()
    try:
        if kind == "Boolean":
            return text.lower() in ("1", "true")
        if kind in _WHOLE_TYPES or kind.startswith(("Int", "UInt")):
            return int(text)
        if kind in _REAL_TYPES or kind.startswith("Float"):
            return float(text)
        if kind.startswith("Complex") and text.startswith("(") and text.endswith(")"):
            real, imaginary = text[1:-1].split(",")
            return complex(float(real), float(imaginary))
    except ValueError:
        pass
    return value


def _inferred_type(key, value):
    """The XISF type a Python value is written with when none is stated."""
    if isinstance(value, (bool, np.bool_)):
        return "Boolean"
    if isinstance(value, str):
        return "String"
    if isinstance(value, np.datetime64) or isinstance(value, (datetime.datetime, datetime.date)):
        return "TimePoint"
    if isinstance(value, np.generic):
        try:
            return _SCALAR_NAMES[value.dtype.name]
        except KeyError:
            raise TypeError("property %s: XISF has no type for a %s" % (key, value.dtype.name)) from None
    if isinstance(value, numbers.Integral):
        number = int(value)
        if -2 ** 31 <= number < 2 ** 31:
            return "Int32"
        if -2 ** 63 <= number < 2 ** 63:
            return "Int64"
        if 0 <= number < 2 ** 64:
            return "UInt64"
        raise ValueError("property %s: %d is beyond 64 bits" % (key, number))
    if isinstance(value, numbers.Real):
        return "Float64"
    if isinstance(value, numbers.Complex):
        return "Complex64"
    if isinstance(value, (bytes, bytearray, memoryview)):
        return "ByteArray"
    if isinstance(value, (np.ndarray, list, tuple)):
        array = np.asarray(value)
        if array.ndim not in (1, 2):
            raise ValueError("property %s: a vector is a 1-D array and a matrix a 2-D one; this one has %d "
                             "dimension(s)" % (key, array.ndim))
        name = array.dtype.name
        if not isinstance(value, np.ndarray) and array.dtype.kind in "iu":
            # numbers from a list: 32 bits if they fit, whatever NumPy takes for its integer here
            name = "int32" if array.size == 0 or (array.min() >= -2 ** 31 and array.max() < 2 ** 31) else name
        if name not in _ELEMENT_NAMES:
            raise TypeError("property %s: XISF has no vector or matrix of %s" % (key, name))
        return _ELEMENT_NAMES[name] + ("Vector" if array.ndim == 1 else "Matrix")
    raise TypeError("property %s: a value is a number, a string, True or False, a date and time, or an array of "
                    "numbers; not %s" % (key, value.__class__.__name__))


def _real_text(key, value, kind):
    if not isinstance(value, numbers.Real):
        raise TypeError("property %s: a number is needed, not %s" % (key, value.__class__.__name__))
    number = float(value)
    if kind is np.float32:
        with np.errstate(over="ignore"):
            single = np.float32(number)
        if math.isfinite(number) and not np.isfinite(single):
            raise ValueError("property %s: %r is beyond what Float32 holds" % (key, number))
        return str(single)    # the shortest text that reads back as the same 32-bit number
    return repr(number)


def _time_text(key, value):
    if isinstance(value, str):
        return value
    if isinstance(value, np.datetime64):
        if np.isnat(value):
            raise ValueError("property %s: not a time" % key)
        unit = np.datetime_data(value.dtype)[0]
        try:
            # (through seconds for the fine units, and for the finest through nanoseconds: NumPy has
            # no factor between years and picoseconds, nor between seconds and attoseconds)
            coarse = value if unit in ("Y", "M", "W", "D", "h", "m") else \
                (value.astype("datetime64[ns]") if unit in ("fs", "as") else value).astype("datetime64[s]")
            year = int(coarse.astype("datetime64[Y]").astype(np.int64)) + 1970
        except (OverflowError, ValueError):
            year = None
        if year is None or not 1 <= year <= 9999:
            raise ValueError("property %s: a TimePoint is written with a year from 0001 to 9999, which %r is not of" % (key, value))
        # In the unit it has: a date as a date, a time with all the digits of its seconds. Without
        # a zone, as for a datetime without one: a datetime64 does not say where it is.
        if unit in ("Y", "M", "W", "D"):
            return str(np.datetime_as_string(value.astype("datetime64[D]")))
        if unit in ("h", "m"):
            value = value.astype("datetime64[s]")
        return str(np.datetime_as_string(value))
    if isinstance(value, datetime.datetime):
        offset = value.utcoffset()
        if offset is not None and (offset.seconds % 60 or offset.microseconds):
            try:
                value = value.astimezone(datetime.timezone.utc)   # (an offset is written in hours and minutes)
            except OverflowError:
                raise ValueError("property %s: %r is not a time in the years 0001 to 9999 once its offset of "
                                 "seconds is taken off" % (key, value)) from None
        text = value.isoformat()
        return text[:-6] + "Z" if text.endswith("+00:00") else text
    if isinstance(value, datetime.date):
        return value.isoformat()
    raise TypeError("property %s: a TimePoint is a datetime, or the text of one (2026-10-06T18:30:00Z), not %s" %
                    (key, value.__class__.__name__))


def _as_elements(key, array, dtype, kind):
    """The array in the element type of the property, if its numbers can be that."""
    have, want = array.dtype.kind, dtype.kind
    allowed = {"i": "iu", "u": "iu", "f": "iuf", "c": "iufc"}[want]
    if have not in allowed:
        raise TypeError("property %s: an array of %s cannot be a %s" % (key, array.dtype.name, kind))
    if want in "iu" and array.size:
        limits = np.iinfo(dtype)
        if int(array.min()) < limits.min or int(array.max()) > limits.max:
            raise ValueError("property %s: the array holds numbers a %s does not" % (key, kind))
    with np.errstate(over="ignore"):
        out = np.ascontiguousarray(array, dtype=dtype)
    if want in "fc" and array.dtype != dtype and bool((np.isfinite(array) & ~np.isfinite(out)).any()):
        raise ValueError("property %s: the array holds numbers a %s does not" % (key, kind))
    return out


def _property_payload(key, value, kind):
    """What the library is given as the value of a property of the XISF type `kind`: the text of
    a scalar, or the array of a vector or a matrix."""
    dtype, matrix = _element_of(kind)
    if dtype is not None:
        if isinstance(value, (bytes, bytearray, memoryview)):
            array = np.frombuffer(bytes(value), np.uint8)
        else:
            array = np.asarray(value)
        if array.ndim != (2 if matrix else 1):
            raise ValueError("property %s: a %s is a %d-D array; this one has %d dimension(s)" %
                             (key, kind, 2 if matrix else 1, array.ndim))
        return _as_elements(key, array, dtype, kind)
    if kind == "Boolean":
        if isinstance(value, (bool, np.bool_)) or (isinstance(value, numbers.Integral) and value in (0, 1)):
            return "true" if value else "false"
        raise TypeError("property %s: a Boolean is True or False, not %r" % (key, value))
    if kind in _WHOLE_TYPES:
        try:
            return str(operator.index(value))
        except TypeError:
            raise TypeError("property %s: a whole number is needed for %s, not %s" %
                            (key, kind, value.__class__.__name__)) from None
    if kind in _REAL_TYPES:
        return _real_text(key, value, _REAL_TYPES[kind])
    if kind in _COMPLEX_TYPES:
        if not isinstance(value, numbers.Complex):
            raise TypeError("property %s: a number is needed, not %s" % (key, value.__class__.__name__))
        number = complex(value)
        part = _COMPLEX_TYPES[kind]
        return "(%s,%s)" % (_real_text(key, number.real, part), _real_text(key, number.imag, part))
    if kind == "TimePoint":
        return _time_text(key, value)
    if kind == "String":
        if not isinstance(value, str):
            raise TypeError("property %s: a String is text, not %s" % (key, value.__class__.__name__))
        return value
    raise ValueError("property %s: a property of the type %s is not written from a value" % (key, kind))


def _plain_text(key, value, kind):
    """The value of a property of a type that is too wide for the library to check (Float128,
    UInt128), as the text it is written with."""
    if isinstance(value, str):
        return value
    if isinstance(value, (bool, np.bool_)):
        return "true" if value else "false"
    if isinstance(value, numbers.Integral):
        return str(int(value))
    if isinstance(value, numbers.Real):
        return repr(float(value))
    if isinstance(value, numbers.Complex):
        return "(%r,%r)" % (complex(value).real, complex(value).imag)
    raise TypeError("property %s: the value of a %s is given as a number or as its text, not as %s" %
                    (key, kind, value.__class__.__name__))


def _same_value(a, b):
    """True if a value is still the one that was read: the same object, or one equal to it of
    the same kind."""
    if a is b:
        return True
    if a.__class__ is not b.__class__ or isinstance(a, np.ndarray):
        return False
    try:
        if isinstance(a, (float, np.floating)):
            # (0.0 and -0.0 are two values, and not-a-number is the one it was)
            return bool((a == b and math.copysign(1.0, a) == math.copysign(1.0, b)) or (a != a and b != b))
        if isinstance(a, (complex, np.complexfloating)):
            return _same_value(float(a.real), float(b.real)) and _same_value(float(a.imag), float(b.imag))
        return bool(a == b)
    except Exception:   # noqa: BLE001 - a value that cannot be compared is another one
        return False


def _still_read(value, read):
    """True if a property of an astrometric solution is still what was read: as for
    :func:`_same_value`, and an array with the elements it had."""
    if isinstance(value, np.ndarray) and isinstance(read, np.ndarray):
        return value is read or (value.dtype == read.dtype and value.shape == read.shape and
                                 bool(np.array_equal(value, read, equal_nan=value.dtype.kind in "fc")))
    return _same_value(value, read)


class PropertyDict(dict):
    """XISF properties in memory: ``{id: value}``. This is what an :class:`Image` has and what
    :func:`write` takes.

    A value is a bool, an int, a float, a complex number, a str, a ``datetime``, or a NumPy
    array of one dimension (a vector) or two (a matrix). It is written with the XISF type that
    goes with it: Boolean, Int32 (Int64 or UInt64 if it does not fit), Float64, Complex64,
    String, TimePoint, and vectors and matrices in the type of their elements (``uint16`` gives
    UI16Vector, ``float64`` F64Matrix, ``complex64`` C32Vector). A NumPy scalar keeps its width
    (``numpy.float32(0.5)`` is a Float32) and ``bytes`` are a ByteArray.

    :meth:`set` states the type, and a comment and a format, where that is not what is wanted;
    :meth:`type`, :meth:`comment` and :meth:`format` tell them, as for :class:`Properties`. A
    value of the 128-bit types of XISF (Int128, UInt128, Float128, Complex128) is written as
    the number it is given as, or as its text.

    Properties read from a file (:func:`read_image`) have what the file states. A value that is
    not touched is written again with the text the file has for it, whatever it says (only a
    NUL character, and what stands behind it, is lost); assigning a new value to a
    key keeps its type, comment and format, and deleting the key forgets them. ``update`` and
    ``|`` carry all that over from another ``PropertyDict``; a plain ``dict`` made of one has
    the values only.

    A property whose value is None (one that could not be read) is not written.

    ``solution_of`` belongs to an astrometric solution (``PCL:AstrometricSolution:...``) that
    was read from a file: it says which WCS keywords and which image size the solution was
    read with. :func:`write` writes such properties only with an image that still has those,
    and leaves them out otherwise, so that a cropped image or new keywords are not contradicted
    by an old solution; a solution is then made from the WCS keywords, if the image has them.
    It is None for properties that were not read from a file, and a solution among those is
    written as it is: a program that puts a solution of its own in the place of the one that
    was read has it written. A solution is one thing, though: if only some of its properties
    were given new values and the others no longer belong to the image, all of it is left out,
    and a warning says so. Set ``solution_of`` to None to say that the solution is right as it
    stands.
    """

    def __init__(self, *properties, **more):
        dict.__init__(self)
        self._about = {}     # {key: (type or None, comment, format)}: what is stated
        # {key: (text, value, as a text block)}: a value as a file wrote it, what that text was
        # read as, and whether the file keeps it as a data block (a String)
        self._read = {}
        self._solution = {}  # {key: (digest, value)}: a property of a solution, and what it was read with
        self._untyped = set()   # the keys of properties a file has without a type
        if len(properties) > 1:
            raise TypeError("PropertyDict expected at most 1 argument, got %d" % len(properties))
        if properties and properties[0] is not None:
            self.update(properties[0])
        if more:
            self.update(more)

    def set(self, key, value, type=None, comment="", format=""):   # noqa: A002 - the words of XISF
        """Sets a property with what XISF states about it: ``type`` is the XISF type name
        ("Float32", "UInt16", "TimePoint", "F32Vector"; None: the type that goes with the
        value), ``comment`` a remark, and ``format`` how the value is meant to be shown
        ("%.3f")."""
        if type is not None and not isinstance(type, str):
            raise TypeError("the type of a property is its XISF name, a string")
        dict.__setitem__(self, key, value)
        comment, format = str(comment or ""), str(format or "")   # noqa: A001
        self._read.pop(key, None)
        self._solution.pop(key, None)
        self._untyped.discard(key)
        if not type and not comment and not format:
            self._about.pop(key, None)
        else:
            self._about[key] = (type or None, comment, format)

    def _stated(self, key):
        return self._about.get(key, (None, "", ""))

    def _as_read(self, key):
        """The text a file has for the value and whether it keeps it as a data block, if the
        value is still the one that was read; else None."""
        read = self._read.get(key)
        if read is not None and _same_value(self[key], read[1]):
            return read[0], bool(read[2])
        return None

    def _read_solution(self):
        """{key: digest} for the properties of an astrometric solution that are still what a
        file had."""
        return {key: digest for key, (digest, value) in self._solution.items()
                if key in self and _still_read(self[key], value)}

    @property
    def solution_of(self):
        """What the astrometric solution among the properties was read with (a digest of WCS
        keywords, image size and row order); None if it was not read from a file."""
        for digest in self._read_solution().values():
            return digest
        return None

    @solution_of.setter
    def solution_of(self, digest):
        self._solution.clear()
        if digest is not None:
            for key in self:
                if isinstance(key, str) and key.startswith(_SOLUTION):
                    self._solution[key] = (str(digest), self[key])

    def type(self, key):   # noqa: A003
        """The XISF type name the property is written with: the one stated, or the one that
        goes with its value. "" for a property without a value, and for one that a file has
        without a type and that was not given a new value."""
        value = self[key]
        stated = self._stated(key)[0]
        if stated:
            return stated
        if value is None or (key in self._untyped and self._as_read(key) is not None):
            return ""
        return _inferred_type(key, value)

    def comment(self, key):
        self[key]
        return self._stated(key)[1]

    def format(self, key):   # noqa: A003
        """How the value is meant to be shown (a format specification like "%.3f"); "" if
        the property has none."""
        self[key]
        return self._stated(key)[2]

    def _forget(self, key):
        self._about.pop(key, None)
        self._read.pop(key, None)
        self._solution.pop(key, None)
        self._untyped.discard(key)

    def __delitem__(self, key):
        dict.__delitem__(self, key)
        self._forget(key)

    def pop(self, key, *default):
        value = dict.pop(self, key, *default)
        self._forget(key)
        return value

    def popitem(self):
        key, value = dict.popitem(self)
        self._forget(key)
        return key, value

    def clear(self):
        dict.clear(self)
        self._about.clear()
        self._read.clear()
        self._solution.clear()
        self._untyped.clear()

    def update(self, *other, **more):
        """As for a dict. From another :class:`PropertyDict`, and from the :class:`Properties`
        of an open file, the types, comments and formats come along."""
        if len(other) > 1:
            raise TypeError("update expected at most 1 argument, got %d" % len(other))
        source = other[0] if other else ()
        if source is self:
            source = ()
        if isinstance(source, PropertyDict):
            dict.update(self, source)
            for key in source:
                self._forget(key)
                for mine, theirs in ((self._about, source._about), (self._read, source._read),
                                     (self._solution, source._solution)):
                    if key in theirs:
                        mine[key] = theirs[key]
                if key in source._untyped:
                    self._untyped.add(key)
        elif isinstance(source, Properties):
            source._detach(self)
        else:
            dict.update(self, source)
        if more:
            dict.update(self, more)

    def __ior__(self, other):
        self.update(other)
        return self

    def __or__(self, other):
        if not isinstance(other, Mapping):
            return NotImplemented
        merged = self.copy()
        merged.update(other)
        return merged

    def __ror__(self, other):
        if not isinstance(other, Mapping):
            return NotImplemented
        merged = PropertyDict(other)
        merged.update(self)
        return merged

    def copy(self):
        return PropertyDict(self)

    __copy__ = copy

    def _without(self, keys):
        """A copy without these properties."""
        out = self.copy()
        for key in keys:
            del out[key]
        return out

    def __reduce__(self):
        return (_restored_properties, (dict(self), dict(self._about), dict(self._read), dict(self._solution),
                                       set(self._untyped)))

    def __repr__(self):
        return "PropertyDict(%s)" % dict.__repr__(self)


def _restored_properties(values, about, read, solution, untyped=()):
    out = PropertyDict(values)
    out._about, out._read, out._solution, out._untyped = about, read, solution, set(untyped)
    return out


def _stale_solution(context, properties, keywords, width, height, row_order):
    """The properties of an astrometric solution that were read from a file with other WCS
    keywords, another image size or another row order than these."""
    read = properties._read_solution()
    if not read:
        return [], 0
    now = _wcs_digest(context, keywords, width, height, row_order)
    if all(digest == now for digest in read.values()):
        return [], 0
    # A solution is one thing: what was set since belongs to what was read, and goes with it.
    whole = [key for key in properties if isinstance(key, str) and key.startswith(_SOLUTION)]
    return whole, len(whole) - len(read)


def _written_format(format, path):   # noqa: A002
    """"xisf", "fits", "asdf", "tiff" or "png" as far as it can be told here; None if not."""
    if isinstance(format, str) and format.strip():
        return _FORMAT_NAMES.get(_FORMATS.get(format.strip().lower()))
    try:
        name = os.fsdecode(os.fspath(path)).lower()
    except TypeError:
        return None
    for ending, kind in ((".xisf", "xisf"), (".fits", "fits"), (".fit", "fits"), (".fts", "fits"), (".fz", "fits"),
                         (".asdf", "asdf"), (".tif", "tiff"), (".tiff", "tiff"), (".png", "png")):
        if name.endswith(ending):
            return kind
    return None


def _solution_left_out(name, own, keywords, wcs, kind="xisf"):
    """Says that a solution that was read is not written, and what the file has instead. `own`:
    how many properties of it had been given new values."""
    said = "%s: the astrometric solution that was read from a file is not written: the image has not the size " \
           "or the WCS keywords it was read with" % name
    try:
        cards = keywords if isinstance(keywords, Keywords) else Keywords(keywords)
        # (the library decides whether they describe a WCS it has a solution for, and warns if not)
        has_wcs = all(isinstance(cards.get(name), str) and cards.get(name).strip() for name in ("CTYPE1", "CTYPE2"))
    except Exception:   # noqa: BLE001 - keywords that are not keywords are refused where they are written
        has_wcs = False
    hint = " (properties.solution_of = None says that the solution is right as it stands)"
    if own:
        said += "; the %d of its properties that were set since are left out with it, a solution being one thing" % own
    if kind in ("fits", "asdf"):
        if has_wcs and not own:
            _log.info("%s; the file has the WCS keywords", said)
        else:
            _warn(said + ("; the file has the WCS keywords" if has_wcs else ", and the image has no WCS keywords either") + hint)
    elif wcs and has_wcs:
        (_warn if own else _log.info)("%s; the solution is made from the WCS keywords, if they describe one%s" %
                                      (said, hint if own else ""))
    else:
        _warn("%s, and the file has no astrometric solution%s%s" %
              (said, "" if wcs or not has_wcs else ": none is made from WCS keywords without wcs=True", hint))


def _wcs_digest(context, keywords, width, height, row_order):
    """What tells whether WCS keywords are still those a solution was read with (see
    xisfconv_wcs_digest)."""
    handle = Keywords(keywords)._to_handle(context)
    try:
        text = c_char_p()
        context.quick(_library.xisfconv_wcs_digest, handle, int(width), int(height),
                      _ROWS.get(row_order, _lib.ROWS_DEFAULT), byref(text))
        return _text(text.value)
    finally:
        _library.xisfconv_keywords_free(handle)


def _properties_handle(context, properties, what):
    """A property list of the library from ``{id: value}``, or None if there is nothing to
    write. The caller frees it."""
    if properties is None:
        return None
    if not isinstance(properties, Mapping):
        raise TypeError("%s are a dict {id: value}, not %s" % (what, properties.__class__.__name__))
    if isinstance(properties, Properties):      # those of an open file, as they are
        properties = PropertyDict(properties)
    if not len(properties):
        return None
    handle = c_void_p()
    context.quick(_library.xisfconv_properties_new, context.pointer, byref(handle))
    try:
        for key in properties:
            if not isinstance(key, str):
                raise TypeError("the id of a property is a string, not %s" % key.__class__.__name__)
            as_read = None
            if isinstance(properties, PropertyDict):
                value = properties[key]
                kind, comment, form = properties._stated(key)
                as_read = properties._as_read(key)
            else:
                value, kind, comment, form = properties[key], None, "", ""
            if value is None:
                _warn("property %s%s has no value and is not written" % (key, " (a %s)" % kind if kind else ""))
                continue
            name = _bytes(key, "the id of a property")
            comment = _bytes(comment, "the comment of property %s" % key) if comment else None
            form = _bytes(form, "the format of property %s" % key) if form else None
            if not kind:
                # (a property a file has without a type is written again without one)
                kind = "" if as_read is not None and isinstance(properties, PropertyDict) and key in properties._untyped \
                    else _inferred_type(key, value)
            array = _element_of(kind)[0] is not None
            if not array and as_read is None and kind not in _CHECKED_TYPES:
                if kind not in _WIDE_TYPES:
                    raise ValueError("property %s: %s is not a type of XISF that is written from a value" % (key, kind))
                as_read = _plain_text(key, value, kind), False      # 128 bits: written as its text says
            if not array and as_read is not None:
                # What a file has is written as the file has it; what it is, is the file's business.
                context.quick(_library.xisfconv_properties_set_as_read, handle, name, _bytes(kind, "a property type"),
                              _bytes(as_read[0], "the value of property %s" % key), comment, form,
                              _lib.PROPERTY_TEXT_BLOCK if as_read[1] and kind == "String" else _lib.PROPERTY_VALUE)
                continue
            payload = _property_payload(key, value, kind)
            if isinstance(payload, np.ndarray):
                rows, columns = payload.shape if payload.ndim == 2 else (payload.shape[0], 0)
                context.quick(_library.xisfconv_properties_set_array, handle, name, _bytes(kind, "a property type"),
                              payload.ctypes.data_as(c_void_p), payload.nbytes, rows, columns, comment, form)
            else:
                context.quick(_library.xisfconv_properties_set, handle, name, _bytes(kind, "a property type"),
                              _bytes(payload, "the value of property %s" % key), comment, form)
        if not _library.xisfconv_properties_count(handle):
            _library.xisfconv_properties_free(handle)
            return None
    except BaseException:
        _library.xisfconv_properties_free(handle)
        raise
    return handle


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
        XISF properties, ``{id: value}``: a :class:`PropertyDict`, or a dict it is made from.
        Written to XISF as the properties of the image, and to FITS and ASDF the way
        :func:`convert` takes the properties of an XISF file along. An astrometric solution
        among them (``PCL:AstrometricSolution:...``) is written as it is, and none is made from
        WCS keywords then. One that was read with the image is written only while the image
        has the WCS keywords and the size it was read with (see :class:`PropertyDict`).
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
        self.properties = properties if isinstance(properties, PropertyDict) else PropertyDict(properties)
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

    Scalars are bool, int, float, complex or str (a time point is the text the file has);
    vectors and matrices are NumPy arrays in the type of their elements (an F32Vector is
    float32, a UI16Matrix uint16, a C64Vector complex128; up to 0.14 they were all read as
    float64, and complex ones not at all). A property this library does not read is None: a
    table, a data block of a type it has no name for. A value of a type it has no name for
    (Float128) is the text the file has. :meth:`type`, :meth:`comment` and :meth:`format` give
    the XISF type name, the comment and the format specification of a property.

    A FITS or ASDF file has no properties of its own: it has those of the XISF file it was
    converted from, if it was (see :func:`convert`), and none otherwise.
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
        return self._at(self._load()[key])[1:]

    def _at(self, index):
        """The property at a place in the list: id, type, value as text, comment, and whether
        the value is a data block."""
        file = self._file
        name, kind, value, comment, block = c_char_p(), c_char_p(), c_char_p(), c_char_p(), c_int32()
        with file._context.lock:
            file._context.quick(_library.xisfconv_property_get, file._pointer(), self._image, index, byref(name),
                                byref(kind), byref(value), byref(comment), byref(block))
            return _text(name.value), _text(kind.value), _text(value.value), _text(comment.value), bool(block.value)

    def _count(self):
        file = self._file
        with file._context.lock:
            return int(_library.xisfconv_property_count(file._pointer(), self._image))

    def _as_stored(self, index, text, stored):
        """What is written again for a value that is not touched: its text, or the bytes it has
        if they are not UTF-8."""
        if "\ufffd" not in text:
            return text
        file = self._file
        value = c_char_p()
        with file._context.lock:
            file._context.quick(_library.xisfconv_property_get, file._pointer(), self._image, index, None, None,
                                byref(value), None, None)
            raw = value.value or b""
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError:
            return raw
        return text

    def __len__(self):
        return len(self._load())

    def __iter__(self):
        return iter(self._load())

    def __contains__(self, key):
        return key in self._load()

    def _stored(self, index):
        """How the property is stored: one of the PROPERTY_ numbers of the library."""
        file = self._file
        with file._context.lock:
            return int(_library.xisfconv_property_stored(file._pointer(), self._image, index))

    def __getitem__(self, key):
        index = self._load()[key]
        _, kind, value, _, _ = self._at(index)
        stored = self._stored(index)
        if stored == _lib.PROPERTY_ARRAY:
            return self._read_block(index, kind)
        if stored == _lib.PROPERTY_UNREAD:
            return None
        return _scalar_value(kind, value)

    def _read_block(self, index, kind):
        """A vector or a matrix in the type of its elements; None for a data block of any
        other type."""
        dtype, matrix = _element_of(kind)
        if dtype is None:
            return None
        file = self._file
        size, rows, columns = c_size_t(), c_size_t(), c_size_t()
        with file._context.lock:
            pointer = file._pointer()
            file._context.call(_library.xisfconv_property_read, pointer, self._image, index, None, 0, byref(size),
                               byref(rows), byref(columns))
            out = np.empty((rows.value, columns.value) if matrix else (columns.value,), dtype)
            if out.nbytes != size.value:
                raise InternalError("the library has %d bytes for a property of %d" % (size.value, out.nbytes))
            file._context.call(_library.xisfconv_property_read, pointer, self._image, index,
                               out.ctypes.data_as(c_void_p), out.nbytes, byref(size), byref(rows), byref(columns))
        return out

    def _detach(self, into):
        """Reads every property into a :class:`PropertyDict`, with its type, comment and
        format. One that cannot be read is there without a value, and a warning says why."""
        file = self._file
        solved = [key for key in self._load() if key.startswith(_SOLUTION)]
        digest = None
        if solved and self._image != _lib.FILE_PROPERTIES:
            entry = file[self._image]
            if file.format != "xisf" and entry.detail("carriedSolution") == "stale":
                # as a conversion of the file has it: the keywords say what the WCS is now
                _log.info("%s: the astrometric solution the file carries is left out: its WCS keywords, its size "
                          "or the order of its rows changed since the solution was written", file._name)
            else:
                info, keywords = entry._info(), entry.keywords
                with file._context.lock:
                    digest = _wcs_digest(file._context, keywords, info.width, info.height,
                                         _ROW_NAMES.get(info.wcs_row_order))
        names = [self._at(index)[0] for index in range(self._count())]
        nameless = names.count("")
        twice = len(names) - nameless - len(set(names) - {""})
        if twice:
            _warn("%s: %d propert%s the id of an earlier one, and %s left out" %
                  (file._name, twice, "y has" if twice == 1 else "ies have", "is" if twice == 1 else "are"))
        if nameless:
            _warn("%s: %s left out" % (file._name, "a property without an id is" if nameless == 1 else
                                       "%d properties without an id are" % nameless))
        for key, index in self._load().items():
            if not key:
                continue
            if digest is None and key in solved and self._image != _lib.FILE_PROPERTIES:
                continue
            _, kind, text, comment, _ = self._at(index)
            stored = self._stored(index)
            try:
                if stored == _lib.PROPERTY_ARRAY:
                    value = self._read_block(index, kind)
                else:
                    value = None if stored == _lib.PROPERTY_UNREAD else _scalar_value(kind, text)
            except Error as e:   # one property that cannot be read does not cost the others
                _warn("%s: property %s is left out: %s" % (file._name, key, e))
                value = None
            into.set(key, value, kind, comment, self.format(key))
            if stored in (_lib.PROPERTY_VALUE, _lib.PROPERTY_TEXT_BLOCK):
                # written again as the file has it, if it is not touched (a text block with its
                # bytes, which need not be UTF-8)
                into._read[key] = (self._as_stored(index, text, stored), value, stored == _lib.PROPERTY_TEXT_BLOCK)
                if not kind:
                    into._untyped.add(key)
            if digest is not None and key in solved:
                into._solution[key] = (digest, value)
        return into

    def type(self, key):
        """The XISF type name: "Float64", "String", "F64Matrix", "TimePoint"."""
        return self._get(key)[0]

    def comment(self, key):
        return self._get(key)[2]

    def format(self, key):
        """How the value is meant to be shown (a format specification like "%.3f"); "" if
        the property has none."""
        index = self._load()[key]
        file = self._file
        with file._context.lock:
            return _text(_library.xisfconv_property_format(file._pointer(), self._image, index))

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
        """Colour filter array of an XISF or DNG image: ``(pattern, width, height)`` such as
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
        """FITS and ASDF: BITPIX of the stored data; DNG: 16 or 32; 0 for XISF."""
        return int(self._info().bitpix)

    @property
    def bscale(self):
        return self._info().bscale

    @property
    def bzero(self):
        return self._info().bzero

    @property
    def source_index(self):
        """FITS: the number of the HDU; ASDF: the running number of the array; DNG: 0."""
        return int(self._info().source_index)

    @property
    def plain_array(self):
        """ASDF: True for an array that is not an HDU of a FITS-tagged node."""
        return bool(self._info().plain_array)

    def detail(self, name):
        """A detail of the image as text, "" if it has none. XISF: "sampleFormat", "colorSpace",
        "pixelStorage", "byteOrder", "location", "compression", "subblocks", "checksum",
        "imageType", "orientation", "cfaPattern", "cfaName", "resolutionUnit". FITS:
        "tileCompression", "mapping". ASDF: "source", "storage", "mapping". DNG: "source" (the
        directory of the raw image), "storage", "mapping", "cfaPattern"."""
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

        Not everything an XISF file holds is in an :class:`Image`: the saved screen stretch,
        the resolution and the thumbnail are not carried. :func:`rewrite` copies an XISF file
        with everything in it."""
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
        values = PropertyDict()
        if properties:
            self.properties._detach(values)    # (with what tells whether a solution is still that of its keywords)
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

    def __init__(self, path, *, external_files=None, _shown=None):
        # (_shown: the name for messages, if `path` is a temporary copy of what the caller gave)
        #: the name the file was opened with
        self.path = path
        name = _path(path)
        # (Nothing that is done with an open file has steps: opening, reading an image and
        # reading keywords are each one piece of work for the library. Should that change, the
        # context needs `steps`, or Ctrl-C will not stop those calls.)
        self._context = _Context(path if _shown is None else _shown, steps=False)
        self._context.shown = _shown
        self._context.follow(external_files)     # (the file keeps what it is opened with)
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
        """"xisf", "fits", "asdf" or "dng"."""
        with self._context.lock:
            return _FORMAT_NAMES.get(_library.xisfconv_file_format(self._pointer()), "")

    @property
    def size(self):
        """Size of the file in bytes."""
        with self._context.lock:
            return int(_library.xisfconv_file_size(self._pointer()))

    @property
    def unit(self):
        """XISF: "monolithic" for a file that holds the whole unit (.xisf), "distributed" for
        the header file of a unit whose data is in other files (.xish). "" for FITS and ASDF."""
        return self.detail("unit")

    @property
    def external_files(self):
        """The files the header of an XISF unit names beside itself, where its data blocks are:
        a list of absolute paths (a URL for a file that is not a local one), each once, in the
        order of the header, whether or not the file is there and may be read. Empty for a
        monolithic XISF file that names none, and for FITS and ASDF."""
        with self._context.lock:
            pointer = self._pointer()
            return [_text(_library.xisfconv_external_file(pointer, i))
                    for i in range(_library.xisfconv_external_count(pointer))]

    @property
    def unit_size(self):
        """Size in bytes of the file together with :attr:`external_files`, as far as those are
        there and may be read. The same as :attr:`size` for a file that names none."""
        with self._context.lock:
            return int(_library.xisfconv_unit_size(self._pointer()))

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
        return self._header_bytes().decode("utf-8", "replace")

    def _header_bytes(self):
        text = c_void_p()
        length = c_size_t()
        with self._context.lock:
            self._context.call(_library.xisfconv_header_text, self._pointer(), byref(text), byref(length))
            return ctypes.string_at(text.value, length.value) if text.value else b""

    def detail(self, name):
        """A detail of the file as text, "" if it has none. XISF: "version", "unit". ASDF: "format"."""
        with self._context.lock:
            return _text(_library.xisfconv_file_detail(self._pointer(), _bytes(name)))

    def __repr__(self):
        if self.closed:
            return "<xisfconv.File %r, closed>" % (self.path,)
        return "<xisfconv.File %r: %s, %d image(s)>" % (self.path, self.format, self._count)


def open(path, *, external_files=None):   # noqa: A001 - the name is the point, as in gzip.open
    """Opens an XISF, FITS or ASDF file for reading: a :class:`File`.

    FITS and ASDF are recognized by their signature; any other file is taken for XISF, so
    that the XISF reader says what is wrong with it.

    An XISF file is a monolithic file (.xisf) or the header file of a distributed unit
    (.xish), whose data blocks are in the files that header names (data blocks files, .xisb,
    and any other). ``external_files`` says how far a header is followed, here and wherever a
    function of this package reads an XISF file:

    "header-dir" (None, the default)
        to files in the directory of the header and below it; and only a header file that is
        named as one (.xish) is followed: a monolithic file holds all of its data, so a .xisf
        file that names the file beside it is not followed there
    "anywhere"
        also to absolute paths, ``file:`` URLs, and where ``..`` and symbolic links lead, and
        from any XISF file
    "none"
        to no file but the header itself

    A header is data that came from somewhere: one that names a file of this machine as the
    pixels of an image would have a conversion copy that file into its output. A block in a
    file the header is not followed to is not read: :class:`NotAllowedError` for the pixels
    of an image, a warning for a property. Nothing is ever fetched from a network. (This is a
    rule for files from people you do not know, not a sandbox: it does not hold against
    somebody who changes the directory while the file is read.)
    """
    return File(path, external_files=external_files)


def detect_format(path):
    """"xisf", "fits", "asdf" or "dng", from the first bytes of the file. :class:`FormatError`
    if it is none of them."""
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


def read(path, image=0, *, sample_format=None, row_order="top-down", channels="last", verify=True, bounds=None,
         external_files=None):
    """Reads one image of a file into a NumPy array: ``xisfconv.read("m31.xisf")``.

    ``image`` is the number of the image in the file, or its name. For the array and the
    options see :meth:`FileImage.read`; for ``external_files`` see :func:`open`.
    """
    with File(path, external_files=external_files) as file:
        return _one_image(file, image).read(sample_format, row_order=row_order, channels=channels, verify=verify,
                                            bounds=bounds)


def read_image(path, image=0, *, sample_format=None, row_order="top-down", channels="last", verify=True, bounds=None,
               properties=True, external_files=None):
    """Reads one image of a file with its keywords, name, bounds, ICC profile and properties:
    an :class:`Image`. See :meth:`FileImage.read_image`; for ``external_files`` see :func:`open`."""
    with File(path, external_files=external_files) as file:
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
          channels="last", wcs_row_order=None, properties=None, file_properties=None, shuffle=True, level=None,
          creator=None, progress=None):
    """Writes images to an XISF, FITS, ASDF, TIFF or PNG file.

    ``images`` is a NumPy array, an :class:`Image`, or a list of them: XISF images, FITS HDUs,
    the HDU list of an ASDF file, TIFF pages. PNG holds one image. An array of 3 channels is
    written as RGB, of 1 as grayscale, of any other number as a stack of planes.

    About the file:

    format
        "xisf", "fits", "asdf", "tiff" or "png"; None: from the extension of ``path``.
        XISF under a name that ends in ".xish" is written as a distributed unit: that file is
        the header, and the pixels (and every other block too large for the header) go into
        the file of the same name that ends in ".xisb" (``frame.xish`` and ``frame.xisb``).
        Any other name is a monolithic file. PixInsight itself opens monolithic files only.
    codec
        None: no compression. "zlib", "zstd", "lz4" or "lz4hc" for XISF, "zlib" or "zstd" for
        ASDF; any codec means Deflate for TIFF. ``True`` or "default": the usual codec of the
        format.
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
    shuffle
        XISF: byte shuffling before compression (the bytes of the samples sorted by their
        place in the sample, which compresses better). On unless it is False.
    level
        XISF: the compression level. None: the usual one of the codec (zlib 6, lz4hc 9,
        zstd 3); else zlib 1 to 9, lz4hc 1 to 12, zstd 1 to 22. "lz4" has no levels.
    wcs
        To XISF: also write PixInsight's astrometric solution properties from WCS keywords
        (for an image that brings no such solution among its properties).
    file_properties
        XISF properties of the file, ``{id: value}`` (see :class:`PropertyDict`): its
        ``Metadata``. To FITS and ASDF they go the way :func:`convert` takes them along. The
        properties that describe how one XISF file was made and is stored
        (``XISF:CreationTime``, ``XISF:CreatorApplication``, ``XISF:BlockAlignmentSize`` and
        the like) are left out: a file that is written has its own.
    creator
        XISF: the name of the program that makes the file (``XISF:CreatorApplication``);
        this library is then named in ``XISF:CreatorModule``. None: the library names itself.
    overwrite
        Replace an existing file. Without it :class:`OutputExistsError`.
    progress
        A function ``progress(stage, done, total)``, called when the file is written (once: a
        file is written in one step, except tile-compressed FITS, which reports as it goes).
        An exception it raises stops the work.

    About a NumPy array (an :class:`Image` brings its own): ``keywords``, ``name``, ``bounds``,
    ``icc_profile``, ``row_order`` (of the array: "top-down" or "bottom-up"), ``channels``
    ("last" or "first"), ``wcs_row_order`` and ``properties`` (XISF properties of the image,
    ``{id: value}``), as described for :class:`Image`. Keywords describe the array as it is
    given; BAYERPAT and WCS keywords are converted when the rows are stored in the other order.

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
                         row_order=row_order, channels=channels, wcs_row_order=wcs_row_order, properties=properties)
        prepared.append(item)

    options = _lib.struct(_lib.WriteOptions, _library.xisfconv_write_options_init)
    options.format = _output_format(format)
    options.codec = _codec(codec)
    options.checksum = _checksum(checksum)
    options.row_order = _rows(stored_row_order)
    options.subblock_size = _subblock(subblock_size, options.subblock_size)
    options.wcs = int(bool(wcs))
    options.overwrite = int(bool(overwrite))
    options.shuffle = int(bool(shuffle))
    if level is not None:
        if isinstance(level, bool):
            raise TypeError("the compression level is a number, not %r" % level)
        options.compression_level = operator.index(level)
        if options.compression_level <= 0:
            raise ValueError("the compression level is 1 or more (None: the usual one of the codec)")
    if creator is not None:
        if not isinstance(creator, str):
            raise TypeError("creator is the name of a program, a string")
        options.creator_application = _bytes(creator, "creator")

    context = _Context.borrow()
    with context.lock:
        context.about(path, reading=False)
        writer = c_void_p()
        whole = _properties_handle(context, file_properties, "the properties of the file")
        try:
            options.properties = whole
            context.call(_library.xisfconv_writer_new, context.pointer, _path(path), byref(options), byref(writer),
                         undo=lambda: _library.xisfconv_writer_discard(writer))
        finally:
            if whole is not None:
                _library.xisfconv_properties_free(whole)
        kind = _written_format(format, path)
        try:
            for number, item in enumerate(prepared):
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
                handle = own = None
                try:
                    if item.keywords is not None and len(item.keywords):
                        handle = Keywords(item.keywords)._to_handle(context)
                        image.keywords = handle
                    given = item.properties
                    if isinstance(given, Properties):      # those of an open file
                        given = PropertyDict(given)
                    if isinstance(given, PropertyDict) and kind not in ("tiff", "png"):
                        # A solution that was read from a file describes the WCS keywords and the
                        # size it was read with. With others it is left out. (TIFF and PNG have
                        # no place for properties at all.)
                        stale, set_since = _stale_solution(context, given, item.keywords, planar.shape[2],
                                                           planar.shape[1], item.wcs_row_order or item.row_order)
                        if stale:
                            given = given._without(stale)
                            _solution_left_out(context._name() + (" (image %d)" % number if len(prepared) > 1 else ""),
                                               set_since, item.keywords, wcs, kind)
                    own = _properties_handle(context, given, "the properties of an image")
                    image.properties = own
                    context.call(_library.xisfconv_writer_add_image, writer, byref(image))
                finally:
                    if handle is not None:
                        _library.xisfconv_keywords_free(handle)
                    if own is not None:
                        _library.xisfconv_properties_free(own)
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


def _smaller(bin, resize, scale):   # noqa: A002
    """bin, fit_width, fit_height and scale of the conversion options."""
    if isinstance(bin, bool) or not isinstance(bin, numbers.Integral) or not 1 <= bin <= 1000000:
        raise ValueError("bin expects a number of pixels, 1 or more (2 makes one pixel of 2 x 2), not %r" % (bin,))
    width = height = 0
    if resize is not None:
        if isinstance(resize, numbers.Integral) and not isinstance(resize, bool):
            box = (resize, resize)
        elif isinstance(resize, (tuple, list, np.ndarray)):
            box = tuple(np.asarray(resize).tolist()) if isinstance(resize, np.ndarray) else tuple(resize)
        else:
            box = ()
        whole = len(box) == 2 and all(isinstance(side, numbers.Integral) and not isinstance(side, bool) and 0 < side < 2 ** 63
                                      for side in box)
        if whole:
            width, height = box
        else:
            raise ValueError("resize expects the longest side in pixels (256) or a box to fit, (width, height), not %r"
                             % (resize,))
    if scale is None:
        scale = 0.0
    elif isinstance(scale, bool) or not isinstance(scale, numbers.Real) or not 0 < scale <= 1:
        raise ValueError("scale expects a fraction of the image, above 0 and up to 1 (0.5 halves width and height), not %r"
                         % (scale,))
    return int(bin), int(width), int(height), float(scale)


def convert(input, output, *, format=None, sample_format=None, image=None, stretch=None, codec=None, checksum=None,
            subblock_size=None, row_order=None, property_keywords=True, wcs=True, sip_order=3, verify=True, bounds=None,
            overwrite=False, progress=None, properties=True, bin=1, resize=None, scale=None,
            external_files=None, debayer=False):   # noqa: A002 - the names of the command line
    """Converts a file, as the command line tool does: XISF to FITS, ASDF, TIFF or PNG; FITS
    and ASDF to XISF, to each other, or to TIFF or PNG; FITS to FITS to pack a file
    (``codec=True``: tile-compressed) or to unpack one. (XISF to XISF is :func:`rewrite`.)

    An XISF input is a monolithic file or the header file of a distributed unit (.xish); how
    far that header is followed, ``external_files`` says (see :func:`open`). XISF output under
    a name that ends in ".xish" is written as a distributed unit: the header there, the data
    blocks in the file of the same name that ends in ".xisb".

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
    properties
        ``--no-properties`` is False. From XISF to FITS and ASDF the XISF properties of the
        images and of the file are written along, with their types and exact values: in a
        FITS file as a table behind each image, in an ASDF file under the key ``xisf`` of the
        tree. Converted to XISF again, such a file gives them back: the processing history,
        the instrument and observation properties, and the astrometric solution as PixInsight
        wrote it, as long as the WCS keywords of the file are still the ones it was written
        with (else the solution is made from the keywords). False leaves the properties out,
        and leaves alone those a FITS or ASDF file carries.
    bin, resize, scale
        A smaller picture, for TIFF and PNG output (``--bin``, ``--resize``). ``bin=2`` makes
        one pixel of every 2 x 2. ``resize=256`` makes the longest side 256 pixels,
        ``resize=(1024, 768)`` fits the picture into that width and height, its proportions
        kept. ``scale=0.5`` halves width and height. Every pixel of the picture is the mean
        of the pixels it covers, taken of the image as it is stored: before a stretch. A
        picture is never larger than the image. With ``bin`` and one of the others the
        blocks come first; with ``resize`` and ``scale`` the picture is the smaller of the two.
    debayer
        ``--debayer``: a colour picture of the mosaic of a one-shot colour camera, for TIFF and
        PNG output (bilinear, by the image's 2 x 2 pattern of R, G and B, or BAYERPAT; before
        ``bin``, ``resize`` and a stretch; no white balance). An image without such a pattern is
        written as it is, with a warning. (Since 0.18.1.)
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
    options.properties = int(bool(properties))
    options.bin, options.fit_width, options.fit_height, options.scale = _smaller(bin, resize, scale)
    options.debayer = int(bool(debayer))
    context = _Context.borrow()
    with context.lock:
        context.about(input, other=output, external_files=external_files)
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
            overwrite=False, progress=None, external_files=None):   # noqa: A002
    """Writes an XISF file again with its data blocks stored another way: another compression,
    checksums added or removed, one image of several. Returns a :class:`RewriteResult`.

    The input is a monolithic file or the header file of a distributed unit, and so is the
    output, by its name: ``rewrite("frame.xish", "frame.xisf")`` packs a distributed unit into
    one file, and ``rewrite("frame.xisf", "frame.xish")`` writes ``frame.xish`` (the header)
    and ``frame.xisb`` (every data block) beside it. Whatever files the input has its blocks
    in, all of them end up in the output.

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
    external_files
        How far the header of the input is followed: see :func:`open`.
    """
    options = _rewrite_options(codec, checksum, image, verify, read_back, subblock_size, overwrite)
    result = _lib.struct(_lib.RewriteResult, _library.xisfconv_rewrite_result_init)
    context = _Context.borrow()
    with context.lock:
        context.about(input, other=output, external_files=external_files)
        context.call(_library.xisfconv_rewrite, context.pointer, _path(input), _path(output), byref(options),
                     byref(result), progress=progress)
        return _rewrite_result(result)


def rewrite_in_place(path, *, codec=None, checksum=None, image=None, verify=True, subblock_size=None, overwrite=False,
                     progress=None, external_files=None):
    """Replaces an XISF file by its rewritten self. The new file is written next to it, read
    back and compared, flushed to the disk, and only then renamed over the original. A file
    that is already stored as requested is left alone (``changed`` is False). The options are
    those of :func:`rewrite`; ``overwrite`` only decides whether a leftover ``path + ".part"``
    of an interrupted run may be overwritten.

    A distributed unit is replaced by its header file and the data blocks file of the header's
    name (``frame.xisb`` for ``frame.xish``), which then holds every block; other files the
    header named before are left where they are. Two files cannot be replaced in one step: the
    data blocks file that is there is set aside, the new files take their places, and if one
    of them cannot, it is put back and the unit is as it was. A data blocks file that also
    holds blocks this header does not name (those of another header) is replaced only with
    ``overwrite=True``: without it :class:`OutputExistsError`."""
    options = _rewrite_options(codec, checksum, image, verify, True, subblock_size, overwrite)
    result = _lib.struct(_lib.RewriteResult, _library.xisfconv_rewrite_result_init)
    context = _Context.borrow()
    with context.lock:
        context.about(path, external_files=external_files)
        context.call(_library.xisfconv_rewrite_in_place, context.pointer, _path(path), byref(options), byref(result),
                     progress=progress)
        return _rewrite_result(result)


def stored_as_requested(path, *, codec=None, checksum=None, image=None, external_files=None):
    """True if every data block of the XISF file is already stored the way the options of
    :func:`rewrite` ask, judged by the header alone."""
    options = _rewrite_options(codec, checksum, image, True, True, None, False)
    out = c_int32()
    context = _Context.borrow()
    with context.lock:
        context.about(path, external_files=external_files)
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
        "xisf", "fits", "asdf" or "dng".
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


def verify(path, *, progress=None, external_files=None):
    """Reads a file completely without converting anything and reports whether it is intact:
    a :class:`Report`. A damaged or unreadable file is not an exception here: its report says
    "failed" and why. Of a distributed XISF unit (its header file, .xish) every block is read
    from the file the header names for it; a block in a file the header is not followed to
    (``external_files``, see :func:`open`) is reported as not checked."""
    context = _Context.borrow()
    with context.lock:
        context.about(path, external_files=external_files)
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
