/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file error.h
 * @brief Error codes and the thread-local error accessors.
 */

#ifndef LANCE_ERROR_H
#define LANCE_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Error handling ─── */

typedef enum {
    LANCE_OK = 0,
    LANCE_ERR_INVALID_ARGUMENT = 1,
    LANCE_ERR_IO = 2,
    LANCE_ERR_NOT_FOUND = 3,
    LANCE_ERR_DATASET_ALREADY_EXISTS = 4,
    LANCE_ERR_INDEX = 5,
    LANCE_ERR_INTERNAL = 6,
    LANCE_ERR_NOT_SUPPORTED = 7,
    LANCE_ERR_COMMIT_CONFLICT = 8,
    /* An unexpected panic was caught at the FFI boundary. */
    LANCE_ERR_PANIC = 9,
} LanceErrorCode;

/**
 * Panic handling (issue lance-format/lance-c#61).
 *
 * A panic raised inside Lance/Arrow code is caught at the FFI boundary and
 * reported as LANCE_ERR_PANIC through the usual thread-local error channel
 * (lance_last_error_code / lance_last_error_message) instead of unwinding
 * into the host. After a panic:
 *
 *  - A LanceScanner handle is poisoned: every later call on it fails with
 *    LANCE_ERR_PANIC ("scanner is poisoned by an earlier panic"). Close it
 *    and build a fresh scanner; do not retry the poisoned handle.
 *  - A LanceDataset handle remains usable: commits are atomic manifest
 *    swaps, and a mutation that panics before commit rolls back in memory,
 *    leaving the last committed snapshot intact.
 *
 * Honest limits: a double panic, a panic in a destructor while unwinding, a
 * stack overflow, or an allocation failure still aborts the process. A
 * panic caught inside a close/free call (lance_*_close, lance_batch_free,
 * lance_free_string, lance_scanner_async_stream_free, or the release callback
 * of an exported ArrowArrayStream) is logged and the remainder of the value
 * may leak — close is best-effort by design. Post-panic process state is
 * best-effort: hosts should fail the in-flight query rather than retry a
 * poisoned handle.
 *
 * Callbacks passed INTO the library (LanceCallback, LanceWaker, and
 * LanceScanStatisticsCallback, LanceTrackedFilesProgressCallback and
 * LanceTrackedFilesProgressRelease) are the reverse direction and are NOT
 * covered by this contract: their ABI is non-unwinding, so a callback that
 * throws or unwinds can abort the host process before the library can contain
 * it. Callbacks must return normally.
 *
 * This contract requires Rust's `panic = "unwind"` strategy. The crate
 * rejects `panic = "abort"` builds at compile time because catch_unwind
 * cannot provide this API contract in such a build.
 */


/** Return the error code from the last failed operation on this thread. */
LanceErrorCode lance_last_error_code(void);

/** Return the error message. Caller must free with lance_free_string(). */
const char* lance_last_error_message(void);

/** Free a string returned by lance_last_error_message(). */
void lance_free_string(const char* s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_ERROR_H */
