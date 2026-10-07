# Security

xisfconv reads files that come from other people: an image somebody sent, a download, a frame from
a shared archive. A file like that must not be able to do more than be a bad image.

## Reporting a problem

Please report a security problem privately, not in a public issue:
[Report a vulnerability](https://github.com/jkobierczynski/xisfconv/security/advisories/new) (the
"Security" tab of the repository). If that page is not open to you, open an ordinary issue that
says only that you have a security report, without the details, and you will be asked how to
reach you.

A report is most useful with the file that shows the problem, or a way to make it, the command or
the call, the version (`xisfconv --version`) and the platform. Fixes are made in the newest
version; there are no branches for older ones.

## What counts

For a file that is given to the tool, to the library or to the Python package:

- memory that is read or written out of bounds, a crash, a call that never returns;
- memory or disk space asked for out of all proportion to the size of the file;
- a file read that the caller did not name and that the rule below does not allow, or its
  content ending up in an output;
- a file written, replaced or removed that is not the output that was asked for.

## What is done about it

- **Which files are read.** The header of a distributed XISF unit says where its data is. It is
  followed only to files in its own directory and below, and only from a header file named as one
  (`.xish`): a path that leaves the directory (`..`, an absolute path, a `file:` URL, a symbolic
  link that leads out) is refused, and a monolithic `.xisf` file that names another file is not
  followed there at all. `--external-files` (`xisfconv_context_set_external_files`,
  `external_files=` in Python) widens that to any file, or narrows it to none; a program that
  opens files from people it does not know should leave it as it is. Only regular files are read,
  and a refusal does not say where a link leads.
- **Nothing is fetched from a network.** A block at an `http:` or `ftp:` URL is reported as not
  supported.
- **Sizes are held against the file.** A size in a header that the stored data cannot account for
  is refused before memory is taken for it, and the properties of a file may hold its size plus
  256 MiB together.
- **XML and YAML are read by readers of the library's own.** A document type declaration in an
  XISF header is not acted on: no entity is taken from it and nothing is loaded from elsewhere.
- **Outputs.** A file is written under another name (`<name>.part`) and renamed when it is
  complete. An existing file is replaced only when that is asked for; an output name that is a
  directory or a device is refused; a file a run reads is never its output or its temporary file.
- **Testing.** Every reader is tested with damaged and truncated files and with files that say
  more than they hold, the suites run under AddressSanitizer and UndefinedBehaviorSanitizer, and
  the readers are fuzzed. New code is reviewed by a reader who did not write it, with this kind of
  problem as the task.

## What is not promised

The rule about which files a header is followed to is for files from people you do not know. It is
not a sandbox: it does not hold against somebody who can change the directory while a file is
read. An image is read and written as a whole, in memory, so a large image needs memory of its
size, several times over when it is compressed. And the thumbnailer entry for Linux file managers
runs the tool on whatever is in a folder; file managers that run thumbnailers in a sandbox of their
own (GNOME) add that protection, others do not.
