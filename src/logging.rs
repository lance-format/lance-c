// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Opt-in process-wide diagnostic logging.
//!
//! Lance and OpenDAL emit both `tracing` events and `log` records. The C ABI
//! cannot install either global automatically without surprising the host, so
//! callers opt in once through [`lance_init_logging`]. One `EnvFilter` controls
//! the stderr formatter and the `log` bridge, and neither global is replaced
//! when the host already installed its own.

use std::ffi::c_char;
use std::sync::Mutex;

use lance_core::{Error, Result};
use tracing_core::callsite::rebuild_interest_cache;
use tracing_log::AsLog;
use tracing_subscriber::EnvFilter;

use crate::error::ffi_try;
use crate::helpers;

const DEFAULT_FILTER: &str = "info";

/// Serializes concurrent initialization and records whether the first
/// successful call has installed the process globals.
static INITIALIZED: Mutex<bool> = Mutex::new(false);

/// Opt in to native diagnostic logging on stderr.
///
/// `filter` is a NUL-terminated filter expression using the same directive
/// syntax as Rust's `RUST_LOG` / `EnvFilter`: comma-separated `target=level`
/// directives with an optional bare default level. Common target prefixes are
/// `lance`, `lance_core`, `lance_io`, `opendal`, and `lance_c`. For example,
/// `"warn,lance=debug,opendal=debug"` enables warnings everywhere and debug
/// diagnostics for Lance and OpenDAL. NULL selects `"info"`; an empty string is
/// rejected.
///
/// This call is optional; without it the library emits no diagnostics. It
/// installs one stderr formatter for Rust `tracing` events and, unless the host
/// already installed a `log` logger, a bridge that routes `log` records through
/// the same filter. For the globals it installs, both event families obey the
/// filter; a host global keeps its own filtering.
///
/// The process-global logger and tracing subscriber can only be installed once.
/// This function never replaces either one: if the host already installed a
/// `log` logger or a `tracing` subscriber, that global remains in place and
/// receives its events. If both are already installed, this function leaves
/// them unchanged. Calls are thread-safe; the first successful call's filter
/// is final and later calls are no-ops. An invalid filter returns -1 with
/// `LANCE_ERR_INVALID_ARGUMENT` and leaves logging unconfigured, so a later
/// valid call can still enable it.
///
/// Filtering is evaluated per event; changing the filter after this call is not
/// supported.
///
/// @return 0 on success, -1 on an invalid or non-UTF-8 filter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_init_logging(filter: *const c_char) -> i32 {
    ffi_try!(unsafe { init_logging_inner(filter) }, -1)
}

unsafe fn init_logging_inner(filter: *const c_char) -> Result<i32> {
    let mut initialized = INITIALIZED
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if *initialized {
        return Ok(0);
    }

    let filter = unsafe { helpers::parse_c_string(filter)? }.unwrap_or(DEFAULT_FILTER);
    if filter.trim().is_empty() {
        return Err(Error::invalid_input(
            "logging filter must not be empty; pass NULL for the default \"info\" filter",
        ));
    }
    let filter = EnvFilter::try_new(filter).map_err(|error| {
        Error::invalid_input(format!("invalid logging filter {filter:?}: {error}"))
    })?;

    let subscriber = tracing_subscriber::fmt()
        .with_env_filter(filter)
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .finish();
    if tracing::subscriber::set_global_default(subscriber).is_err() {
        // `set_global_default` builds a `Dispatch` (which registers it and
        // rebuilds callsite interest) before checking the global slot. The
        // rejected Dispatch is dropped by now, but the interest and
        // `LevelFilter::current()` it contributed are not. Rebuild from the
        // surviving host dispatchers so a failed install leaves no stale level
        // or callsite interest in the host process. tracing-core has no public
        // atomic install-if-absent operation, so a concurrent host install has
        // this same failure path.
        rebuild_interest_cache();
    }

    // Match the surviving subscriber level so the `log` facade can skip records
    // before they reach the tracing dispatcher. If the host installed a `log`
    // logger, this fails without changing its max level or replacing it.
    let _ = tracing_log::LogTracer::builder()
        .with_max_level(tracing::level_filters::LevelFilter::current().as_log())
        .init();

    *initialized = true;
    Ok(0)
}
