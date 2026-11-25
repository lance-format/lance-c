// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Global Tokio runtime for the C FFI layer.

use std::sync::LazyLock;

/// Global multi-threaded Tokio runtime, shared across all FFI calls.
/// Initialized lazily on first access.
pub static RT: LazyLock<tokio::runtime::Runtime> = LazyLock::new(|| {
    // A native linker can omit OpenDAL's automatic constructor from liblance_c.a.
    // Keep initialization reachable from the FFI entry points, before any HTTP I/O.
    // Installation is idempotent and preserves an already installed transport.
    opendal::install_default();

    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("Failed to create tokio runtime")
});

/// Block the current thread on an async future using the global runtime.
pub fn block_on<F: std::future::Future>(f: F) -> F::Output {
    RT.block_on(f)
}

/// Stack reserved for synchronous FFI futures that nest a native namespace
/// publication and manifest scan. Windows main threads default to 1 MiB, while
/// a debug build of that path can exceed it; polling on a dedicated thread keeps
/// the C ABI independent of the caller's stack size.
const SYNC_FFI_STACK_SIZE: usize = 8 * 1024 * 1024;

/// Block on a synchronous FFI future without polling it on the caller's stack.
///
/// The future runs on a scoped thread with a platform-independent stack budget.
/// A panic is resumed on the caller so the existing FFI guard still reports it
/// as a panic.
pub(crate) fn block_on_with_large_stack<F, T>(f: F) -> T
where
    F: std::future::Future<Output = T> + Send,
    T: Send,
{
    std::thread::scope(|scope| {
        let handle = std::thread::Builder::new()
            .name("lance-c-sync".into())
            .stack_size(SYNC_FFI_STACK_SIZE)
            .spawn_scoped(scope, || block_on(f))
            .expect("failed to spawn the synchronous FFI executor");
        match handle.join() {
            Ok(value) => value,
            Err(payload) => std::panic::resume_unwind(payload),
        }
    })
}
