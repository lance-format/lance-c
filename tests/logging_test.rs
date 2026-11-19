// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Process-isolated tests for the opt-in diagnostic logging contract.
//!
//! Global loggers and tracing subscribers cannot be uninstalled, so each case
//! re-runs itself in a child process with `--exact --test-threads=1`. This
//! keeps the default-silent and host-global-preservation assertions
//! deterministic in one `cargo test` run.

use std::ffi::{CStr, CString};
use std::io::Write as _;
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Barrier, Mutex, OnceLock};

use lance_c::{
    LanceErrorCode, lance_free_string, lance_init_logging, lance_last_error_code,
    lance_last_error_message,
};
use tracing::level_filters::LevelFilter as TracingLevelFilter;
use tracing_subscriber::Layer as _;
use tracing_subscriber::layer::SubscriberExt;

const CHILD_ENV: &str = "LANCE_C_LOGGING_TEST_CHILD";

const DEFAULT_LOG_TARGET: &str = "lance_c_logging_default_log";
const DEFAULT_TRACE_TARGET: &str = "lance_c_logging_default_trace";
const OPT_IN_TARGET: &str = "lance_c_logging_opt_in";
const FILTER_TARGET: &str = "lance_c_logging_filter";
const HOST_LOGGER_LOG_TARGET: &str = "lance_c_logging_host_logger_log";
const HOST_LOGGER_TRACE_TARGET: &str = "lance_c_logging_host_logger_trace";
const HOST_SUBSCRIBER_LOG_TARGET: &str = "lance_c_logging_host_subscriber_log";
const HOST_SUBSCRIBER_TRACE_TARGET: &str = "lance_c_logging_host_subscriber_trace";
const FIRST_TARGET: &str = "lance_c_logging_first";
const CONCURRENT_TARGET: &str = "lance_c_logging_concurrent";
const RESTRICTED_TRACE_TARGET: &str = "lance_c_logging_restricted_trace";
const RESTRICTED_WARN_TARGET: &str = "lance_c_logging_restricted_warn";
const RESTRICTED_LOG_INFO_TARGET: &str = "lance_c_logging_restricted_log_info";
const RESTRICTED_LOG_WARN_TARGET: &str = "lance_c_logging_restricted_log_warn";
const BOTH_TRACE_TARGET: &str = "lance_c_logging_both_trace";
const BOTH_WARN_TARGET: &str = "lance_c_logging_both_warn";
const BOTH_LOG_INFO_TARGET: &str = "lance_c_logging_both_log_info";
const BOTH_LOG_WARN_TARGET: &str = "lance_c_logging_both_log_warn";
const SCOPED_TARGET: &str = "lance_c_logging_scoped_only";

static LOG_RECORDS: OnceLock<Mutex<Vec<String>>> = OnceLock::new();

struct RecordingLogger;

impl log::Log for RecordingLogger {
    fn enabled(&self, _: &log::Metadata<'_>) -> bool {
        true
    }

    fn log(&self, record: &log::Record<'_>) {
        LOG_RECORDS
            .get_or_init(|| Mutex::new(Vec::new()))
            .lock()
            .expect("record mutex")
            .push(record.target().to_owned());
    }

    fn flush(&self) {}
}

fn install_host_logger(max_level: log::LevelFilter) -> &'static Mutex<Vec<String>> {
    log::set_boxed_logger(Box::new(RecordingLogger)).expect("install host logger");
    log::set_max_level(max_level);
    LOG_RECORDS.get_or_init(|| Mutex::new(Vec::new()))
}

#[derive(Default)]
struct EventTarget {
    target: Option<String>,
}

impl tracing::field::Visit for EventTarget {
    fn record_str(&mut self, field: &tracing::field::Field, value: &str) {
        if matches!(field.name(), "target" | "log.target") {
            self.target = Some(value.to_owned());
        }
    }

    fn record_debug(&mut self, _: &tracing::field::Field, _: &dyn std::fmt::Debug) {}
}

#[derive(Clone, Default)]
struct TargetLayer {
    targets: Arc<Mutex<Vec<String>>>,
}

impl<S: tracing::Subscriber> tracing_subscriber::Layer<S> for TargetLayer {
    fn on_event(
        &self,
        event: &tracing::Event<'_>,
        _context: tracing_subscriber::layer::Context<'_, S>,
    ) {
        let mut fields = EventTarget::default();
        event.record(&mut fields);
        // `tracing-log` events carry the original `log` target as a field;
        // native tracing events carry it in the event metadata.
        let target = fields
            .target
            .unwrap_or_else(|| event.metadata().target().to_owned());
        self.targets.lock().expect("target mutex").push(target);
    }
}

fn install_host_subscriber(filter: Option<&str>) -> Arc<Mutex<Vec<String>>> {
    let targets = Arc::new(Mutex::new(Vec::new()));
    let layer = TargetLayer {
        targets: Arc::clone(&targets),
    };
    if let Some(filter) = filter {
        let subscriber = tracing_subscriber::registry()
            .with(layer.with_filter(tracing_subscriber::EnvFilter::new(filter)));
        tracing::subscriber::set_global_default(subscriber)
            .expect("install filtered host subscriber");
    } else {
        let subscriber = tracing_subscriber::registry().with(layer);
        tracing::subscriber::set_global_default(subscriber).expect("install host subscriber");
    }
    targets
}

fn run_child(test_name: &str) -> std::process::Output {
    let exe = std::env::current_exe().expect("current test executable");
    Command::new(exe)
        .args([test_name, "--exact", "--nocapture", "--test-threads=1"])
        .env(CHILD_ENV, "1")
        .env_remove("RUST_LOG")
        .env_remove("LANCE_LOG")
        .env_remove("LANCE_LOG_FILE")
        .env_remove("LANCE_LOG_STYLE")
        .env_remove("LANCE_LOG_TS_PRECISION")
        .output()
        .expect("run logging test child")
}

fn assert_child_success(output: &std::process::Output) {
    assert!(
        output.status.success(),
        "child exited with {:?}\nstdout:\n{}\nstderr:\n{}",
        output.status,
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn init_logging(filter: &str) -> i32 {
    let filter = CString::new(filter).expect("test filter must not contain NUL");
    unsafe { lance_init_logging(filter.as_ptr()) }
}

fn init_logging_default() -> i32 {
    unsafe { lance_init_logging(std::ptr::null()) }
}

fn take_last_error_message() -> Option<String> {
    let ptr = lance_last_error_message();
    if ptr.is_null() {
        return None;
    }
    let message = unsafe { CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned();
    unsafe { lance_free_string(ptr) };
    Some(message)
}

#[test]
fn default_without_init_does_not_log_to_stderr() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("default_without_init_does_not_log_to_stderr");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(!stderr.contains("DEFAULT_LOG_MARKER"), "stderr:\n{stderr}");
        assert!(
            !stderr.contains("DEFAULT_TRACE_MARKER"),
            "stderr:\n{stderr}"
        );
        return;
    }

    log::error!(target: DEFAULT_LOG_TARGET, "DEFAULT_LOG_MARKER");
    tracing::error!(target: DEFAULT_TRACE_TARGET, "DEFAULT_TRACE_MARKER");
}

#[test]
fn init_with_null_filter_logs_both_families_to_stderr() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("init_with_null_filter_logs_both_families_to_stderr");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains("OPT_IN_LOG_MARKER"), "stderr:\n{stderr}");
        assert!(stderr.contains("OPT_IN_TRACE_MARKER"), "stderr:\n{stderr}");
        return;
    }

    assert_eq!(init_logging_default(), 0);
    log::info!(target: OPT_IN_TARGET, "OPT_IN_LOG_MARKER");
    tracing::info!(target: OPT_IN_TARGET, "OPT_IN_TRACE_MARKER");
}

#[test]
fn filter_applies_to_log_and_tracing_events() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("filter_applies_to_log_and_tracing_events");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(!stderr.contains("FILTERED_LOG_INFO"), "stderr:\n{stderr}");
        assert!(!stderr.contains("FILTERED_TRACE_INFO"), "stderr:\n{stderr}");
        assert!(stderr.contains("VISIBLE_LOG_WARN"), "stderr:\n{stderr}");
        assert!(stderr.contains("VISIBLE_TRACE_ERROR"), "stderr:\n{stderr}");
        return;
    }

    assert_eq!(init_logging("lance_c_logging_filter=warn"), 0);
    log::info!(target: FILTER_TARGET, "FILTERED_LOG_INFO");
    tracing::info!(target: FILTER_TARGET, "FILTERED_TRACE_INFO");
    log::warn!(target: FILTER_TARGET, "VISIBLE_LOG_WARN");
    tracing::error!(target: FILTER_TARGET, "VISIBLE_TRACE_ERROR");
}

#[test]
fn invalid_filter_fails_then_a_valid_call_can_init() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("invalid_filter_fails_then_a_valid_call_can_init");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains("RETRY_LOG_MARKER"), "stderr:\n{stderr}");
        assert!(stderr.contains("RETRY_TRACE_MARKER"), "stderr:\n{stderr}");
        return;
    }

    assert_eq!(init_logging("lance=definitely_not_a_level"), -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let message = take_last_error_message().expect("invalid filter must set an error");
    assert!(message.contains("invalid logging filter"), "{message}");

    assert_eq!(init_logging("lance_c_logging_retry=info"), 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
    log::error!(target: "lance_c_logging_retry", "RETRY_LOG_MARKER");
    tracing::error!(target: "lance_c_logging_retry", "RETRY_TRACE_MARKER");
}

#[test]
fn host_log_logger_is_preserved() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("host_log_logger_is_preserved");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            stderr.contains("HOST_LOGGER_TRACE_MARKER"),
            "stderr:\n{stderr}"
        );
        assert!(
            !stderr.contains("HOST_LOGGER_LOG_MARKER"),
            "stderr:\n{stderr}"
        );
        return;
    }

    let records = install_host_logger(log::LevelFilter::Warn);

    assert_eq!(init_logging("lance_c_logging_host_logger=info"), 0);
    assert_eq!(log::max_level(), log::LevelFilter::Warn);
    log::warn!(target: HOST_LOGGER_LOG_TARGET, "HOST_LOGGER_LOG_MARKER");
    tracing::error!(target: HOST_LOGGER_TRACE_TARGET, "HOST_LOGGER_TRACE_MARKER");

    let records = records.lock().expect("record mutex");
    assert!(
        records
            .iter()
            .any(|target| target == HOST_LOGGER_LOG_TARGET),
        "host logger did not receive the log record: {records:?}"
    );
}

#[test]
fn host_tracing_subscriber_is_preserved() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("host_tracing_subscriber_is_preserved");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            !stderr.contains("HOST_SUBSCRIBER_LOG_MARKER"),
            "stderr:\n{stderr}"
        );
        assert!(
            !stderr.contains("HOST_SUBSCRIBER_TRACE_MARKER"),
            "stderr:\n{stderr}"
        );
        return;
    }

    let targets = install_host_subscriber(None);

    assert_eq!(init_logging("lance_c_logging_host_subscriber=info"), 0);
    log::error!(target: HOST_SUBSCRIBER_LOG_TARGET, "HOST_SUBSCRIBER_LOG_MARKER");
    tracing::error!(
        target: HOST_SUBSCRIBER_TRACE_TARGET,
        "HOST_SUBSCRIBER_TRACE_MARKER"
    );

    let targets = targets.lock().expect("target mutex");
    assert!(
        targets
            .iter()
            .any(|target| target == HOST_SUBSCRIBER_LOG_TARGET),
        "host subscriber did not receive the log record: {targets:?}"
    );
    assert!(
        targets
            .iter()
            .any(|target| target == HOST_SUBSCRIBER_TRACE_TARGET),
        "host subscriber did not receive the tracing event: {targets:?}"
    );
}

#[test]
fn restricted_host_subscriber_state_is_preserved() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("restricted_host_subscriber_state_is_preserved");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            !stderr.contains("RESTRICTED_TRACE_MARKER"),
            "stderr:\n{stderr}"
        );
        assert!(
            !stderr.contains("RESTRICTED_WARN_MARKER"),
            "stderr:\n{stderr}"
        );
        assert!(
            !stderr.contains("RESTRICTED_LOG_INFO_MARKER"),
            "stderr:\n{stderr}"
        );
        assert!(
            !stderr.contains("RESTRICTED_LOG_WARN_MARKER"),
            "stderr:\n{stderr}"
        );
        return;
    }

    let targets = install_host_subscriber(Some("warn"));
    assert_eq!(TracingLevelFilter::current(), TracingLevelFilter::WARN);
    assert_eq!(log::max_level(), log::LevelFilter::Off);

    assert_eq!(init_logging("trace"), 0);
    assert_eq!(
        TracingLevelFilter::current(),
        TracingLevelFilter::WARN,
        "a failed global install must not raise the host subscriber level"
    );
    assert_eq!(
        log::max_level(),
        log::LevelFilter::Warn,
        "the log bridge must use the surviving host subscriber level"
    );

    tracing::trace!(target: RESTRICTED_TRACE_TARGET, "RESTRICTED_TRACE_MARKER");
    tracing::warn!(target: RESTRICTED_WARN_TARGET, "RESTRICTED_WARN_MARKER");
    log::info!(target: RESTRICTED_LOG_INFO_TARGET, "RESTRICTED_LOG_INFO_MARKER");
    log::warn!(target: RESTRICTED_LOG_WARN_TARGET, "RESTRICTED_LOG_WARN_MARKER");

    let targets = targets.lock().expect("target mutex");
    assert!(
        !targets
            .iter()
            .any(|target| target == RESTRICTED_TRACE_TARGET),
        "stale callsite interest delivered a trace event to the warn host: {targets:?}"
    );
    assert!(
        !targets
            .iter()
            .any(|target| target == RESTRICTED_LOG_INFO_TARGET),
        "stale log max level forwarded an info record to the warn host: {targets:?}"
    );
    assert!(
        targets
            .iter()
            .any(|target| target == RESTRICTED_WARN_TARGET),
        "host subscriber did not receive the warn tracing event: {targets:?}"
    );
    assert!(
        targets
            .iter()
            .any(|target| target == RESTRICTED_LOG_WARN_TARGET),
        "host subscriber did not receive the warn log record: {targets:?}"
    );
}

#[test]
fn both_host_globals_keep_their_filter_and_log_level() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("both_host_globals_keep_their_filter_and_log_level");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        for marker in [
            "BOTH_TRACE_MARKER",
            "BOTH_WARN_MARKER",
            "BOTH_LOG_INFO_MARKER",
            "BOTH_LOG_WARN_MARKER",
        ] {
            assert!(
                !stderr.contains(marker),
                "{marker} leaked to stderr:\n{stderr}"
            );
        }
        return;
    }

    let targets = install_host_subscriber(Some("warn"));
    let records = install_host_logger(log::LevelFilter::Warn);
    assert_eq!(TracingLevelFilter::current(), TracingLevelFilter::WARN);
    assert_eq!(log::max_level(), log::LevelFilter::Warn);

    assert_eq!(init_logging("trace"), 0);
    assert_eq!(
        TracingLevelFilter::current(),
        TracingLevelFilter::WARN,
        "both-global path must preserve the host subscriber level"
    );
    assert_eq!(
        log::max_level(),
        log::LevelFilter::Warn,
        "both-global path must preserve the host logger max level"
    );

    tracing::trace!(target: BOTH_TRACE_TARGET, "BOTH_TRACE_MARKER");
    tracing::warn!(target: BOTH_WARN_TARGET, "BOTH_WARN_MARKER");
    log::info!(target: BOTH_LOG_INFO_TARGET, "BOTH_LOG_INFO_MARKER");
    log::warn!(target: BOTH_LOG_WARN_TARGET, "BOTH_LOG_WARN_MARKER");

    let targets = targets.lock().expect("target mutex");
    assert!(
        !targets.iter().any(|target| target == BOTH_TRACE_TARGET),
        "both-global path delivered a trace event to the warn host: {targets:?}"
    );
    assert!(
        targets.iter().any(|target| target == BOTH_WARN_TARGET),
        "host subscriber did not receive the warn tracing event: {targets:?}"
    );

    let records = records.lock().expect("record mutex");
    assert!(
        !records.iter().any(|target| target == BOTH_LOG_INFO_TARGET),
        "both-global path forwarded an info record to the warn logger: {records:?}"
    );
    assert!(
        records.iter().any(|target| target == BOTH_LOG_WARN_TARGET),
        "host logger did not receive the warn record: {records:?}"
    );
}

#[test]
fn scoped_host_dispatch_does_not_block_global_install() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("scoped_host_dispatch_does_not_block_global_install");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains("SCOPED_ONLY_MARKER"), "stderr:\n{stderr}");
        return;
    }

    let scoped = tracing::Dispatch::new(tracing::subscriber::NoSubscriber::default());
    tracing::dispatcher::with_default(&scoped, || {
        assert_eq!(init_logging("lance_c_logging_scoped_only=info"), 0);
    });
    drop(scoped);

    tracing::info!(target: SCOPED_TARGET, "SCOPED_ONLY_MARKER");
}

#[test]
fn first_successful_filter_is_final() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("first_successful_filter_is_final");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains("FIRST_LOG_MARKER"), "stderr:\n{stderr}");
        assert!(stderr.contains("FIRST_TRACE_MARKER"), "stderr:\n{stderr}");
        return;
    }

    assert_eq!(init_logging("lance_c_logging_first=error"), 0);
    assert_eq!(init_logging("off"), 0);
    log::error!(target: FIRST_TARGET, "FIRST_LOG_MARKER");
    tracing::error!(target: FIRST_TARGET, "FIRST_TRACE_MARKER");
}

#[test]
fn concurrent_init_calls_are_idempotent() {
    if std::env::var_os(CHILD_ENV).is_none() {
        let output = run_child("concurrent_init_calls_are_idempotent");
        assert_child_success(&output);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert_eq!(
            stderr.matches("CONCURRENT_MARKER").count(),
            1,
            "stderr:\n{stderr}"
        );
        return;
    }

    const THREADS: usize = 8;
    let barrier = Arc::new(Barrier::new(THREADS + 1));
    let failures = Arc::new(AtomicUsize::new(0));
    let mut handles = Vec::new();
    for _ in 0..THREADS {
        let barrier = Arc::clone(&barrier);
        let failures = Arc::clone(&failures);
        handles.push(std::thread::spawn(move || {
            barrier.wait();
            if init_logging("lance_c_logging_concurrent=error") != 0 {
                failures.fetch_add(1, Ordering::Relaxed);
            }
        }));
    }
    barrier.wait();
    for handle in handles {
        handle.join().expect("init thread");
    }
    assert_eq!(failures.load(Ordering::Relaxed), 0);

    log::error!(target: CONCURRENT_TARGET, "CONCURRENT_MARKER");
    std::io::stderr().flush().expect("flush stderr");
}
