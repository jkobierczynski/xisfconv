# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""What the library says and how it is stopped: warnings, notes, progress, Ctrl-C, threads."""

import logging
import os
import signal
import subprocess
import sys
import threading
import time
import warnings
import _thread

import numpy as np
import pytest

import xisfconv
from util import sample, same

fits = pytest.importorskip("astropy.io.fits")


def smooth(height=400, width=600):
    return (np.indices((height, width)).sum(axis=0) % 251).astype(np.uint16)


@pytest.fixture
def many_blocks(tmp_path):
    """An XISF file with enough data blocks for progress reports: 24 images."""
    path = tmp_path / "many.xisf"
    xisfconv.write(path, [xisfconv.Image(smooth(60, 80) + n, name="image%d" % n) for n in range(24)])
    return path


# --- warnings and notes -----------------------------------------------------------------------

def test_warnings_are_python_warnings(tmp_path):
    data = sample("uint8", (4, 4))
    with pytest.warns(xisfconv.XisfconvWarning) as caught:
        xisfconv.write(tmp_path / "sha3.xisf", data, checksum="sha3-512")
    assert len(caught) == 1 and "PixInsight" in str(caught[0].message)
    assert str(tmp_path / "sha3.xisf") in str(caught[0].message)       # which file it is about
    assert caught[0].filename == __file__                               # blamed on the caller, not on the package
    assert issubclass(xisfconv.XisfconvWarning, UserWarning)

    with warnings.catch_warnings():
        warnings.simplefilter("ignore", xisfconv.XisfconvWarning)
        xisfconv.write(tmp_path / "quiet.xisf", data, checksum="sha3-512")
    assert (tmp_path / "quiet.xisf").exists()

    # turned into errors, a warning is raised after the work is done
    with warnings.catch_warnings():
        warnings.simplefilter("error", xisfconv.XisfconvWarning)
        with pytest.raises(xisfconv.XisfconvWarning):
            xisfconv.write(tmp_path / "strict.xisf", data, checksum="sha3-512")
    assert xisfconv.verify(tmp_path / "strict.xisf").ok


def test_an_error_is_not_hidden_by_a_warning(tmp_path):
    """PNG holds one image: of two, the first is written with a warning. If the file exists,
    the error about that is what is raised, also when warnings are errors."""
    data = sample("uint8", (4, 4))
    (tmp_path / "there.png").write_bytes(b"x")
    with warnings.catch_warnings():
        warnings.simplefilter("error", xisfconv.XisfconvWarning)
        with pytest.raises(xisfconv.OutputExistsError):
            xisfconv.write(tmp_path / "there.png", [data, data])
    with pytest.warns(xisfconv.XisfconvWarning, match="PNG holds one image"):
        xisfconv.write(tmp_path / "new.png", [data, data])


def test_notes_go_to_the_logger(tmp_path, caplog):
    data = sample("float32", (8, 8)) * 3000
    fits.PrimaryHDU(data).writeto(tmp_path / "in.fits")
    with caplog.at_level(logging.INFO, logger="xisfconv"):
        xisfconv.convert(tmp_path / "in.fits", tmp_path / "out.xisf")
    notes = [record for record in caplog.records if record.name == "xisfconv"]
    assert notes and all(record.levelno == logging.INFO for record in notes)
    assert any("bottom-up" in record.getMessage() for record in notes)
    caplog.clear()
    with caplog.at_level(logging.WARNING, logger="xisfconv"):
        xisfconv.convert(tmp_path / "in.fits", tmp_path / "out2.xisf")
    assert not caplog.records


def test_nothing_is_printed(tmp_path, capfd):
    data = sample("float32", (8, 8)) * 3000
    fits.PrimaryHDU(data).writeto(tmp_path / "in.fits")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        xisfconv.convert(tmp_path / "in.fits", tmp_path / "out.xisf", checksum="sha3-256")
        xisfconv.convert(tmp_path / "out.xisf", tmp_path / "out.png", stretch="auto")
        xisfconv.verify(tmp_path / "out.xisf")
        with pytest.raises(xisfconv.Error):
            xisfconv.read(tmp_path / "out.png")
    out, err = capfd.readouterr()
    assert out == "" and err == ""


# --- progress and stopping --------------------------------------------------------------------

def test_progress(tmp_path, many_blocks):
    calls = []
    result = xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib",
                              progress=lambda stage, done, total: calls.append((stage, done, total)))
    assert result.compressed == 24
    stages = [stage for stage, _, _ in calls]
    assert "rewriting" in stages and "comparing" in stages
    rewriting = [(done, total) for stage, done, total in calls if stage == "rewriting"]
    assert [done for done, _ in rewriting] == sorted(done for done, _ in rewriting) and rewriting[0][1] == 24
    assert all(isinstance(stage, str) and isinstance(done, int) and isinstance(total, int) for stage, done, total in calls)

    calls.clear()
    xisfconv.verify(many_blocks, progress=lambda *arguments: calls.append(arguments))
    assert calls and {stage for stage, _, _ in calls} == {"verifying"}
    calls.clear()
    xisfconv.convert(many_blocks, tmp_path / "out.fits", progress=lambda *arguments: calls.append(arguments))
    assert {stage for stage, _, _ in calls} == {"reading", "writing"}
    calls.clear()
    xisfconv.write(tmp_path / "w.xisf", smooth(), progress=lambda *arguments: calls.append(arguments))
    assert [stage for stage, _, _ in calls] == ["writing"]


def test_an_exception_in_progress_stops_the_work(tmp_path, many_blocks):
    class Stop(Exception):
        pass

    seen = []

    def progress(stage, done, total):
        seen.append(stage)
        if stage == "rewriting" and done >= 5:
            raise Stop("enough")

    with pytest.raises(Stop, match="enough"):
        xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", progress=progress)
    assert seen.count("rewriting") == 6                                 # not called again after it raised
    assert sorted(os.listdir(tmp_path)) == ["many.xisf"]                # no output, no .part file

    def cancel(stage, done, total):
        raise xisfconv.Cancelled("stopped by the user")

    with pytest.raises(xisfconv.Cancelled, match="stopped by the user"):
        xisfconv.convert(many_blocks, tmp_path / "out.fits", progress=cancel)
    with pytest.raises(ZeroDivisionError):
        xisfconv.verify(many_blocks, progress=lambda *a: 1 / 0)
    with pytest.raises(KeyboardInterrupt):
        xisfconv.rewrite_in_place(many_blocks, codec="zlib", progress=lambda *a: (_ for _ in ()).throw(KeyboardInterrupt))
    assert sorted(os.listdir(tmp_path)) == ["many.xisf"]
    assert xisfconv.verify(many_blocks).ok                              # the original is whole
    # and the next call works as if nothing had happened
    assert xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib").compressed == 24


def test_progress_is_called_where_the_call_was_made(tmp_path, many_blocks):
    """The progress function is called in the thread of the caller, between two steps of the
    work, and may use the package itself."""
    def run(seen, number=0):
        def progress(stage, done, total):
            seen.append(threading.get_ident())
            if stage == "rewriting" and done in (1, 5):
                with xisfconv.open(many_blocks) as file:                       # another file
                    seen.append(file[done].name)
                assert xisfconv.verify(many_blocks, progress=lambda *a: seen.append("nested")).ok   # another long call
                xisfconv.write(tmp_path / ("inner%d.xisf" % number), smooth(20, 30), overwrite=True)

        out = tmp_path / ("out%d.xisf" % number)
        seen.append(xisfconv.rewrite(many_blocks, out, codec="zlib", progress=progress).compressed)
        seen.append(xisfconv.verify(out).ok)

    seen = []
    run(seen)
    assert seen[-2:] == [24, True] and "image1" in seen and "image5" in seen and "nested" in seen
    assert {entry for entry in seen[:-2] if isinstance(entry, int)} == {threading.get_ident()}

    # the same in other threads, several at once
    results = [[] for _ in range(4)]
    threads = [threading.Thread(target=run, args=(result, number + 1)) for number, result in enumerate(results)]
    for thread in threads:
        thread.start()
    for thread, result in zip(threads, results):
        thread.join()
        assert result[-2:] == [24, True] and "nested" in result
        assert {entry for entry in result[:-2] if isinstance(entry, int)} == {thread.ident}

    # the package makes no threads of its own
    assert len(sys._current_frames()) == threading.active_count()


class _Stop(Exception):
    """What the signal handler of the two tests below raises."""


class _Strikes:
    """Calls a progress handler as the library does, from C with no Python code in between, with
    a signal at another moment each time whose handler raises, and says where the exception
    went."""

    def __init__(self):
        import ctypes

        from xisfconv import _lib

        self.go_on, self.stop = _lib.HOST_GO_ON, _lib.HOST_STOP
        self.armed = []
        self.argument = ctypes.pointer(_lib.ProgressReport(None, b"testing", 1, 2))
        self.old = signal.signal(signal.SIGALRM, self.handler)

    def handler(self, signum, frame):
        # (only while a run is on: a signal that is delivered late, after its timer was stopped,
        # would strike the test itself; macOS does that)
        if self.armed:
            self.armed.clear()
            raise _Stop()

    def close(self):
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, self.old)

    def run(self, send, raised, delay):
        """"caught": the handler's exception was caught inside `send`, which answered "stop";
        "escaped": it came out of `send`; "outside": it struck between two calls, in this
        function; "quiet": there was no signal in time."""
        import itertools
        import operator
        from functools import partial

        call = operator.methodcaller("__call__")
        sends = iter([partial(send, self.argument) for _ in range(300)])
        try:
            self.armed.append(True)
            signal.setitimer(signal.ITIMER_REAL, delay)
            try:
                # in C, nothing of Python between the steps; up to the first answer that is not "go on"
                answers = list(itertools.takewhile(self.go_on.__eq__, map(call, sends)))
                answers.append(self.stop if raised[1] is not None else None)
            finally:
                self.armed.clear()
                signal.setitimer(signal.ITIMER_REAL, 0)
        except _Stop as e:
            traceback = e.__traceback__
            while traceback.tb_next is not None and traceback.tb_next.tb_frame.f_code is not _Strikes.handler.__code__:
                traceback = traceback.tb_next
            return "outside" if traceback.tb_frame.f_code is _Strikes.run.__code__ else "escaped"
        if raised[1] is None:
            return "quiet"
        # one "stop", and after it the generator has come to its end (the library asks no more)
        return "caught" if answers.count(self.stop) == 1 else "wrong"


_needs_timer = pytest.mark.skipif(not hasattr(signal, "setitimer") or threading.current_thread() is not threading.main_thread(),
                                  reason="needs an interval timer (POSIX) and the main thread")


@_needs_timer
def test_no_exception_escapes_the_reporter():
    """What the library calls between its steps is the `send` of a generator, because that
    catches whatever a signal handler raises at the moment it is entered; an ordinary function
    cannot (see the next test). Driven here as the library drives it, with a signal at another
    moment each time: every exception is caught inside, none comes out."""
    from xisfconv import _core

    strikes = _Strikes()
    calls = []
    try:
        outcomes = {}
        deadline = time.monotonic() + 2.0
        n = 0
        while time.monotonic() < deadline:
            n += 1
            state = [lambda *a: calls.append(a), None]
            reporter = _core._reporter(state)
            next(reporter)
            outcome = strikes.run(reporter.send, state, 20e-6 + (n % 50) * 3e-6)
            outcomes[outcome] = outcomes.get(outcome, 0) + 1
            try:
                reporter.close()      # (here, not when it is collected, while a late signal may still come)
            except _Stop:
                pass
            if outcome == "caught":
                assert isinstance(state[1], _Stop)
        assert outcomes.get("caught", 0) > 50 and not outcomes.get("escaped") and not outcomes.get("wrong"), outcomes
        assert calls and calls[0] == ("testing", 1, 2)
    finally:
        strikes.close()


@_needs_timer
@pytest.mark.skipif(sys.version_info < (3, 11), reason="before Python 3.11 the interpreter did not look for signals at the "
                                                       "start of a function that begins with `try`")
def test_a_function_in_the_place_of_the_reporter_loses_exceptions():
    """Why the reporter is a generator: a function with the same `try` around all of its body
    lets the exception of a signal handler out, when the signal arrives as the function is
    entered. That is a matter of microseconds, and of how the system times its signals: where
    no signal lands there within the time given, there is nothing to show, and the test is
    skipped (it says something about the interpreter, not about the package)."""
    from xisfconv import _lib

    def function(pointer):
        try:
            return _lib.HOST_GO_ON
        except BaseException:      # noqa: BLE001
            return _lib.HOST_STOP

    strikes = _Strikes()
    try:
        outcomes = {}
        deadline = time.monotonic() + 3.0
        n = 0
        while time.monotonic() < deadline and not outcomes.get("escaped"):
            n += 1
            outcome = strikes.run(function, [None, None], 5e-6 + (n % 97) * 2e-6)
            outcomes[outcome] = outcomes.get(outcome, 0) + 1
    finally:
        strikes.close()
    if not outcomes.get("escaped"):
        pytest.skip("no signal arrived at the moment a function is entered, in %d tries: %r" % (n, outcomes))


def test_a_progress_report_that_fails(tmp_path, many_blocks, monkeypatch):
    """If what the library calls between its steps does not come back with an answer, the work
    is stopped and the call says why. Made to happen here by an answer the library does not
    know. The next call makes itself a new reporter if the old one has come to an end."""
    from xisfconv import _core, _lib

    monkeypatch.setattr(_lib, "HOST_GO_ON", 0)
    with pytest.raises(xisfconv.Cancelled, match="the progress report did not come back"):
        xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", progress=lambda *a: None)
    with pytest.raises(xisfconv.Cancelled, match="the progress report did not come back"):
        xisfconv.verify(many_blocks)                                   # without a progress function too
    assert sorted(os.listdir(tmp_path)) == ["many.xisf"]
    monkeypatch.undo()
    assert xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", progress=lambda *a: None).compressed == 24

    context = _core._local.context
    old = context._reporter
    old.close()
    assert old.gi_frame is None
    calls = []
    assert xisfconv.verify(many_blocks, progress=lambda *a: calls.append(a)).ok and calls
    assert context._reporter is not old and context._reporter.gi_frame is not None


def test_calls_inside_calls_come_to_an_end(tmp_path, many_blocks):
    """A progress function that uses the package runs inside the call that reports to it. One
    that does so without end is stopped by an exception, as endless recursion is in Python."""
    depths = []

    def progress(stage, done, total):
        depths.append(len(depths))
        xisfconv.verify(many_blocks, progress=progress)

    with pytest.raises(RecursionError, match="nested"):
        xisfconv.verify(many_blocks, progress=progress)
    assert 10 <= len(depths) <= 64
    assert xisfconv.verify(many_blocks, progress=lambda *a: None).ok


@pytest.mark.skipif(threading.current_thread() is not threading.main_thread(), reason="signals reach the main thread only")
def test_an_interrupt_from_another_thread(tmp_path):
    """Ctrl-C as another thread sends it (and as it always arrives on Windows), during a call
    without a progress function: the work stops, nothing is left, the next call works."""
    big = tmp_path / "big.xisf"
    xisfconv.write(big, [xisfconv.Image(smooth(500, 700) + n, name="i%d" % n) for n in range(40)])
    old = signal.signal(signal.SIGINT, signal.default_int_handler)
    try:
        for attempt, work in enumerate([lambda: xisfconv.rewrite(big, tmp_path / "out.xisf", codec="zlib"),
                                        lambda: xisfconv.convert(big, tmp_path / "out.fits"),
                                        lambda: xisfconv.verify(big),
                                        lambda: xisfconv.rewrite_in_place(big, codec="zlib")] * 3):
            timer = threading.Timer(0.01 + 0.004 * attempt, _thread.interrupt_main)
            timer.start()
            started = time.monotonic()
            try:
                with pytest.raises(KeyboardInterrupt):
                    while time.monotonic() - started < 10:
                        work()
                        for name in ("out.xisf", "out.fits"):
                            if (tmp_path / name).exists():
                                os.remove(tmp_path / name)
            finally:
                timer.cancel()
                timer.join()
            # (an output is there only if the interrupt came after the call that wrote it)
            assert not [name for name in os.listdir(tmp_path) if name.endswith(".part")], attempt
            for name in ("out.xisf", "out.fits"):
                if (tmp_path / name).exists():
                    assert xisfconv.verify(tmp_path / name).ok
                    os.remove(tmp_path / name)
            assert time.monotonic() - started < 5
    finally:
        signal.signal(signal.SIGINT, old)
    assert xisfconv.verify(big).ok


@pytest.mark.skipif(not hasattr(os, "fork"), reason="needs fork (POSIX)")
def test_a_forked_process_ends(tmp_path, many_blocks):
    """A child process forked while another thread was inside a call does not wait at its end
    for a call that is not running in it."""
    program = """
import os, sys, threading, time, warnings
import xisfconv

warnings.simplefilter("ignore", DeprecationWarning)      # (fork with threads: Python says it is a risk)
path, stop = sys.argv[1], []

def loop():
    while not stop:
        xisfconv.rewrite(path, path + ".out.xisf", codec="zlib", overwrite=True, progress=lambda *a: time.sleep(0.001))

thread = threading.Thread(target=loop)
thread.start()
time.sleep(0.2)
child = os.fork()
if child == 0:
    assert xisfconv.verify(path).ok
    sys.exit(0)
deadline = time.monotonic() + 20
status = None
while status is None and time.monotonic() < deadline:
    done, code = os.waitpid(child, os.WNOHANG)
    if done:
        status = code
    time.sleep(0.02)
if status is None:
    os.kill(child, 9)
    os.waitpid(child, 0)
stop.append(1)
thread.join()
print("child:", status)
"""
    for _ in range(3):
        done = script(tmp_path, program, many_blocks)
        assert (done.returncode, done.stdout, done.stderr) == (0, "child: 0\n", "")


def test_a_file_that_a_handler_closes(tmp_path, many_blocks, monkeypatch):
    """A signal handler may run between two calls of the library, also inside a method that
    makes several (reading an image does), and may close the file there: the method then finds
    the file closed. It cannot run inside a call on an open file, because those have no steps;
    should the library ever say that a call is running, the file is refused."""
    from xisfconv import _core

    big = tmp_path / "big.xisf"
    xisfconv.write(big, [xisfconv.Image(smooth(30, 40) + n, name="i%d" % n) for n in range(6)], codec="zlib")
    with xisfconv.open(big) as file:
        context = file._context
        calls = []
        original = context.call

        def call(function, *arguments, **options):
            calls.append(function)
            if len(calls) == 2:
                file.close()
            return original(function, *arguments, **options)

        context.call = call
        with pytest.raises(ValueError, match="the file is closed"):
            file[2].read()
        assert len(calls) == 2 and file.closed
        del context.call
    assert file.closed
    for use in (lambda: file[0].read(), lambda: file[0].keywords, lambda: file[0].shape, lambda: file.format):
        with pytest.raises(ValueError, match="closed"):
            use()

    with xisfconv.open(big) as file:
        with monkeypatch.context() as patch:
            patch.setattr(_core._library, "xisfconv_context_running", lambda pointer: 1)
            for use in (lambda: file[0].read(), lambda: file[0].keywords, file.close):
                with pytest.raises(RuntimeError, match="in use"):
                    use()
        assert not file.closed and same(file[5].read(), smooth(30, 40) + 5)
    assert file.closed


def script(tmp_path, text, *arguments, **options):
    """Runs a Python program with this package; returns what it did."""
    program = tmp_path / "program.py"
    program.write_text(text)
    return subprocess.run([sys.executable, "-X", "faulthandler", str(program)] + [str(a) for a in arguments],
                          capture_output=True, text=True, timeout=120, **options)


def test_python_ends_while_the_library_works(tmp_path, many_blocks):
    """Daemon threads are not waited for when Python ends. The library is stopped first: a
    thread that came back from it into an interpreter being taken down would crash."""
    program = """
import sys, threading, time
import xisfconv

path, delay = sys.argv[1], float(sys.argv[2])

def loop(n):
    try:
        while True:
            xisfconv.verify(path, progress=lambda *a: None)
            xisfconv.rewrite(path, "%s.%d.xisf" % (path, n), codec="zlib", overwrite=True)
    except xisfconv.Cancelled as e:
        assert "Python is ending" in str(e), e

for n in range(4):
    threading.Thread(target=loop, args=(n,), daemon=True).start()
time.sleep(delay)
print("bye")
"""
    for delay in (0.05, 0.11, 0.2, 0.31):
        done = script(tmp_path, program, many_blocks, delay)
        assert (done.returncode, done.stdout, done.stderr) == (0, "bye\n", ""), delay
        assert not [name for name in os.listdir(tmp_path) if name.endswith(".part")]
    assert xisfconv.verify(many_blocks).ok


def test_work_while_python_ends(tmp_path):
    """A function that the program has registered to run at exit can still use the package,
    whether it runs before the package stops its threads or after."""
    program = """
import atexit, sys
import numpy as np

def save(name):
    import xisfconv
    xisfconv.write(name + ".xisf", np.arange(12, dtype=np.uint16).reshape(3, 4))
    xisfconv.convert(name + ".xisf", name + ".fits", progress=lambda *a: None)
    assert xisfconv.verify(name + ".xisf").ok
    print("saved", name[-5:])

atexit.register(save, sys.argv[1] + "after")      # registered first: runs last
import xisfconv
atexit.register(save, sys.argv[1] + "first")
"""
    done = script(tmp_path, program, str(tmp_path) + os.sep)
    assert (done.returncode, done.stdout, done.stderr) == (0, "saved first\nsaved after\n", "")
    for name in ("first", "after"):
        assert same(xisfconv.read(tmp_path / (name + ".fits")), np.arange(12, dtype=np.uint16).reshape(3, 4))


needs_main_thread = pytest.mark.skipif(threading.current_thread() is not threading.main_thread(),
                                       reason="signals reach the main thread only")
# os.kill ends the process on Windows, whatever the signal
from_outside = pytest.mark.skipif(sys.platform == "win32", reason="needs a signal sent to the process (POSIX)")


@pytest.fixture
def ctrl_c():
    """Python's own handling of SIGINT, whatever the tests were started with. (A program put in
    the background by a shell script ignores the signal.)"""
    old = signal.signal(signal.SIGINT, signal.default_int_handler)
    yield
    signal.signal(signal.SIGINT, old)


@needs_main_thread
def test_ctrl_c_stops_the_work(tmp_path, many_blocks, ctrl_c):
    """SIGINT while the library works: the work stops, KeyboardInterrupt is raised, no file is
    left half written."""
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler

    def progress(stage, done, total):
        if stage == "rewriting" and done == 3:
            signal.raise_signal(signal.SIGINT)
        assert not (stage == "rewriting" and done > 4), "the work went on after Ctrl-C"

    with pytest.raises(KeyboardInterrupt):
        xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", progress=progress)
    assert sorted(os.listdir(tmp_path)) == ["many.xisf"]
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


@needs_main_thread
@from_outside
@pytest.mark.parametrize("work", ["messages", "no callbacks", "progress"])
def test_ctrl_c_is_never_lost(tmp_path, many_blocks, work, ctrl_c):
    """SIGINT from outside, at any moment of a loop of library calls: the loop ends with
    KeyboardInterrupt. (A Python exception cannot travel through the C code of a callback;
    without care the interrupt would be printed and forgotten.)"""
    data = sample("uint8", (16, 16))
    big = tmp_path / "big.xisf"
    xisfconv.write(big, smooth(1500, 2000))

    def loop():
        # (a loop that an interrupt fails to end gives up after a while, and the test fails)
        deadline = time.monotonic() + 15
        if work == "messages":            # each call makes the library send a warning
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", xisfconv.XisfconvWarning)
                while time.monotonic() < deadline:
                    xisfconv.write(tmp_path / "w.xisf", data, checksum="sha3-256", overwrite=True)
        elif work == "no callbacks":      # the library is not heard from while it reads
            while time.monotonic() < deadline:
                xisfconv.read(big)
        else:
            while time.monotonic() < deadline:
                xisfconv.verify(many_blocks, progress=lambda *a: None)

    for attempt in range(6):
        timer = threading.Timer(0.05 + 0.037 * attempt, os.kill, (os.getpid(), signal.SIGINT))
        timer.start()
        started = time.monotonic()
        try:
            with pytest.raises(KeyboardInterrupt):
                loop()
        finally:
            timer.cancel()
            timer.join()
        assert time.monotonic() - started < 20
        assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


@needs_main_thread
def test_a_handler_of_the_program_is_called(tmp_path, many_blocks, ctrl_c):
    """A program with its own SIGINT handler: the handler is called when the signal comes, the
    work goes on if the handler does not object, and the program has its handler back."""
    hits = []

    def handler(signum, frame):
        hits.append(signum)

    def progress(stage, done, total):
        if stage == "rewriting" and done == 2:
            signal.raise_signal(signal.SIGINT)
            assert hits == [signal.SIGINT]                              # called at once, not later

    signal.signal(signal.SIGINT, handler)
    result = xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", progress=progress)
    assert result.compressed == 24 and hits == [signal.SIGINT]
    assert signal.getsignal(signal.SIGINT) is handler

    class Stop(Exception):
        pass

    def stopping(signum, frame):
        raise Stop("the program says stop")

    signal.signal(signal.SIGINT, stopping)
    with pytest.raises(Stop, match="the program says stop"):             # what the handler raises reaches the caller
        xisfconv.rewrite(many_blocks, tmp_path / "second.xisf", codec="zlib", progress=progress)
    assert not (tmp_path / "second.xisf").exists() and signal.getsignal(signal.SIGINT) is stopping


@needs_main_thread
def test_what_the_program_does_with_its_handlers_stays(tmp_path, many_blocks, ctrl_c):
    """A handler that the program sets while the library works is not taken away again, and a
    handler that ends the program does."""
    def first(signum, frame):                       # "press again to abort"
        signal.signal(signal.SIGINT, signal.default_int_handler)

    def progress(stage, done, total):
        if stage == "rewriting" and done == 2:
            signal.raise_signal(signal.SIGINT)

    signal.signal(signal.SIGINT, first)
    assert xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", progress=progress).compressed == 24
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler

    # SIGTERM with the usual handler: the program ends, although the signal came during a callback
    def leave(signum, frame):
        raise SystemExit(3)

    def terminate(stage, done, total):
        if stage == "rewriting" and done == 2:
            signal.raise_signal(signal.SIGTERM)

    old = signal.signal(signal.SIGTERM, leave)
    try:
        with pytest.raises(SystemExit):
            xisfconv.rewrite(many_blocks, tmp_path / "second.xisf", codec="zlib", progress=terminate)
        assert signal.getsignal(signal.SIGTERM) is leave and not (tmp_path / "second.xisf").exists()
    finally:
        signal.signal(signal.SIGTERM, old)


@needs_main_thread
@pytest.mark.skipif(not hasattr(signal, "setitimer"), reason="needs an interval timer (POSIX)")
def test_another_signal_handler_that_raises(tmp_path, many_blocks):
    """An alarm whose handler raises, a common way to put a time limit on work: the exception
    reaches the caller of the library, at any moment it comes."""
    class TimeUp(Exception):
        pass

    def handler(signum, frame):
        raise TimeUp("time is up")

    old = signal.signal(signal.SIGALRM, handler)
    try:
        for attempt in range(6):
            signal.setitimer(signal.ITIMER_REAL, 0.03 + 0.021 * attempt)
            deadline = time.monotonic() + 15
            with pytest.raises(TimeUp):
                while time.monotonic() < deadline:
                    xisfconv.verify(many_blocks, progress=lambda *a: None)
            assert signal.getsignal(signal.SIGALRM) is handler
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


@needs_main_thread
@pytest.mark.skipif(not hasattr(signal, "setitimer"), reason="needs an interval timer (POSIX)")
def test_a_signal_handler_that_uses_the_package(tmp_path, many_blocks):
    """A handler may use the package while the program is in it: what the program is told
    (errors, warnings, results) is about its own call."""
    data = sample("uint8", (4, 4))
    xisfconv.write(tmp_path / "there.xisf", data)
    hits = []

    busy = []

    def handler(signum, frame):
        if busy:                # the next alarm comes while this one is at work
            return
        busy.append(1)
        try:
            hits.append(1)
            n = len(hits) % 7
            xisfconv.write(tmp_path / ("handler%d.fits" % n), data, overwrite=True, keywords={"N": n})
            assert xisfconv.read_image(tmp_path / ("handler%d.fits" % n)).keywords["N"] == n
            if n == 3:
                assert xisfconv.verify(many_blocks, progress=lambda *a: None).ok
            with pytest.raises(FileNotFoundError, match="handler-none"):
                xisfconv.convert(tmp_path / "handler-none.xisf", tmp_path / "x.fits")
        finally:
            busy.clear()

    from xisfconv._core import _Context

    one = _Context.borrow()                             # the mechanism itself
    with one.lock:
        one.about("a")
        two = _Context.borrow()
        with two.lock:
            two.about("b")
            assert one is not two and one.path == "a" and two.path == "b"
    assert _Context.borrow() is two

    old = signal.signal(signal.SIGALRM, handler)
    try:
        signal.setitimer(signal.ITIMER_REAL, 0.002, 0.002)
        deadline = time.monotonic() + 2.5
        rounds = 0
        while time.monotonic() < deadline:
            rounds += 1
            with pytest.raises(FileExistsError) as caught:
                xisfconv.write(tmp_path / "there.xisf", data)
            assert "there.xisf" in str(caught.value) and "overwrite=True" in str(caught.value)
            with pytest.raises(xisfconv.ArgumentError) as caught:
                xisfconv.convert(many_blocks, tmp_path / "out.unknown")
            assert "many.xisf" in str(caught.value) and "format=" in str(caught.value)
            with pytest.warns(xisfconv.XisfconvWarning) as warned:
                xisfconv.write(tmp_path / "sha3.xisf", data, checksum="sha3-256", overwrite=True)
            assert [str(w.message) for w in warned if "sha3.xisf" in str(w.message)] and \
                all("sha3.xisf" in str(w.message) for w in warned)
            assert xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", overwrite=True,
                                    progress=lambda *a: None).compressed == 24
            assert Keywords_text() == 80
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)
    assert rounds >= 3 and len(hits) >= 20, (rounds, len(hits))


def Keywords_text():
    return len(xisfconv.Keywords({"OBJECT": "M 31"}).fits_text())


@needs_main_thread
def test_an_ignored_signal_stays_ignored(tmp_path, many_blocks):
    """A program that ignores SIGINT is not interrupted, and still ignores it afterwards."""
    old = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        def progress(stage, done, total):
            if stage == "rewriting" and done == 2:
                signal.raise_signal(signal.SIGINT)

        assert xisfconv.rewrite(many_blocks, tmp_path / "out.xisf", codec="zlib", progress=progress).compressed == 24
        assert signal.getsignal(signal.SIGINT) == signal.SIG_IGN
    finally:
        signal.signal(signal.SIGINT, old)


# --- threads ----------------------------------------------------------------------------------

def test_threads(tmp_path):
    """Separate files from several threads at once, and one open file shared between threads."""
    images = {n: sample("uint16", (120, 160), seed=n) for n in range(8)}
    errors = []

    def work(n):
        try:
            for round_ in range(5):
                path = tmp_path / ("t%d-%d.xisf" % (n, round_))
                xisfconv.write(path, images[n], codec="zlib", checksum="sha1", keywords={"N": n})
                image = xisfconv.read_image(path)
                assert same(image.data, images[n]) and image.keywords["N"] == n
                xisfconv.convert(path, tmp_path / ("t%d-%d.fits" % (n, round_)))
                assert xisfconv.verify(path).ok
        except BaseException as e:   # noqa: B902
            errors.append(e)

    threads = [threading.Thread(target=work, args=(n,)) for n in images]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert not errors, errors
    for n in images:
        with fits.open(tmp_path / ("t%d-4.fits" % n)) as hdus:
            assert same(hdus[0].data, images[n][::-1])

    shared = tmp_path / "shared.xisf"
    xisfconv.write(shared, [xisfconv.Image(images[n], name="i%d" % n) for n in images])
    with xisfconv.open(shared) as file:
        def read(n):
            try:
                for _ in range(10):
                    assert same(file[n].read(), images[n]) and file[n].name == "i%d" % n
                    assert same(file[n].read(row_order="bottom-up"), images[n][::-1])
            except BaseException as e:   # noqa: B902
                errors.append(e)

        threads = [threading.Thread(target=read, args=(n,)) for n in images]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
    assert not errors, errors


def test_many_files_are_closed(tmp_path):
    """Files that are not closed by hand are closed when they are no longer used."""
    path = tmp_path / "f.xisf"
    xisfconv.write(path, sample("uint8", (4, 4)))
    if not sys.platform.startswith("linux"):
        pytest.skip("counts the open files of the process through /proc")
    before = len(os.listdir("/proc/self/fd"))
    for _ in range(300):
        xisfconv.open(path)[0].name
    import gc

    gc.collect()
    assert len(os.listdir("/proc/self/fd")) <= before + 2
