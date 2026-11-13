//! Historical transaction-file readback across the core-15 migration.
//!
//! The fixture is produced by a Lance 14.0.2 `lance-c` build
//! (`.validation/core15-migration/fixtures/core14_transaction_producer.c`) and
//! contains one attached and one detached transaction with fixed UUIDs, tags
//! and properties. This test is ignored because it needs that external
//! fixture; run it with:
//!
//! ```text
//! LANCE_CORE14_FIXTURE_URI=/tmp/core14-txn-fixture.lance \
//! LANCE_CORE14_FIXTURE_DETACHED_VERSION=<version> \
//! cargo test --test core14_transaction_fixture_test -- --ignored
//! ```

use std::ffi::{CStr, CString};

use lance_c::*;

const ATTACHED_UUID: &str = "11111111-1111-1111-1111-111111111111";
const DETACHED_UUID: &str = "22222222-2222-2222-2222-222222222222";

fn last_error() -> String {
    unsafe {
        let message = lance_last_error_message();
        if message.is_null() {
            "no error message".to_string()
        } else {
            CStr::from_ptr(message).to_string_lossy().into_owned()
        }
    }
}

unsafe fn open(uri: &CStr, version: u64) -> *mut LanceDataset {
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), std::ptr::null(), version) };
    assert!(
        !dataset.is_null(),
        "open version {version}: {}",
        last_error()
    );
    dataset
}

unsafe fn transaction_string(
    getter: unsafe extern "C" fn(*const LanceTransaction) -> *const i8,
    transaction: *const LanceTransaction,
) -> String {
    let value = unsafe { getter(transaction) };
    assert!(!value.is_null(), "{}", last_error());
    let owned = unsafe { CStr::from_ptr(value).to_string_lossy().into_owned() };
    unsafe { lance_free_string(value as *mut i8) };
    owned
}

unsafe fn property(transaction: *const LanceTransaction, wanted: &str) -> String {
    let mut count = 0_u64;
    assert_eq!(
        unsafe { lance_transaction_property_count(transaction, &mut count) },
        0,
        "{}",
        last_error()
    );
    for index in 0..count as usize {
        let key = unsafe { lance_transaction_property_key_at(transaction, index) };
        assert!(!key.is_null(), "{}", last_error());
        let key_owned = unsafe { CStr::from_ptr(key).to_string_lossy().into_owned() };
        unsafe { lance_free_string(key as *mut i8) };
        if key_owned == wanted {
            let value = unsafe { lance_transaction_property_value_at(transaction, index) };
            assert!(!value.is_null(), "{}", last_error());
            let value_owned = unsafe { CStr::from_ptr(value).to_string_lossy().into_owned() };
            unsafe { lance_free_string(value as *mut i8) };
            return value_owned;
        }
    }
    panic!("missing transaction property {wanted}");
}

unsafe fn assert_transaction(
    transaction: *const LanceTransaction,
    read_version: u64,
    uuid: &str,
    tag: &str,
    stage: &str,
) {
    assert!(!transaction.is_null());
    let mut actual_read_version = u64::MAX;
    assert_eq!(
        unsafe { lance_transaction_read_version(transaction, &mut actual_read_version) },
        0,
        "{}",
        last_error()
    );
    assert_eq!(actual_read_version, read_version);
    assert_eq!(
        unsafe { transaction_string(lance_transaction_uuid, transaction) },
        uuid
    );
    let mut out_tag = std::ptr::null();
    assert_eq!(
        unsafe { lance_transaction_tag(transaction, &mut out_tag) },
        1,
        "{}",
        last_error()
    );
    let actual_tag = unsafe { CStr::from_ptr(out_tag).to_string_lossy().into_owned() };
    unsafe { lance_free_string(out_tag as *mut i8) };
    assert_eq!(actual_tag, tag);
    assert_eq!(unsafe { property(transaction, "stage") }, stage);
    assert_eq!(unsafe { property(transaction, "writer") }, "core14");

    let mut operation = std::ptr::null_mut();
    assert_eq!(
        unsafe { lance_transaction_operation(transaction, &mut operation) },
        0,
        "{}",
        last_error()
    );
    let mut kind = LANCE_OPERATION_UNKNOWN;
    assert_eq!(
        unsafe { lance_operation_kind(operation, &mut kind) },
        0,
        "{}",
        last_error()
    );
    assert_eq!(kind, LANCE_OPERATION_APPEND);
    let mut fragments = 0_u64;
    assert_eq!(
        unsafe { lance_operation_append_fragment_count(operation, &mut fragments) },
        0,
        "{}",
        last_error()
    );
    assert_eq!(fragments, 1);
    unsafe { lance_operation_close(operation) };
}

unsafe fn read_transaction(dataset: *const LanceDataset) -> *mut LanceTransaction {
    let mut transaction = std::ptr::null_mut();
    assert_eq!(
        unsafe { lance_dataset_read_transaction(dataset, &mut transaction) },
        0,
        "{}",
        last_error()
    );
    assert!(!transaction.is_null(), "missing transaction");
    transaction
}

#[test]
#[ignore = "needs a Lance 14.0.2 transaction fixture produced externally"]
fn core14_attached_and_detached_transactions_read_back() {
    let uri = std::env::var("LANCE_CORE14_FIXTURE_URI")
        .expect("LANCE_CORE14_FIXTURE_URI must name the fixture dataset");
    let detached_version: u64 = std::env::var("LANCE_CORE14_FIXTURE_DETACHED_VERSION")
        .expect("LANCE_CORE14_FIXTURE_DETACHED_VERSION must name the detached manifest")
        .parse()
        .expect("detached version must be a u64");
    assert_ne!(
        detached_version & (1_u64 << 63),
        0,
        "detached version must carry the native high bit"
    );
    let uri = CString::new(uri).unwrap();

    unsafe {
        let dataset = open(&uri, 0);
        let current = read_transaction(dataset);
        assert_transaction(current, 0, ATTACHED_UUID, "core14-attached", "attached");
        lance_transaction_close(current);

        let mut by_version = std::ptr::null_mut();
        assert_eq!(
            lance_dataset_read_transaction_by_version(dataset, 1, &mut by_version),
            0,
            "{}",
            last_error()
        );
        assert_transaction(by_version, 0, ATTACHED_UUID, "core14-attached", "attached");
        lance_transaction_close(by_version);
        lance_dataset_close(dataset);

        let detached = open(&uri, detached_version);
        let detached_transaction = read_transaction(detached);
        assert_transaction(
            detached_transaction,
            1,
            DETACHED_UUID,
            "core14-detached",
            "detached",
        );
        lance_transaction_close(detached_transaction);
        lance_dataset_close(detached);
    }
}
