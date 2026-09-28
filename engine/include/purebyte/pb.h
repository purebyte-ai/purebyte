/*
 * purebyte/pb.h -- C API of libpurebyte, the PureByte runtime for byte-level models.
 *
 * A model is a GGUF file (spec/FORMAT.md) that declares everything the runtime needs: its blocks, its heads, the
 * window it was trained on and, optionally, the post-processing profile that turns its raw outputs into results.
 * This header is the only stable interface of the library; every binding (the `purebyte` CLI, Python) sits on it.
 *
 * Conventions
 *   - Every fallible function returns a pb_status (PB_OK == 0) and never aborts or exits the process. Functions that
 *     take a `pb_error *` fill it with a human-readable message on failure; passing NULL is allowed.
 *   - No global state. A pb_model is immutable once loaded and may be used by any number of threads at once. A
 *     pb_session owns worker threads and scratch memory: use one session per thread, or serialise calls on it.
 *   - Offsets and lengths are in bytes; spans are half-open [start, end).
 *   - Memory returned by the library (strings, results) is released with the matching *_free function.
 *   - ABI: PB_ABI_VERSION changes on any incompatible change. Option structs start with `struct_size` and only ever
 *     grow by appending fields. Always initialise them with the matching *_init function: those are static inline
 *     functions of this header, so they write the size of the struct YOUR program was compiled with. A library newer
 *     than your header accepts the smaller struct (the fields it does not have keep their defaults); a library older
 *     than your header refuses a larger struct with PB_ERR_UNSUPPORTED instead of ignoring fields it cannot honour.
 */
#ifndef PUREBYTE_PB_H
#define PUREBYTE_PB_H

#include <stddef.h>
#include <stdint.h>
#include <string.h> /* memset, in the inline *_init functions */

#if defined(_WIN32) && defined(PUREBYTE_SHARED)
#if defined(PUREBYTE_BUILDING)
#define PB_API __declspec(dllexport)
#else
#define PB_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(PUREBYTE_SHARED)
#define PB_API __attribute__((visibility("default")))
#else
#define PB_API
#endif

/* The *_init functions are compiled into the caller (see "ABI" above). */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define PB_INLINE static inline
#elif defined(_MSC_VER)
#define PB_INLINE static __inline
#else
#define PB_INLINE static
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ versions, errors */

#define PB_ABI_VERSION 1

PB_API uint32_t pb_abi_version(void);  /* PB_ABI_VERSION of the loaded library */
PB_API const char *pb_version(void);   /* library version, "MAJOR.MINOR.PATCH" */
PB_API uint32_t pb_format_version(void); /* newest model format version (spec/FORMAT.md) this library reads */

typedef enum pb_status {
    PB_OK = 0,
    PB_ERR_ARGUMENT = 1,         /* invalid argument: null pointer, value out of range, unknown option */
    PB_ERR_IO = 2,               /* a file could not be opened, read or written */
    PB_ERR_FORMAT = 3,           /* not a valid PureByte GGUF: corrupt, truncated or self-contradictory */
    PB_ERR_UNSUPPORTED = 4,      /* well-formed, but needs something this library does not have (newer format, an
                                    unknown block or head type, a kernel this CPU lacks, a head the model lacks) */
    PB_ERR_NO_MEMORY = 5,
    PB_ERR_BUFFER_TOO_SMALL = 6, /* the caller's buffer is too small; the required size is reported */
    PB_ERR_INTERNAL = 7          /* a bug: please report it */
} pb_status;

typedef struct pb_error {
    int32_t status;     /* a pb_status */
    char message[508];  /* NUL-terminated, English */
} pb_error;

PB_API const char *pb_status_name(pb_status status); /* "ok", "argument", "io", ... */

/* Frees a string or buffer returned by the library. */
PB_API void pb_free(void *memory);

/* ------------------------------------------------------------------------------------------------------- models */

typedef struct pb_model pb_model;

/* Loads and validates a model. `path` is UTF-8 on every platform. */
PB_API pb_status pb_model_load(const char *path, pb_model **out, pb_error *err);
/* Same, from a buffer the library copies. */
PB_API pb_status pb_model_load_memory(const void *data, size_t size, pb_model **out, pb_error *err);
PB_API void pb_model_free(pb_model *model);

/* JSON description of the model (hyper-parameters, blocks, heads with their names and labels, window, profile).
 * The string is owned by the model and lives as long as it. */
PB_API const char *pb_model_describe(const pb_model *model);

PB_API int32_t pb_model_d_model(const pb_model *model);
PB_API int32_t pb_model_head_count(const pb_model *model);
/* Index of the first head of `type` ("choice", "multilabel", "score", "ordinal", "tag", "byte_map", "exit"), or -1. */
PB_API int32_t pb_model_find_head(const pb_model *model, const char *type);

/* ------------------------------------------------------------------------------------------------------ sessions */

typedef struct pb_session pb_session;

typedef struct pb_session_options {
    uint32_t struct_size;  /* sizeof(pb_session_options) */
    int32_t threads;       /* worker threads, >= 1 (default 1) */
    int32_t intra_threads; /* threads that cooperate inside ONE window: 0 = automatic (the idle threads of a batch
                              with fewer windows than threads join a window), 1 = never split a window */
    const char *kernel;    /* "auto" (default), "scalar", "avx2", "neon" */
} pb_session_options;

PB_INLINE void pb_session_options_init(pb_session_options *options) {
    if (!options) return;
    memset(options, 0, sizeof *options);
    options->struct_size = (uint32_t)sizeof *options;
    options->threads = 1;
    options->intra_threads = 0;
    options->kernel = "auto";
}
PB_API pb_status pb_session_create(const pb_session_options *options, pb_session **out, pb_error *err);
PB_API void pb_session_free(pb_session *session);
PB_API const char *pb_session_kernel(const pb_session *session); /* the kernel actually in use */
PB_API int32_t pb_session_threads(const pb_session *session);

/* ---------------------------------------------------------------------------------------------------- inference */

typedef struct pb_input {
    const uint8_t *data;
    size_t size;
} pb_input;

typedef struct pb_span {
    int64_t start;    /* absolute offset in the input */
    int64_t end;      /* exclusive */
    int32_t type;     /* entity index of the tag head; the name is in pb_model_describe */
    float confidence; /* mean probability of the labels the decoder chose over the span's bytes */
} pb_span;

/* Flags of pb_scan_options.flags */
#define PB_SCAN_PREFILTER 0x1u  /* skip windows no region of the prefilter rule touches (not exact: prefilter_rule) */
#define PB_SCAN_EARLY_EXIT 0x2u /* stop windows the model's `exit` head rejects (not exact by construction) */
#define PB_SCAN_DIGEST 0x4u     /* fill pb_window.digest (reproducibility checks) */
#define PB_SCAN_UNGATED 0x8u    /* evaluate gated heads even when their gate says negative */
#define PB_SCAN_WHOLE 0x10u     /* one window per input, whatever its length (window/stride are ignored) */

typedef struct pb_scan_options {
    uint32_t struct_size;       /* sizeof(pb_scan_options) */
    uint32_t window;            /* bytes per window; 0 = the model's (purebyte.window.size, else 512) */
    uint32_t stride;            /* advance between windows; 0 = the model's when `window` is 0 too, else
                                   window - window / 4 */
    uint32_t flags;             /* PB_SCAN_* */
    int32_t use_bias;           /* 1 = `bias` replaces the operating bias the model carries */
    float bias;                 /* operating bias added to every non-O label of the tag head */
    const float *type_bias;     /* optional: one bias per entity type (overrides `bias` and the model's) */
    uint32_t type_bias_count;   /* number of floats in type_bias; must equal the tag head's entity count */
    const char *prefilter_rule; /* PB_SCAN_PREFILTER: the regions that send a window to the model; NULL = "A16+U16".
                                   Components joined by '+' (numbers 1..4096), measured in the whole input:
                                     A<L>      a run of >= L printable ASCII bytes (0x20..0x7e, tab)
                                     U<L>      a run of >= L UTF-16LE characters (printable byte, 0x00), either parity
                                     P<p>x<K>  >= K consecutive i with b[i] == b[i+q], for a period q in 2..p, not
                                               one repeated byte
                                     D<k>x<m>  a slice of k bytes holding >= m printable ones (m <= k)
                                   A window that none touches is skipped: not exact for a model that also fires
                                   elsewhere (compressed or tabular bytes). An invalid rule is PB_ERR_ARGUMENT. */
    const char *const *queries; /* query-conditioned models: the fields to extract (UTF-8) */
    uint32_t query_count;
} pb_scan_options;

PB_INLINE void pb_scan_options_init(pb_scan_options *options) {
    if (!options) return;
    memset(options, 0, sizeof *options);
    options->struct_size = (uint32_t)sizeof *options;
}

/* pb_window.flags */
#define PB_WINDOW_SKIPPED 0x1u /* the prefilter skipped it: nothing was computed */
#define PB_WINDOW_EXITED 0x2u  /* the exit head stopped it: heads were not computed, it counts as negative */
#define PB_WINDOW_GATED 0x4u   /* a gate said negative: the gated heads were not computed */

typedef struct pb_window {
    int64_t start;       /* offset of the window in its input */
    int32_t length;
    int32_t label;       /* argmax of the first choice head; 0 when there is none or it was not computed */
    float p_positive;    /* 1 - P(class 0) of the first choice head; 0 when not computed */
    uint32_t flags;      /* PB_WINDOW_* */
    uint64_t digest;     /* with PB_SCAN_DIGEST: FNV-1a of the final hidden states and the head values */
    uint32_t span_first; /* the window's spans are spans[span_first .. span_first + span_count) of its input */
    uint32_t span_count;
} pb_window;

typedef struct pb_scan_result pb_scan_result;

/* Runs the model over windows of every input (in parallel on the session's threads) and keeps, per window, the
 * outputs of every head. Windows shorter than the model's `purebyte.window.min` (24 bytes unless the model declares
 * another; the released pii declares 1) are not evaluated, so a shorter input has none. The `redact` profile of
 * pb_detect and pb_redact evaluates inputs of any length. */
PB_API pb_status pb_scan(pb_session *session, const pb_model *model, const pb_input *inputs, size_t input_count,
                         const pb_scan_options *options, pb_scan_result **out, pb_error *err);
PB_API void pb_scan_result_free(pb_scan_result *result);

PB_API size_t pb_scan_result_windows(const pb_scan_result *result, size_t input, const pb_window **windows);
PB_API size_t pb_scan_result_spans(const pb_scan_result *result, size_t input, const pb_span **spans);
/* Values of head `head` in window `window` of input `input`: class probabilities (choice, multilabel), scores (score),
 * cumulative probabilities (ordinal); NULL when the head produced none there. */
PB_API const float *pb_scan_result_head_values(const pb_scan_result *result, size_t input, size_t window,
                                               int32_t head, size_t *count);
/* Per-byte labels of a byte_map head in a window (one byte per position); NULL when not computed. */
PB_API const uint8_t *pb_scan_result_byte_map(const pb_scan_result *result, size_t input, size_t window,
                                              int32_t head, size_t *count);

/* Final hidden states of `input` run as ONE sequence: `hidden` receives size * d_model floats. */
PB_API pb_status pb_encode(pb_session *session, const pb_model *model, const uint8_t *input, size_t size,
                           float *hidden, size_t hidden_capacity, pb_error *err);

/* -------------------------------------------------------------------------------------------- documents (detect) */

typedef struct pb_detector pb_detector;

/* A detector = one model (or an ensemble: models[0] is the main one) + a post-processing profile ("none", "secrets-code",
 * "secrets-binary", "redact"; NULL = the profile the model declares, else "none"). */
PB_API pb_status pb_detector_create(const pb_model *const *models, size_t model_count, const char *profile,
                                    pb_detector **out, pb_error *err);
PB_API void pb_detector_free(pb_detector *detector);
PB_API const char *pb_detector_profile(const pb_detector *detector);

/* What the detector's profile reads, for callers that walk directories: 1 when a file (by its name, without the
 * directory) is worth reading while walking, 1 when a directory is worth entering. A file named explicitly is always
 * read. Inputs larger than pb_detector_max_input_bytes are not analyzed (pb_detect reports them as failures). */
PB_API int32_t pb_detector_wants_file(const pb_detector *detector, const char *file_name);
PB_API int32_t pb_detector_wants_directory(const pb_detector *detector, const char *directory_name);
PB_API uint64_t pb_detector_max_input_bytes(const pb_detector *detector);

/* Severity of findings (spec/OUTPUT.md, section 5): an error should fail a check; a warning is reported without
 * failing it. */
#define PB_SEVERITY_ERROR 0
#define PB_SEVERITY_WARNING 1

/* The severity the detector's profile gives to the findings of a file, from the file's path inside the project scanned
 * (e.g. relative to the repository root; `/` or `\` as separators): PB_SEVERITY_ERROR, or PB_SEVERITY_WARNING where
 * the profile lowers it (secrets-code: test, example and documentation paths, such as tests/, src/test/, docs/,
 * examples/, *_test.go, *.md). Callers that walk directories use it to skip those paths when asked to. */
PB_API int32_t pb_detector_path_severity(const pb_detector *detector, const char *path);

/* pb_detect_options.flags */
#define PB_DETECT_REVEAL 0x1u          /* include the detected bytes themselves (default: masked) */
#define PB_DETECT_PER_MODEL 0x2u       /* also report every ensemble member's own results */
#define PB_DETECT_KEEP_EXAMPLES 0x4u   /* secrets-code: keep well-known documentation example keys */
#define PB_DETECT_PREFILTER 0x8u       /* see PB_SCAN_PREFILTER */
#define PB_DETECT_STRINGS_ONLY 0x10u   /* secrets-binary: keep only spans that touch a printable string */
#define PB_DETECT_EARLY_EXIT 0x20u     /* see PB_SCAN_EARLY_EXIT */
#define PB_DETECT_EXPAND_ARCHIVES 0x40u /* scan the members of zip/jar/apk/gzip/tar inputs that the profile wants */
#define PB_DETECT_NO_ENSEMBLE 0x80u    /* use only models[0] */

typedef struct pb_detect_options {
    uint32_t struct_size;     /* sizeof(pb_detect_options) */
    uint32_t flags;           /* PB_DETECT_* */
    int32_t votes;            /* ensemble votes needed; 0 = majority */
    int32_t use_bias;         /* see pb_scan_options */
    float bias;
    const float *type_bias;
    uint32_t type_bias_count;
    float min_confidence;     /* drop spans below this confidence (default 0) */
    const char *const *queries; /* see pb_scan_options */
    uint32_t query_count;
    /* Optional, one per input (NULL entries allowed): where the input is inside the project scanned, e.g. relative to
     * the repository root, for the profile's path rules (pb_detector_path_severity). NULL = the names. A caller that
     * names inputs by paths that start above the project (/home/me/test/repo/src/app.py) passes the part inside it. */
    const char *const *paths;
} pb_detect_options;

PB_INLINE void pb_detect_options_init(pb_detect_options *options) {
    if (!options) return;
    memset(options, 0, sizeof *options);
    options->struct_size = (uint32_t)sizeof *options;
}

/* Runs the detector over documents and returns one JSON object in the generic schema of spec/OUTPUT.md:
 * {"profile", "models", "votes_needed", "results": [...], "findings": [...], "failures": [...]}. There is one result
 * per analyzed document, in input order: an input, or with PB_DETECT_EXPAND_ARCHIVES each wanted member of an archive
 * input; its `input` field is the index of the input it comes from. `findings` is the flat list of every result's
 * findings; each has a `severity`, "error" or "warning", from the path of its document (options->paths, else its
 * name; an archive member's is its archive's, then "!/" and its name inside). `failures` ({"file", "reason"}) lists
 * what was not analyzed: inputs over the profile's size limit and archive members that could not be extracted.
 * `names` (optional) are the display names of the inputs. Free the string with pb_free. */
PB_API pb_status pb_detect(pb_session *session, const pb_detector *detector, const pb_input *inputs, size_t input_count,
                           const char *const *names, const pb_detect_options *options, char **json_out,
                           pb_error *err);

/* ----------------------------------------------------------------------------------------------------- redaction */

typedef struct pb_redaction pb_redaction;

#define PB_REDACT_WITH_MAP 0x1u /* build the reversible map (it holds the redacted values: handle it as a secret) */

typedef struct pb_redact_options {
    uint32_t struct_size;   /* sizeof(pb_redact_options) */
    uint32_t flags;         /* PB_REDACT_* */
    int32_t use_bias;       /* see pb_scan_options */
    float bias;
    const float *type_bias;
    uint32_t type_bias_count;
    float min_confidence;   /* spans below this confidence are left as they are */
    const char *types;      /* comma-separated type names to redact; NULL = every type */
    const char *name;       /* the input's display name, the `input` of the report and the map; NULL = "" */
} pb_redact_options;

PB_INLINE void pb_redact_options_init(pb_redact_options *options) {
    if (!options) return;
    memset(options, 0, sizeof *options);
    options->struct_size = (uint32_t)sizeof *options;
}

/* Redaction by copy: the output is the input byte for byte except the detected spans, each replaced by a typed
 * marker such as [EMAIL_1] (the same value gets the same marker within the document). The copy property is checked
 * before returning. The report (JSON) never holds the redacted values. Every profile analyzes inputs of any length
 * here (windows shorter than the model's `purebyte.window.min` included); an input larger than
 * pb_detector_max_input_bytes is refused with PB_ERR_ARGUMENT. A UTF-16 input that the profile reads as UTF-8
 * (secrets-code) is redacted in UTF-16: the markers are written in the input's encoding. */
PB_API pb_status pb_redact(pb_session *session, const pb_detector *detector, const uint8_t *input, size_t size,
                           const pb_redact_options *options, pb_redaction **out, pb_error *err);
PB_API const uint8_t *pb_redaction_output(const pb_redaction *redaction, size_t *size);
PB_API const char *pb_redaction_report(const pb_redaction *redaction);
PB_API const char *pb_redaction_map(const pb_redaction *redaction); /* NULL unless PB_REDACT_WITH_MAP */
PB_API void pb_redaction_free(pb_redaction *redaction);

/* Redaction of spans that come from anywhere (rules, another tool, a reviewer): the same copy, markers, report and
 * map as pb_redact. `type_order` (optional) lists the type names in the order that breaks ties between overlapping
 * spans of different types with the same confidence and length. A span whose `source` contains "rules" and whose
 * type has a strict syntax or checksum (EMAIL, IBAN, CREDIT_CARD, DNI_NIE, IP, URL_CREDENTIALS) outranks the others. */
typedef struct pb_redaction_span {
    int64_t start;
    int64_t end;
    const char *type;
    float confidence;
    const char *source; /* NULL = "model" */
} pb_redaction_span;

PB_API pb_status pb_redact_spans(const uint8_t *input, size_t size, const pb_redaction_span *spans, size_t span_count,
                                 const char *const *type_order, size_t type_count, uint32_t flags,
                                 pb_redaction **out, pb_error *err);

/* Rebuilds the original bytes from a redacted output and its map (by offsets, never by searching markers).
 * Free *out with pb_free. */
PB_API pb_status pb_restore(const uint8_t *redacted, size_t size, const char *map_json, uint8_t **out,
                            size_t *out_size, pb_error *err);

/* ------------------------------------------------------------------------------------------------------ archives */

/* zip (and jar, apk, whl...), gzip and tar are expanded in memory, recursively, within limits that make archive
 * bombs harmless. Members are named "outer.jar!/inner/path"; the part after the outer archive's name is cut in its
 * middle beyond 4 KiB. zip members that share bytes (entries pointing at the same data) are not extracted. At most
 * 1000 failures are listed per archive; a last one counts the others. At most 100,000 entries of an archive are read
 * (wanted or not); the rest are one failure. */
typedef struct pb_archive pb_archive;

#define PB_ARCHIVE_MAX_DEPTH 3                       /* the defaults of pb_archive_limits */
#define PB_ARCHIVE_MAX_MEMBER_BYTES (UINT64_C(64) << 20)
#define PB_ARCHIVE_MAX_TOTAL_BYTES (UINT64_C(256) << 20)
#define PB_ARCHIVE_MAX_RATIO 200

typedef struct pb_archive_limits {
    uint32_t struct_size;      /* sizeof(pb_archive_limits) */
    int32_t max_depth;         /* archives inside archives (default 3; 0..16, a larger value counts as 16, a negative
                                  one is PB_ERR_ARGUMENT) */
    uint64_t max_member_bytes; /* one decompressed member (default 64 MiB) */
    uint64_t max_total_bytes;  /* the work of one archive: every byte decompressed from it, whether its member is kept
                                  or refused, and the names reported (default 256 MiB) */
    uint64_t max_ratio;        /* decompressed / compressed size of a member (default 200) */
} pb_archive_limits;

PB_INLINE void pb_archive_limits_init(pb_archive_limits *limits) {
    if (!limits) return;
    memset(limits, 0, sizeof *limits);
    limits->struct_size = (uint32_t)sizeof *limits;
    limits->max_depth = PB_ARCHIVE_MAX_DEPTH;
    limits->max_member_bytes = PB_ARCHIVE_MAX_MEMBER_BYTES;
    limits->max_total_bytes = PB_ARCHIVE_MAX_TOTAL_BYTES;
    limits->max_ratio = PB_ARCHIVE_MAX_RATIO;
}
/* 1 when the bytes start like a zip, gzip or tar archive. */
PB_API int32_t pb_is_archive(const uint8_t *data, size_t size);
PB_API pb_status pb_archive_expand(const uint8_t *data, size_t size, const char *name, const pb_archive_limits *limits,
                                   pb_archive **out, pb_error *err);
PB_API size_t pb_archive_member_count(const pb_archive *archive);
PB_API const char *pb_archive_member_name(const pb_archive *archive, size_t index);
PB_API const uint8_t *pb_archive_member_data(const pb_archive *archive, size_t index, size_t *size);
/* Members that could not be extracted, as "name: reason". */
PB_API size_t pb_archive_failure_count(const pb_archive *archive);
PB_API const char *pb_archive_failure(const pb_archive *archive, size_t index);
PB_API void pb_archive_free(pb_archive *archive);

#ifdef __cplusplus
}
#endif

#endif /* PUREBYTE_PB_H */
