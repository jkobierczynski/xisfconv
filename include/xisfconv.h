/*
 * xisfconv.h - C API of libxisfconv
 *
 * Reads and writes PixInsight XISF images and converts between XISF, FITS and ASDF, with TIFF
 * and PNG export; rewrites the block storage of XISF files and verifies files.
 *
 * FITS and ASDF are supported as far as images need them. For tables and everything else in
 * those formats, use CFITSIO, astropy or the Python asdf package.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 Jurgen Kobierczynski
 *
 * Conventions
 * -----------
 * Language    Plain C99. Usable from C++ (extern "C") and through any C FFI: Python ctypes or
 *             cffi, Perl FFI::Platypus, Rust bindgen.
 * Errors      Every function that can fail returns an xisfconv_status. XISFCONV_OK is 0. The
 *             text of the last failure is kept in the context: xisfconv_error_message(ctx).
 *             No C++ exception leaves the library and the library never calls exit() or abort().
 * Output      The library writes nothing to stdout or stderr. Warnings and notes go to the
 *             message handler of the context; without a handler they are dropped. Some messages
 *             name options of the xisfconv command line tool (--force, --bounds): they are the
 *             tool's messages, and the option names say which setting is meant.
 * Strings     UTF-8, NUL-terminated. This includes file names on Windows, which the library
 *             converts to wide characters itself.
 * Ownership   A `const char *` or `const xisfconv_keywords *` returned by an accessor belongs to
 *             the handle it came from and stays valid until that handle is freed (or, where it
 *             says so, until the next call on it). The caller frees only what it obtained from
 *             a function ending in _new or _open, or documented as "caller frees".
 * Lifetime    A file, report or writer keeps its context alive: handles and context may be freed
 *             in any order.
 * Indices     Zero-based, size_t. Sizes are in bytes unless said otherwise.
 * Integers    Enumerations are int32_t with named constants, so that their size is the same for
 *             every compiler and FFI. Flags are int32_t, 0 or 1.
 * Structs     Every struct starts with struct_size and is filled by its _init function first,
 *             which is told the size the caller compiled with: xisfconv_read_options_init(&o,
 *             sizeof o). New fields are only ever appended, and the library reads and writes no
 *             more of a struct than its struct_size says, so a program built against an older
 *             header keeps working with a newer library.
 * Threads     No global mutable state. A context, and the handles made from it, may be used by
 *             one thread at a time. Different contexts are independent.
 * Pixels      Buffers are planar and in host byte order: channel 0 (all rows, each left to
 *             right), then channel 1, and so on. For NumPy that is shape [channels, height,
 *             width]. Row order is stated explicitly wherever pixels cross the API.
 * Stability   0.x: the API and ABI may change between releases. The shared library's version
 *             changes with every such release.
 */
#ifndef XISFCONV_H
#define XISFCONV_H

#include <stddef.h>
#include <stdint.h>

#define XISFCONV_VERSION_MAJOR 0
#define XISFCONV_VERSION_MINOR 14
#define XISFCONV_VERSION_PATCH 1

#if defined(XISFCONV_STATIC)
#  define XISFCONV_API
#elif defined(_WIN32)
#  if defined(XISFCONV_BUILDING)
#    define XISFCONV_API __declspec(dllexport)
#  else
#    define XISFCONV_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define XISFCONV_API __attribute__((visibility("default")))
#else
#  define XISFCONV_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------------------------- */

typedef int32_t xisfconv_status;
enum {
    XISFCONV_OK              = 0,
    XISFCONV_ERR_ARGUMENT    = 1,  /* NULL where a value is required, option out of range, bad combination */
    XISFCONV_ERR_IO          = 2,  /* cannot open, read, write or rename */
    XISFCONV_ERR_FORMAT      = 3,  /* the file is malformed or truncated */
    XISFCONV_ERR_UNSUPPORTED = 4,  /* a feature this library, or this build of it, does not implement;
                                      the file itself may be fine */
    XISFCONV_ERR_CHECKSUM    = 5,  /* a stored checksum does not match the data */
    XISFCONV_ERR_MEMORY      = 6,  /* allocation failed */
    XISFCONV_ERR_INDEX       = 7,  /* image, keyword or property index out of range */
    XISFCONV_ERR_EXISTS      = 8,  /* the output exists and overwrite was not requested */
    XISFCONV_ERR_BUFFER      = 9,  /* the caller's buffer is too small */
    XISFCONV_ERR_NOT_FOUND   = 10, /* no such keyword or property, no ICC profile, no astrometric solution */
    XISFCONV_ERR_CANCELLED   = 11, /* the progress handler asked to stop */
    XISFCONV_ERR_INTERNAL    = 99  /* a bug in the library */
};

/* Short fixed English name of a status ("checksum mismatch"). Never NULL. */
XISFCONV_API const char *xisfconv_status_text(xisfconv_status status);

/* ------------------------------------------------------------------------------------------
 * Library information
 * ---------------------------------------------------------------------------------------- */

/* "0.12.0" */
XISFCONV_API const char *xisfconv_version(void);
/* major * 10000 + minor * 100 + patch, for comparing at run time */
XISFCONV_API int32_t xisfconv_version_number(void);

typedef int32_t xisfconv_codec;
enum {
    XISFCONV_CODEC_KEEP    = -1, /* rewrite only: leave every block as it is stored */
    XISFCONV_CODEC_NONE    = 0,
    XISFCONV_CODEC_ZLIB    = 1,
    XISFCONV_CODEC_LZ4     = 2,  /* read only */
    XISFCONV_CODEC_LZ4HC   = 3,  /* read only */
    XISFCONV_CODEC_ZSTD    = 4,  /* needs a build with libzstd */
    XISFCONV_CODEC_DEFAULT = 5   /* writing: the usual codec of the format. XISF: Zstandard, or zlib in
                                    a build without libzstd; ASDF: zlib; TIFF: Deflate; FITS: tile
                                    compression with RICE_1 (GZIP_2 for floating point) */
};

/* 1 if this build can read (for_writing = 0) or write (for_writing = 1) the codec, else 0. */
XISFCONV_API int32_t xisfconv_codec_available(xisfconv_codec codec, int32_t for_writing);

/* ------------------------------------------------------------------------------------------
 * Context: error text, messages, progress
 * ---------------------------------------------------------------------------------------- */

typedef struct xisfconv_context xisfconv_context;

typedef int32_t xisfconv_message_level;
enum {
    XISFCONV_MESSAGE_WARNING = 1, /* something the user should know about the result */
    XISFCONV_MESSAGE_INFO    = 2  /* how a conversion was done: sample type, row order, range chosen
                                     for floating point data, WCS fit quality, stretch parameters */
};

/* `path` is the file being processed, or NULL. `message` has no trailing newline. Both are
 * valid only during the call. The handler must not call back into the same context. */
typedef void (*xisfconv_message_fn)(void *user, xisfconv_message_level level, const char *path, const char *message);

/* Called from time to time during reading, writing, rewriting and verifying. `stage` is a short
 * word ("reading", "writing", "compressing", "rewriting", "comparing", "verifying"); `total` is 0
 * when the amount of work is not known. ("compressing", for tile-compressed FITS, counts the
 * rows of the image being written: it starts again with every image, every 8 MiB or so of
 * pixels, and is not called once more when an image is complete.) Return 0 to go on, anything else to stop: the call in progress
 * then returns XISFCONV_ERR_CANCELLED and leaves no partly written file behind. */
typedef int32_t (*xisfconv_progress_fn)(void *user, const char *stage, uint64_t done, uint64_t total);

/* Returns NULL only if memory is exhausted. */
XISFCONV_API xisfconv_context *xisfconv_context_new(void);
/* NULL is allowed. Handles made from the context may live on; the handlers are no longer called. */
XISFCONV_API void xisfconv_context_free(xisfconv_context *ctx);

/* handler NULL = drop messages (the default). */
XISFCONV_API void xisfconv_context_set_message_handler(xisfconv_context *ctx, xisfconv_message_fn handler, void *user);
/* handler NULL = no progress reports (the default). */
XISFCONV_API void xisfconv_context_set_progress_handler(xisfconv_context *ctx, xisfconv_progress_fn handler,
                                                        void *user);

/* Messages without a handler. With keep != 0 the context keeps the warnings and notes of the
 * calls made in it and in the handles made from it, for the caller to fetch afterwards; keep = 0
 * (the default) ends that and drops what was kept. This is for callers who do better without a
 * callback from C: in Python, an exception raised by a signal handler cannot pass through one.
 * A handler, if one is set, is called all the same. The messages add up over the calls until
 * xisfconv_context_clear_messages: a caller that keeps them has to clear them. */
XISFCONV_API void xisfconv_context_keep_messages(xisfconv_context *ctx, int32_t keep);
XISFCONV_API size_t xisfconv_context_message_count(const xisfconv_context *ctx);
/* A kept message: its level, the file it is about (NULL if none) and its text. Any out pointer
 * may be NULL. The strings are valid until the next call in the context or
 * xisfconv_context_clear_messages. XISFCONV_ERR_INDEX beyond the last one. */
XISFCONV_API xisfconv_status xisfconv_context_message(const xisfconv_context *ctx, size_t index,
                                                      xisfconv_message_level *level, const char **path,
                                                      const char **message);
XISFCONV_API void xisfconv_context_clear_messages(xisfconv_context *ctx);

/* Asks the call that is running in this context to stop: at its next step it returns
 * XISFCONV_ERR_CANCELLED and leaves no partly written file, as when the progress handler asks.
 * This is the one function that may be called while another thread is inside a call in the
 * context (and from a signal handler). A request made while no call runs is dropped. Returns 1
 * if a call was running in the context, else 0. */
XISFCONV_API int32_t xisfconv_context_cancel(xisfconv_context *ctx);
/* 1 if a call is running in the context, else 0. For a host whose handlers run inside a call
 * (a progress handler, in Python also a signal handler): the context and the handles made from
 * it must not be used or freed by such a handler while this says 1. */
XISFCONV_API int32_t xisfconv_context_running(const xisfconv_context *ctx);

/* A second kind of progress handler, for a host that runs the library from an interpreter.
 * It differs from xisfconv_progress_fn in two ways that such a host needs.
 *
 * It takes one argument, so that anything the interpreter can call with one value can be the
 * handler. (In Python that is the `send` of a generator: a generator resumes inside its `try`
 * block, so what a signal handler raises at that moment is caught there. In an ordinary function
 * it would strike before the function's `try` and be lost.)
 *
 * Its answer tells a handler that did not finish from one that says "go on": XISFCONV_HOST_GO_ON,
 * XISFCONV_HOST_STOP, or anything else if the handler could not be run or was left by an error,
 * which stops the call too. xisfconv_context_host_progress_failed says whether that happened in
 * the last call. A call that is stopped returns XISFCONV_ERR_CANCELLED.
 *
 * It is called wherever the progress handler is, and before it. Like the other handlers it must
 * not call back into the same context; it may use other contexts. */
enum {
    XISFCONV_HOST_GO_ON = 0x676F6F6E,
    XISFCONV_HOST_STOP  = 0x73746F70
};

typedef struct xisfconv_progress_report {
    void *user;        /* as given to xisfconv_context_set_host_progress */
    const char *stage; /* valid during the call only */
    uint64_t done;
    uint64_t total;
} xisfconv_progress_report;

typedef int32_t (*xisfconv_host_progress_fn)(const xisfconv_progress_report *report);

/* handler NULL = none (the default). */
XISFCONV_API void xisfconv_context_set_host_progress(xisfconv_context *ctx, xisfconv_host_progress_fn handler,
                                                     void *user);
/* 1 if the host's progress handler stopped the last call by failing (an answer that is neither
 * XISFCONV_HOST_GO_ON nor XISFCONV_HOST_STOP). */
XISFCONV_API int32_t xisfconv_context_host_progress_failed(const xisfconv_context *ctx);

/* Text of the most recent failure in this context, or in a handle made from it; "" if there was
 * none. Valid until the next failing call or until the context is freed. */
XISFCONV_API const char *xisfconv_error_message(const xisfconv_context *ctx);

/* ------------------------------------------------------------------------------------------
 * Common types
 * ---------------------------------------------------------------------------------------- */

typedef int32_t xisfconv_format;
enum {
    XISFCONV_FORMAT_AUTO = 0, /* output: from the file extension */
    XISFCONV_FORMAT_XISF = 1,
    XISFCONV_FORMAT_FITS = 2,
    XISFCONV_FORMAT_ASDF = 3,
    XISFCONV_FORMAT_TIFF = 4, /* output only */
    XISFCONV_FORMAT_PNG  = 5  /* output only */
};

typedef int32_t xisfconv_sample_format;
enum {
    XISFCONV_SAMPLE_AS_STORED = 0, /* options only: do not convert */
    XISFCONV_SAMPLE_UINT8     = 1,
    XISFCONV_SAMPLE_UINT16    = 2,
    XISFCONV_SAMPLE_UINT32    = 3,
    XISFCONV_SAMPLE_UINT64    = 4,
    XISFCONV_SAMPLE_FLOAT32   = 5,
    XISFCONV_SAMPLE_FLOAT64   = 6
};

/* Bytes per sample: 1, 2, 4 or 8. 0 for XISFCONV_SAMPLE_AS_STORED or an unknown value. */
XISFCONV_API size_t xisfconv_sample_size(xisfconv_sample_format format);

typedef int32_t xisfconv_row_order;
enum {
    XISFCONV_ROWS_DEFAULT   = 0, /* options: the convention of the format, or what the file declares */
    XISFCONV_ROWS_TOP_DOWN  = 1, /* first row is the top of the image (XISF, TIFF, PNG) */
    XISFCONV_ROWS_BOTTOM_UP = 2  /* first row is the bottom of the image (FITS convention) */
};

typedef int32_t xisfconv_color_space;
enum {
    XISFCONV_COLOR_GRAY  = 0,
    XISFCONV_COLOR_RGB   = 1,
    XISFCONV_COLOR_OTHER = 2  /* XISF: CIELab and others, read as raw channels; see the "colorSpace" detail */
};

typedef int32_t xisfconv_checksum;
enum {
    XISFCONV_CHECKSUM_KEEP     = -1, /* rewrite only: keep what the file has, recomputed where needed */
    XISFCONV_CHECKSUM_NONE     = 0,
    XISFCONV_CHECKSUM_SHA1     = 1,
    XISFCONV_CHECKSUM_SHA256   = 2,
    XISFCONV_CHECKSUM_SHA512   = 3,
    XISFCONV_CHECKSUM_SHA3_256 = 4,  /* valid XISF 1.0, but PixInsight 1.9.3 does not open such files */
    XISFCONV_CHECKSUM_SHA3_512 = 5   /* the same */
};

typedef int32_t xisfconv_stretch;
enum {
    XISFCONV_STRETCH_NONE     = 0,
    XISFCONV_STRETCH_AUTO     = 1, /* the STF saved in the file if there is one, else linked auto-STF */
    XISFCONV_STRETCH_LINKED   = 2,
    XISFCONV_STRETCH_UNLINKED = 3,
    XISFCONV_STRETCH_STORED   = 4  /* the saved STF; XISFCONV_ERR_NOT_FOUND if the file has none */
};

/* For options that select an image: every image of the file. */
#define XISFCONV_ALL_IMAGES ((size_t)-1)

/* ------------------------------------------------------------------------------------------
 * Keyword lists
 *
 * A list of FITS-style cards (name, value, comment), in order. `value` is the FITS-formatted
 * value: strings keep their quotes ('M 31'), numbers and logicals are bare. COMMENT and
 * HISTORY cards have an empty value and their text in `comment`.
 * ---------------------------------------------------------------------------------------- */

typedef struct xisfconv_keywords xisfconv_keywords;

XISFCONV_API xisfconv_status xisfconv_keywords_new(xisfconv_context *ctx, xisfconv_keywords **out); /* caller frees */
XISFCONV_API void xisfconv_keywords_free(xisfconv_keywords *kw); /* NULL is allowed; not for lists owned by a file */

XISFCONV_API size_t xisfconv_keywords_count(const xisfconv_keywords *kw);
/* Any of the three out pointers may be NULL. The strings are valid until the list is changed
 * or freed. */
XISFCONV_API xisfconv_status xisfconv_keywords_get(const xisfconv_keywords *kw, size_t index, const char **name,
                                                   const char **value, const char **comment);
/* Index of the first card with this name (case-insensitive), or -1. */
XISFCONV_API int64_t xisfconv_keywords_find(const xisfconv_keywords *kw, const char *name);
/* value and comment may be NULL. Names that do not fit a standard card are written with
 * HIERARCH; an empty name makes a card of text only, like COMMENT. Lists owned by a file cannot
 * be changed: XISFCONV_ERR_ARGUMENT. */
XISFCONV_API xisfconv_status xisfconv_keywords_append(xisfconv_keywords *kw, const char *name, const char *value,
                                                      const char *comment);
/* Convenience: quotes and escapes `text` as a FITS string value. */
XISFCONV_API xisfconv_status xisfconv_keywords_append_string(xisfconv_keywords *kw, const char *name, const char *text,
                                                             const char *comment);
/* Convenience: a number in the form FITS expects (always with '.' or 'E'), with all the digits
 * that are needed to read back the same double. */
XISFCONV_API xisfconv_status xisfconv_keywords_append_number(xisfconv_keywords *kw, const char *name, double number,
                                                             const char *comment);
XISFCONV_API xisfconv_status xisfconv_keywords_remove(xisfconv_keywords *kw, size_t index);

/* The unquoted content of a FITS string value ("M 31" for 'M 31    '); other values are returned
 * trimmed. *out is valid until the next call on the same list. */
XISFCONV_API xisfconv_status xisfconv_keywords_get_text(const xisfconv_keywords *kw, size_t index, const char **out);

/* The list as the cards of a FITS header: 80 characters each, one after the other without a
 * separator and without the END card. This is what the FITS writer makes of the keywords of an
 * image: a string that does not fit continues on CONTINUE cards (a LONGSTRN card then comes
 * first), a name that does not fit a standard card is written with HIERARCH, text is reduced to
 * printable ASCII, the cards that describe how a FITS file stores its data are left out (see
 * xisfconv_image), and a card that cannot be written is left out with a warning. *text is valid
 * until the next call on the same list; length may be NULL. */
XISFCONV_API xisfconv_status xisfconv_keywords_fits_text(const xisfconv_keywords *kw, const char **text,
                                                         size_t *length);

/* ------------------------------------------------------------------------------------------
 * Reading files
 *
 * One model for the three input formats. A file holds a list of images:
 *   XISF  the Image elements
 *   FITS  the primary HDU and the IMAGE extensions that hold pixels, including tile-compressed ones
 *   ASDF  the HDUs of FITS-tagged nodes, then every other 2-D or 3-D numeric array
 * Opening reads the headers only. Pixels are read by xisfconv_read_pixels.
 * ---------------------------------------------------------------------------------------- */

typedef struct xisfconv_file xisfconv_file;

/* Looks at the first bytes of the file: FITS and ASDF are recognized by their signature, XISF
 * by its. XISFCONV_ERR_FORMAT if it is none of them, XISFCONV_ERR_IO if it cannot be read. */
XISFCONV_API xisfconv_status xisfconv_detect_format(xisfconv_context *ctx, const char *path, xisfconv_format *out);

/* A file that is neither FITS nor ASDF is taken for XISF, so that the XISF reader says what is
 * wrong with it. */
XISFCONV_API xisfconv_status xisfconv_open(xisfconv_context *ctx, const char *path, xisfconv_file **out);
XISFCONV_API void xisfconv_close(xisfconv_file *file); /* NULL is allowed */

XISFCONV_API xisfconv_format xisfconv_file_format(const xisfconv_file *file);
XISFCONV_API uint64_t xisfconv_file_size(const xisfconv_file *file);
XISFCONV_API size_t xisfconv_image_count(const xisfconv_file *file);

/* Details of a file as text, by name; "" if the file has no such detail.
 *   XISF  "version" (of the format, "1.0")
 *   ASDF  "format" (versions of the format and standard, number of blocks) */
XISFCONV_API const char *xisfconv_file_detail(const xisfconv_file *file, const char *name);

/* Number of parts of the file that are not convertible images (FITS tables, ASDF arrays of an
 * unsupported kind), and a one-line description of each. */
XISFCONV_API size_t xisfconv_skipped_count(const xisfconv_file *file);
XISFCONV_API const char *xisfconv_skipped_text(const xisfconv_file *file, size_t index);

/* The header as text: the XML header (XISF), the non-structural cards of the image HDUs, one per
 * line (FITS) or the YAML tree, byte for byte (ASDF). *length excludes the terminating NUL and
 * may be NULL. The text stays valid until the file is closed. */
XISFCONV_API xisfconv_status xisfconv_header_text(xisfconv_file *file, const char **text, size_t *length);

typedef struct xisfconv_image_info {
    size_t struct_size;

    uint64_t width;
    uint64_t height;
    uint64_t channels;

    /* Sample format of the buffer xisfconv_read_pixels returns by default, and the representable
     * range of floating point samples. XISF: from the header (the bounds attribute). FITS and
     * ASDF: both depend on the data (signed integers without negative values become unsigned,
     * with negative values floating point; the range is 0:1 when the data fits, else 0:65535
     * when it fits, else minimum and maximum), so they are known once the pixels have been
     * loaded: data_known is 0 before that. xisfconv_load_pixels loads them. */
    xisfconv_sample_format sample_format;
    int32_t data_known;
    double lower_bound;
    double upper_bound;

    xisfconv_color_space color_space;
    xisfconv_row_order row_order;      /* of the rows as stored in the file */
    int32_t row_order_declared;        /* 0: assumed (FITS or ASDF without ROWORDER) */

    int32_t convertible;               /* 0: the pixels cannot be read; see xisfconv_image_unsupported_reason */
    int32_t has_icc_profile;
    int32_t has_display_function;      /* XISF: a DisplayFunction (saved STF) element is present ... */
    int32_t has_stored_stretch;        /* ... and it is not the identity */
    int32_t has_astrometric_solution;  /* PixInsight solution properties or WCS keywords */

    /* Colour filter array (XISF ColorFilterArray element). */
    int32_t has_cfa;
    int32_t cfa_width;
    int32_t cfa_height;
    char cfa_pattern[68];              /* "RGGB", NUL-terminated, in the row order stored in the file;
                                          cut off if longer (the "cfaPattern" detail has all of it) */

    /* Resolution in pixels per unit. resolution_unit: 0 = none, 1 = inch, 2 = cm. */
    int32_t resolution_unit;
    double resolution_x;
    double resolution_y;

    /* FITS and ASDF input (0, 1.0, 0.0 for XISF). */
    int32_t bitpix;
    int32_t plain_array;               /* ASDF: an array that is not an HDU of a FITS-tagged node */
    double bscale;
    double bzero;
    uint64_t source_index;             /* FITS: number of the HDU; ASDF: running number of the array */

    /* The row order that the WCS keywords of the image describe. FITS and ASDF: the order the
     * rows are stored in. XISF: always bottom-up, although the rows are stored top-down, because
     * that is how PixInsight writes and reads WCS keywords. (BAYERPAT always describes the rows
     * as stored.) */
    xisfconv_row_order wcs_row_order;
} xisfconv_image_info;

XISFCONV_API void xisfconv_image_info_init(xisfconv_image_info *info, size_t struct_size);
XISFCONV_API xisfconv_status xisfconv_image_info_get(const xisfconv_file *file, size_t image, xisfconv_image_info *info);

/* XISF: the image id. FITS and ASDF HDUs: EXTNAME or HDUNAME. Plain ASDF arrays: their place in
 * the tree. "" if the image has no name. */
XISFCONV_API const char *xisfconv_image_name(const xisfconv_file *file, size_t image);
/* Why the pixels cannot be read; "" if they can. */
XISFCONV_API const char *xisfconv_image_unsupported_reason(const xisfconv_file *file, size_t image);

/* Details of an image as text, by name; "" if the image has no such detail.
 *   XISF  "sampleFormat", "colorSpace", "pixelStorage" (Planar, Normal), "byteOrder" (little, big),
 *         "location", "compression", "subblocks", "checksum" (the attributes as written),
 *         "imageType", "orientation", "cfaPattern", "cfaName", "resolutionUnit"
 *   FITS  "tileCompression" (RICE_1, GZIP_1, ...), "mapping" (how the samples were mapped; known
 *         once the pixels are loaded)
 *   ASDF  "source" (place in the tree: fits[0].data), "storage" (datatype, byte order, block,
 *         compression), "mapping" */
XISFCONV_API const char *xisfconv_image_detail(const xisfconv_file *file, size_t image, const char *name);

/* The cards the file itself holds: XISF FITSKeyword elements, or every non-structural card of
 * a FITS HDU. Owned by the file. */
XISFCONV_API xisfconv_status xisfconv_image_keywords(const xisfconv_file *file, size_t image,
                                                     const xisfconv_keywords **out);

/* --- XISF properties --------------------------------------------------------------------- */

/* image = XISFCONV_FILE_PROPERTIES addresses the file-level metadata instead of an image.
 *
 * FITS and ASDF files have no properties of their own. One that was converted from XISF
 * (since 0.13) carries those of the XISF file, and they are read with the same functions:
 * a FITS file in a binary table behind each image (EXTNAME XISF_PROPERTIES, and XISF_METADATA
 * for the properties of the file; a row per property with the columns ID, TYPE, BLOCK, ROWS,
 * COLUMNS, VALUE, COMMENT and FORMAT, the value as UTF-8 text or as the little-endian elements
 * of a vector or matrix), an ASDF file under the key "xisf" of its tree (images[n].properties and
 * metadata: {id: {type, value, comment, format}}, vectors and matrices as arrays). For other
 * FITS and ASDF files the count is 0.
 *
 * The properties of a file are held in memory together when it is converted, and those a FITS
 * or ASDF file carries from the moment it is opened: more than the size of the file plus
 * 256 MiB is not accepted, and what is beyond is left out with a warning. */
#define XISFCONV_FILE_PROPERTIES ((size_t)-1)

XISFCONV_API size_t xisfconv_property_count(const xisfconv_file *file, size_t image);
/* type is the XISF type name ("Float64", "String", "F64Matrix", "TimePoint"). value is the
 * scalar value or string text. *in_data_block is 1 for vector and matrix properties, whose
 * value is "" here and is read with xisfconv_property_read_f64. Any out pointer may be NULL. */
XISFCONV_API xisfconv_status xisfconv_property_get(const xisfconv_file *file, size_t image, size_t index,
                                                   const char **id, const char **type, const char **value,
                                                   const char **comment, int32_t *in_data_block);
/* Index of the property with this id, or -1. */
XISFCONV_API int64_t xisfconv_property_find(const xisfconv_file *file, size_t image, const char *id);
/* The format attribute of a property (how its value is meant to be shown, e.g. "%.3f"); "" if
 * it has none or there is no such property. Owned by the file. (Since 0.13.) */
XISFCONV_API const char *xisfconv_property_format(const xisfconv_file *file, size_t image, size_t index);

/* Reads a numeric vector or matrix property as doubles, row-major. With an image index, the
 * image's properties are searched first, then the file-level metadata. Call with values = NULL
 * to learn the size: *rows and *columns are set (a vector has rows = 1). capacity is the number
 * of doubles `values` can hold; XISFCONV_ERR_BUFFER if it is too small, XISFCONV_ERR_NOT_FOUND
 * if there is no vector or matrix property of that id, XISFCONV_ERR_UNSUPPORTED if its elements
 * are of a type that is not read as numbers (complex ones). */
XISFCONV_API xisfconv_status xisfconv_property_read_f64(xisfconv_file *file, size_t image, const char *id,
                                                        double *values, size_t capacity, size_t *rows,
                                                        size_t *columns);

/* --- Pixels ------------------------------------------------------------------------------- */

typedef struct xisfconv_read_options {
    size_t struct_size;
    xisfconv_sample_format sample_format; /* XISFCONV_SAMPLE_AS_STORED (default) or the format wanted */
    xisfconv_row_order row_order;         /* XISFCONV_ROWS_DEFAULT = as stored in the file */
    int32_t verify_checksums;             /* default 1 */
    /* Range of floating point data, used when sample_format asks for a conversion to integers.
     * use_bounds = 0 (default): the range xisfconv_image_info reports. */
    int32_t use_bounds;
    double lower_bound;
    double upper_bound;
} xisfconv_read_options;

XISFCONV_API void xisfconv_read_options_init(xisfconv_read_options *options, size_t struct_size);

/* FITS and ASDF: reads the pixels of an image and keeps them in the file handle, so that
 * xisfconv_image_info_get reports the final sample format and bounds. The next
 * xisfconv_read_pixels of that image takes them from there and releases them; loading another
 * image, or xisfconv_close, releases them too. XISF: does nothing. */
XISFCONV_API xisfconv_status xisfconv_load_pixels(xisfconv_file *file, size_t image, int32_t verify_checksums);

/* Size in bytes of the buffer xisfconv_read_pixels needs. options may be NULL for the defaults.
 * For FITS and ASDF images whose sample format is not yet known this loads the pixels. */
XISFCONV_API xisfconv_status xisfconv_pixels_size(xisfconv_file *file, size_t image,
                                                  const xisfconv_read_options *options, uint64_t *size);

/* Reads the pixels into `buffer` (planar, host byte order). XISFCONV_ERR_BUFFER if buffer_size
 * is too small. Sample conversion follows the command line tool's --bits rules:
 *   integer -> integer  rescaled over the full ranges
 *   integer -> float    normalized to [0,1]
 *   float -> integer    the bounds mapped to the full integer range, clipped
 *   float -> float      values unchanged
 * The keywords of the image stay as they are in the file whatever row_order is asked for:
 * BAYERPAT describes the rows as stored, WCS keywords the row order xisfconv_image_info names in
 * wcs_row_order. xisfconv_wcs_keywords gives the WCS keywords for any row order. */
XISFCONV_API xisfconv_status xisfconv_read_pixels(xisfconv_file *file, size_t image,
                                                  const xisfconv_read_options *options, void *buffer,
                                                  uint64_t buffer_size);

/* ICC profile of an image (XISF). Call with buffer = NULL to learn the size.
 * XISFCONV_ERR_NOT_FOUND if the image has none. */
XISFCONV_API xisfconv_status xisfconv_read_icc_profile(xisfconv_file *file, size_t image, void *buffer,
                                                       size_t buffer_size, size_t *size);

/* --- Stretch ------------------------------------------------------------------------------ */

/* One histogram transformation in PixInsight's STF form, on values normalized to [0,1]:
 *   x1 = clip((x - shadows) / (highlights - shadows))
 *   x2 = MTF(midtones, x1)
 *   y  = clip((x2 - low) / (high - low)) */
typedef struct xisfconv_stretch_params {
    double shadows;
    double midtones;
    double highlights;
    double low;
    double high;
} xisfconv_stretch_params;

/* The STF saved in an XISF image (its DisplayFunction), one set per colour channel: 1 for a
 * grayscale image, 3 otherwise. *count receives that number; params may be NULL to learn it.
 * XISFCONV_ERR_NOT_FOUND if the image has none; the identity is returned like any other. */
XISFCONV_API xisfconv_status xisfconv_stored_stretch(const xisfconv_file *file, size_t image,
                                                     xisfconv_stretch_params *params, size_t capacity,
                                                     size_t *count);

/* PixInsight-style auto-STF (shadows at median - 2.8 * MADN, background to 0.25) of a buffer.
 * Fills params[0 .. color_channels-1]. linked != 0 shares the averaged statistics between the
 * channels, which keeps the colour balance. lower_bound and upper_bound are the range of
 * floating point samples (0 and 1 normally); integers use their full range. */
XISFCONV_API xisfconv_status xisfconv_auto_stretch(xisfconv_context *ctx, const void *pixels, uint64_t width,
                                                   uint64_t height, uint64_t channels,
                                                   xisfconv_sample_format sample_format, double lower_bound,
                                                   double upper_bound, size_t color_channels, int32_t linked,
                                                   xisfconv_stretch_params *params);

/* Applies the stretch to the first param_count channels; the remaining (alpha) channels are
 * only normalized. `out` receives Float32 samples in [0,1], planar, and must hold
 * width * height * channels floats. */
XISFCONV_API xisfconv_status xisfconv_apply_stretch(xisfconv_context *ctx, const void *pixels, uint64_t width,
                                                    uint64_t height, uint64_t channels,
                                                    xisfconv_sample_format sample_format, double lower_bound,
                                                    double upper_bound, const xisfconv_stretch_params *params,
                                                    size_t param_count, float *out);

/* --- Astrometry --------------------------------------------------------------------------- */

/* WCS keywords (CTYPE, CRVAL, CRPIX, CD, RADESYS, SIP terms) for an image, for pixel rows in
 * `row_order` (XISFCONV_ROWS_DEFAULT = bottom-up, as FITS expects).
 *   An image that carries WCS keywords: those keywords, converted to `row_order`.
 *   XISF with a PixInsight solution and no WCS keywords: built from the
 *     PCL:AstrometricSolution properties; the spline distortion is fitted with SIP polynomials of
 *     order sip_order (2..7, 0 = linear only).
 * fit_summary, if not NULL, receives one line about the fit quality ("" if nothing was fitted),
 * valid until the list is freed. XISFCONV_ERR_NOT_FOUND if the image has no usable solution.
 * Caller frees *out. */
XISFCONV_API xisfconv_status xisfconv_wcs_keywords(xisfconv_file *file, size_t image, xisfconv_row_order row_order,
                                                   int32_t sip_order, xisfconv_keywords **out,
                                                   const char **fit_summary);

/* The cards an image has as a FITS header, for pixel rows in `row_order` (XISFCONV_ROWS_DEFAULT =
 * bottom-up): what xisfconv_convert writes to a FITS or ASDF file, without its HISTORY lines.
 *   XISF: the FITS keywords of the image; where the file has none, keywords derived from its
 *     properties and attributes if property_keywords is not 0 (OBJECT, EXPTIME, TELESCOP, INSTRUME,
 *     FILTER, CCD-TEMP, XPIXSZ, YPIXSZ, FOCALLEN, APTDIA, DATE-OBS, BAYERPAT, IMAGETYP); BAYERPAT
 *     and WCS keywords for `row_order`; and, if wcs is not 0 and the image has no WCS keywords,
 *     those built from a PixInsight solution as by xisfconv_wcs_keywords (sip_order: 2..7,
 *     0 = linear only).
 *   FITS and ASDF: the cards of the image, with BAYERPAT and WCS keywords converted if `row_order`
 *     is not the order the rows are stored in.
 * fit_summary as for xisfconv_wcs_keywords; it may be NULL. Caller frees *out. */
XISFCONV_API xisfconv_status xisfconv_fits_keywords(xisfconv_file *file, size_t image, xisfconv_row_order row_order,
                                                    int32_t property_keywords, int32_t wcs, int32_t sip_order,
                                                    xisfconv_keywords **out, const char **fit_summary);

/* Converts WCS keywords in place between the bottom-up and top-down pixel conventions
 * (CRPIX2, CD/PC/CDELT, SIP coefficients). Applying it twice restores the original values. */
XISFCONV_API xisfconv_status xisfconv_wcs_flip_rows(xisfconv_keywords *kw, uint64_t image_height);

/* ------------------------------------------------------------------------------------------
 * Converting files
 *
 * The whole of the command line tool's conversion in one call: XISF to FITS, ASDF, TIFF or PNG;
 * FITS and ASDF to XISF, to each other, or to TIFF or PNG; FITS to FITS to write tile-compressed
 * images as plain ones, or plain images tile-compressed. XISF to XISF is xisfconv_rewrite.
 * ---------------------------------------------------------------------------------------- */

typedef struct xisfconv_convert_options {
    size_t struct_size;

    xisfconv_format output_format;        /* XISFCONV_FORMAT_AUTO: from the extension of the output path */
    xisfconv_sample_format sample_format; /* --bits; default XISFCONV_SAMPLE_AS_STORED */
    size_t image;                         /* default XISFCONV_ALL_IMAGES; else only this one */
    xisfconv_stretch stretch;             /* --stretch; default XISFCONV_STRETCH_NONE */

    /* -c / --codec. Default XISFCONV_CODEC_NONE. XISF: ZLIB or ZSTD, with byte shuffling. ASDF:
     * ZLIB or ZSTD. TIFF: any value but NONE means Deflate with predictor. DEFAULT picks the
     * usual codec of the output format.
     * FITS: the images are written tile-compressed (the tiled image compression convention of
     * the FITS standard, the format of fpack), one row per tile and without loss: DEFAULT uses
     * RICE_1 for integers and GZIP_2 for floating point, ZLIB uses gzip for both (GZIP_2, and
     * GZIP_1 for 8-bit samples); ZSTD is XISFCONV_ERR_ARGUMENT (XISFCONV_ERR_UNSUPPORTED in a
     * build without libzstd). Images of 64-bit integers stay uncompressed (a warning says so):
     * CFITSIO reads no such compressed images. An output path that ends in ".fz"
     * (image.fits.fz) is written with DEFAULT also when the codec is NONE. Keywords that
     * describe a compressed image and its table (TFORMn, ZCMPTYPE, ZSCALE, ...) are left out of
     * such a file, with a warning. (Up to 0.11 the codec had no effect on FITS output.) */
    xisfconv_codec codec;
    xisfconv_checksum checksum;           /* XISF output; default XISFCONV_CHECKSUM_NONE */
    uint64_t subblock_size;               /* XISF output; default 1 GiB */

    /* XISF input: row order written to FITS or ASDF (DEFAULT = bottom-up).
     * FITS or ASDF input: row order the file is stored in (DEFAULT = what ROWORDER says,
     * else bottom-up). */
    xisfconv_row_order row_order;

    int32_t property_keywords;            /* from XISF: derive missing keywords; default 1 */
    int32_t wcs;                          /* translate the astrometric solution; default 1 */
    int32_t sip_order;                    /* from XISF: 2..7, 0 = linear only; default 3 */
    int32_t verify_checksums;             /* default 1 */

    /* --bounds, for floating point FITS or ASDF input. use_bounds = 0 (default): automatic. */
    int32_t use_bounds;
    int32_t overwrite;                    /* --force; default 0: XISFCONV_ERR_EXISTS if the output exists */
    double lower_bound;
    double upper_bound;

    /* --no-properties sets it to 0; default 1. (Since 0.13.)
     * From XISF to FITS and ASDF: the XISF properties of the images and of the file are written
     * along, with their types and exact values (see "XISF properties" above for where).
     * From FITS and ASDF: the properties a file carries are used. To XISF they are the
     * properties of the images again; an astrometric solution among them is written only if
     * the WCS keywords, the size of the image and the order of its rows are what they were when
     * it was carried, and is made from the WCS keywords otherwise (with wcs = 1). From FITS to
     * ASDF and back, and from FITS to FITS, they are written along as they are. */
    int32_t properties;
    int32_t reserved;                     /* not used (padding in the layout of 0.13) */

    /* A smaller picture, for TIFF and PNG output (XISFCONV_ERR_ARGUMENT for the other formats).
     * (Since 0.14.) Every pixel of the picture is the mean of the pixels of the image it
     * covers, taken of the image as it is stored: before a stretch, and before sample_format.
     * A picture is never larger than the image; if nothing here asks for a smaller one, the
     * image is written as it is.
     *   fit_width, fit_height
     *               --resize: the picture is to fit that many pixels, its proportions kept;
     *               0 (default) sets no limit for that side.
     *   scale       --resize n%: the picture is that fraction of the image in width and height,
     *               above 0 and up to 1; 0 (default): not asked for.
     *   bin         --bin: n x n pixels become one (default 1). What is left over at the right
     *               and at the bottom of the image is dropped (an image smaller than one block
     *               is one block).
     * With bin and one of the others, the blocks come first: fit and scale are of the binned
     * image. With fit and scale, the picture is the smaller of the two. */
    uint64_t fit_width;
    uint64_t fit_height;
    double scale;
    int32_t bin;
    int32_t reserved2;                    /* not used */
} xisfconv_convert_options;

XISFCONV_API void xisfconv_convert_options_init(xisfconv_convert_options *options, size_t struct_size);

/* Writes to "<output>.part" and renames when complete, so a failed call leaves no half-written
 * file under the final name. options may be NULL for the defaults. Refuses an output that is
 * the input file. */
XISFCONV_API xisfconv_status xisfconv_convert(xisfconv_context *ctx, const char *input, const char *output,
                                              const xisfconv_convert_options *options);

/* ------------------------------------------------------------------------------------------
 * Rewriting XISF files (another compression, checksums, one image of several)
 * ---------------------------------------------------------------------------------------- */

typedef struct xisfconv_rewrite_options {
    size_t struct_size;
    xisfconv_codec codec;       /* default XISFCONV_CODEC_KEEP; NONE, ZLIB, ZSTD or DEFAULT */
    xisfconv_checksum checksum; /* default XISFCONV_CHECKSUM_KEEP; NONE removes them */
    size_t image;               /* default XISFCONV_ALL_IMAGES; else keep only this one */
    int32_t verify_input;       /* default 1 */
    int32_t read_back;          /* default 1: read the output back and compare every block */
    uint64_t subblock_size;     /* default 1 GiB */
    int32_t overwrite;          /* default 0. In place it only decides whether a leftover
                                   "<path>.part" file from an interrupted run may be overwritten */
} xisfconv_rewrite_options;

typedef struct xisfconv_rewrite_result {
    size_t struct_size;
    uint64_t input_size;
    uint64_t output_size;
    uint64_t blocks;            /* attached data blocks written */
    uint64_t compressed;        /* blocks compressed with the requested codec */
    uint64_t decompressed;      /* blocks now stored uncompressed */
    uint64_t kept;              /* blocks copied as they were stored */
    uint64_t checksums;         /* checksums computed for the output */
    uint64_t checksums_removed;
    int32_t read_back;          /* the output was read back and matched */
    int32_t changed;            /* 0: the input already stored everything as requested */
} xisfconv_rewrite_result;

XISFCONV_API void xisfconv_rewrite_options_init(xisfconv_rewrite_options *options, size_t struct_size);
XISFCONV_API void xisfconv_rewrite_result_init(xisfconv_rewrite_result *result, size_t struct_size);

/* result may be NULL. */
XISFCONV_API xisfconv_status xisfconv_rewrite(xisfconv_context *ctx, const char *input, const char *output,
                                              const xisfconv_rewrite_options *options,
                                              xisfconv_rewrite_result *result);

/* Replaces the file: written next to it, read back and compared (always), flushed, then
 * renamed over the original. A file that is already stored as requested is left alone
 * (result->changed = 0). A read-only file is refused; a symbolic link is followed. */
XISFCONV_API xisfconv_status xisfconv_rewrite_in_place(xisfconv_context *ctx, const char *path,
                                                       const xisfconv_rewrite_options *options,
                                                       xisfconv_rewrite_result *result);

/* *out = 1 if every attached block is already stored the way the options ask, judged by the
 * header alone. */
XISFCONV_API xisfconv_status xisfconv_stored_as_requested(xisfconv_context *ctx, const char *path,
                                                          const xisfconv_rewrite_options *options, int32_t *out);

/* ------------------------------------------------------------------------------------------
 * Verifying files
 * ---------------------------------------------------------------------------------------- */

typedef struct xisfconv_report xisfconv_report;

typedef int32_t xisfconv_verdict;
enum {
    XISFCONV_VERDICT_OK                = 0,
    XISFCONV_VERDICT_NOT_FULLY_CHECKED = 1, /* intact as far as could be told; some part not checked */
    XISFCONV_VERDICT_FAILED            = 2
};

/* Reads the file completely without converting anything. A damaged or unreadable file is not
 * an error of this function: it returns XISFCONV_OK and a report whose verdict is
 * XISFCONV_VERDICT_FAILED. A status other than XISFCONV_OK means no report could be made (bad
 * argument, cancelled). Caller frees. */
XISFCONV_API xisfconv_status xisfconv_verify(xisfconv_context *ctx, const char *path, xisfconv_report **out);
XISFCONV_API void xisfconv_report_free(xisfconv_report *report); /* NULL is allowed */

XISFCONV_API xisfconv_verdict xisfconv_report_verdict(const xisfconv_report *report);
XISFCONV_API xisfconv_format xisfconv_report_format(const xisfconv_report *report);
XISFCONV_API const char *xisfconv_report_summary(const xisfconv_report *report);     /* "3 data blocks" */
XISFCONV_API size_t xisfconv_report_verified(const xisfconv_report *report);         /* checksums present and matching */
XISFCONV_API size_t xisfconv_report_unchecked(const xisfconv_report *report);        /* blocks or HDUs without a checksum */
XISFCONV_API size_t xisfconv_report_problem_count(const xisfconv_report *report);
XISFCONV_API const char *xisfconv_report_problem(const xisfconv_report *report, size_t index);
XISFCONV_API size_t xisfconv_report_not_checked_count(const xisfconv_report *report);
XISFCONV_API const char *xisfconv_report_not_checked(const xisfconv_report *report, size_t index);

/* ------------------------------------------------------------------------------------------
 * Writing images from memory
 *
 * Saves arrays as XISF, FITS, ASDF, TIFF or PNG. An image is treated exactly like one read from
 * a FITS file and converted: 3 channels are written as RGB, 1 as grayscale, any other number as
 * a stack of planes (XISF: channels; FITS and ASDF: a cube; TIFF: one page per plane; PNG: the
 * first plane).
 * ---------------------------------------------------------------------------------------- */

typedef struct xisfconv_image {
    size_t struct_size;
    const void *pixels;                   /* planar, host byte order; copied by xisfconv_writer_add_image */
    uint64_t width;
    uint64_t height;
    uint64_t channels;
    xisfconv_sample_format sample_format; /* XISFCONV_SAMPLE_AS_STORED is not allowed here */
    xisfconv_row_order row_order;         /* of the buffer; XISFCONV_ROWS_DEFAULT = top-down */
    /* Range of floating point samples. use_bounds = 0 (default): 0:1 when the data fits, else
     * 0:65535 when it fits, else minimum and maximum. It becomes the XISF bounds, and black and
     * white in TIFF and PNG. */
    int32_t use_bounds;
    double lower_bound;
    double upper_bound;
    const char *name;                     /* XISF id, FITS EXTNAME, TIFF description; may be NULL */
    /* May be NULL; not written to TIFF and PNG. The cards describe the buffer as it is given:
     * BAYERPAT counts rows from its first row, and so do WCS keywords unless wcs_row_order says
     * otherwise. They are converted when the rows are stored in the other order. A 2x2 BAYERPAT
     * of R, G and B also becomes the XISF ColorFilterArray. Cards that describe how a FITS file
     * stores its data are left out, so that a header taken from a FITS file can be passed as it
     * is: SIMPLE, BITPIX, NAXIS, NAXISn, EXTEND, XTENSION, PCOUNT, GCOUNT, BZERO, BSCALE, BLANK,
     * ROWORDER, CHECKSUM, DATASUM and END. */
    const xisfconv_keywords *keywords;
    const void *icc_profile;              /* may be NULL; written to XISF, TIFF and PNG */
    size_t icc_profile_size;
    /* The row order the WCS keywords describe, if it is not that of the buffer. DEFAULT = that of
     * the buffer. Pixels and keywords read from a file go back out unchanged when this is set to
     * the wcs_row_order of xisfconv_image_info (which matters for XISF files). */
    xisfconv_row_order wcs_row_order;
} xisfconv_image;

XISFCONV_API void xisfconv_image_init(xisfconv_image *image, size_t struct_size);

typedef struct xisfconv_write_options {
    size_t struct_size;
    xisfconv_format format;       /* XISFCONV_FORMAT_AUTO: from the extension */
    xisfconv_codec codec;         /* as in xisfconv_convert_options */
    xisfconv_checksum checksum;   /* XISF */
    /* FITS and ASDF: row order to store. DEFAULT = bottom-up, the FITS convention; the rows of a
     * top-down buffer are reversed, with its WCS keywords and BAYERPAT. XISF, TIFF and PNG are
     * always stored top-down. */
    xisfconv_row_order row_order;
    uint64_t subblock_size;       /* XISF; default 1 GiB */
    int32_t wcs;                  /* to XISF: also write PixInsight solution properties from WCS
                                     keywords; default 1 */
    int32_t overwrite;            /* default 0 */
} xisfconv_write_options;

XISFCONV_API void xisfconv_write_options_init(xisfconv_write_options *options, size_t struct_size);

typedef struct xisfconv_writer xisfconv_writer;

/* Collects images, then writes them in one go: XISF images, FITS HDUs, ASDF HDU list, TIFF
 * pages. PNG holds one image: of several, the first is written and a warning says so. Nothing is
 * written before xisfconv_writer_finish. options may be NULL for the defaults. */
XISFCONV_API xisfconv_status xisfconv_writer_new(xisfconv_context *ctx, const char *path,
                                                 const xisfconv_write_options *options, xisfconv_writer **out);
XISFCONV_API xisfconv_status xisfconv_writer_add_image(xisfconv_writer *writer, const xisfconv_image *image);
/* Writes "<path>.part", renames it, and frees the writer whether or not it succeeds. */
XISFCONV_API xisfconv_status xisfconv_writer_finish(xisfconv_writer *writer);
/* Drops a writer without writing anything. NULL is allowed. */
XISFCONV_API void xisfconv_writer_discard(xisfconv_writer *writer);

/* ------------------------------------------------------------------------------------------
 * Diagnostics
 *
 * For the command line tool's --dump-header and for the test suite. Not part of the API that is
 * kept stable.
 * ---------------------------------------------------------------------------------------- */

/* The YAML tree of an ASDF file as it is stored, byte for byte and without parsing it, so that
 * it can be looked at when the file does not open. Call with buffer = NULL to learn the size
 * (without a terminating NUL, and none is written). XISFCONV_ERR_UNSUPPORTED if the file is not
 * ASDF. */
XISFCONV_API xisfconv_status xisfconv_asdf_tree_text(xisfconv_context *ctx, const char *path, char *buffer,
                                                     size_t buffer_size, size_t *size);

/* The YAML tree of an ASDF file, parsed and written as JSON, with plain scalars typed the way
 * PyYAML types them. Works on any tree, also one that holds no images. Call with buffer = NULL
 * to learn the size (without a terminating NUL, and none is written).
 * XISFCONV_ERR_UNSUPPORTED if the file is not ASDF. */
XISFCONV_API xisfconv_status xisfconv_asdf_tree_json(xisfconv_context *ctx, const char *path, char *buffer,
                                                     size_t buffer_size, size_t *size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XISFCONV_H */
