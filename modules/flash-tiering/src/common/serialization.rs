//! Serialization callbacks for converting between robj* and byte buffers.
//!
//! These function pointers are provided by Valkey core during module init
//! and called on the IO worker thread to serialize keys/values before
//! writing to storage and deserialize them after reading back.

use std::ffi::c_void;
use std::os::raw::{c_char, c_int};
use std::ptr;

use crate::common::types::BackendError;

/// Function pointers provided by Valkey core for serializing/deserializing
/// robj* across the tiering boundary.
#[derive(Debug, Clone, Copy)]
pub struct SerializationCallbacks {
    pub serialize_key:
        unsafe extern "C" fn(key: *mut c_void, serialized_key: *mut *mut c_char) -> c_int,
    pub serialize_value:
        unsafe extern "C" fn(value: *mut c_void, serialized_value: *mut *mut c_char) -> c_int,
    pub deserialize_key:
        unsafe extern "C" fn(key: *mut c_char, length: c_int) -> *mut c_void,
    pub deserialize_value:
        unsafe extern "C" fn(value: *mut c_char, length: c_int) -> *mut c_void,
    pub free_serialized_key: unsafe extern "C" fn(key: *mut c_void),
    pub free_serialized_value: unsafe extern "C" fn(value: *mut c_void),
}

unsafe impl Send for SerializationCallbacks {}
unsafe impl Sync for SerializationCallbacks {}

impl SerializationCallbacks {
    /// Serialize a key robj* into a byte buffer.
    pub unsafe fn serialize_key_to_buf(
        &self,
        key_robj: *mut c_void,
    ) -> Result<(*mut c_char, usize), BackendError> {
        let mut buf: *mut c_char = ptr::null_mut();
        let len = (self.serialize_key)(key_robj, &mut buf);
        if len < 0 || buf.is_null() {
            return Err(BackendError::SerializationError(
                "serialize_key returned error or null buffer".into(),
            ));
        }
        Ok((buf, len as usize))
    }

    /// Serialize a value robj* into a byte buffer.
    pub unsafe fn serialize_value_to_buf(
        &self,
        value_robj: *mut c_void,
    ) -> Result<(*mut c_char, usize), BackendError> {
        let mut buf: *mut c_char = ptr::null_mut();
        let len = (self.serialize_value)(value_robj, &mut buf);
        if len < 0 || buf.is_null() {
            return Err(BackendError::SerializationError(
                "serialize_value returned error or null buffer".into(),
            ));
        }
        Ok((buf, len as usize))
    }

    /// Deserialize a value byte buffer into an robj*.
    pub unsafe fn deserialize_value_from_buf(
        &self,
        buf: *const c_char,
        len: usize,
    ) -> Result<*mut c_void, BackendError> {
        let robj = (self.deserialize_value)(buf as *mut c_char, len as c_int);
        if robj.is_null() {
            return Err(BackendError::SerializationError(
                "deserialize_value returned null".into(),
            ));
        }
        Ok(robj)
    }

    /// Free a buffer returned by `serialize_key_to_buf`.
    pub unsafe fn free_serialized_key_buf(&self, buf: *mut c_char) {
        (self.free_serialized_key)(buf as *mut c_void);
    }

    /// Free a buffer returned by `serialize_value_to_buf`.
    pub unsafe fn free_serialized_value_buf(&self, buf: *mut c_char) {
        (self.free_serialized_value)(buf as *mut c_void);
    }
}
