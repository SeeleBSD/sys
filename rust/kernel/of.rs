// SPDX-License-Identifier: ISC

use crate::{bindings, prelude::*};
use alloc::{vec, vec::Vec};

pub struct NodeIter {
    curr: *mut bindings::device_node,
    is_halt: bool,
    is_first: bool,
}

impl Iterator for NodeIter {
    type Item = Node;

    fn next(&mut self) -> Option<Self::Item> {
        if self.is_halt {
            None
        } else {
            if self.is_first {
                self.is_first = false;
                if self.curr.is_null() {
                    None
                } else {
                    unsafe { Node::from_raw(self.curr) }
                }
            } else {
                self.curr = unsafe {
                    bindings::fdt_next_node(self.curr as *mut core::ffi::c_void)
                        as *mut bindings::device_node
                };
                self.is_halt = self.curr.is_null();
                unsafe { Node::from_raw(self.curr) }
            }
        }
    }
}

#[derive(Clone, Copy)]
pub struct Node {
    raw_node: *mut bindings::device_node,
}

impl Node {
    pub unsafe fn from_raw(node: *mut bindings::device_node) -> Option<Node> {
        if node.is_null() {
            None
        } else {
            Some(Node { raw_node: node })
        }
    }

    pub fn from_handle(handle: i32) -> Option<Node> {
        let node = (unsafe { bindings::fdt_get().header.addr() } + (handle as usize))
            as *mut bindings::device_node;
        if node.is_null() {
            None
        } else {
            Some(Node { raw_node: node })
        }
    }

    pub fn node(&self) -> &bindings::device_node {
        unsafe { &*self.raw_node }
    }

    pub fn handle(&self) -> i32 {
        (self.raw_node.addr() - unsafe { bindings::fdt_get().header.addr() }) as i32
    }

    pub fn full_name(&self) -> &CStr {
        unsafe { CStr::from_char_ptr(self.node().full_name) }
    }

    pub fn child(&self) -> NodeIter {
        NodeIter {
            curr: self.raw_node,
            is_halt: false,
            is_first: true,
        }
    }

    pub fn parent(&self) -> Option<Self> {
        let par = unsafe { bindings::fdt_parent_node(self.raw_node as *mut core::ffi::c_void) };
        if par.is_null() {
            None
        } else {
            Some(Self {
                raw_node: par as *mut bindings::device_node,
            })
        }
    }

    pub fn is_compatible(&self, name: &CStr) -> i32 {
        unsafe {
            bindings::fdt_is_compatible(self.raw_node as *mut core::ffi::c_void, name.as_char_ptr())
        }
    }

    pub fn find_property(&self, name: &CStr) -> Option<Property> {
        unsafe {
            let len = bindings::OF_getproplen(self.handle(), name.as_char_ptr() as *mut i8);
            let mut buf = vec![0u8; len as usize];
            if len
                == bindings::OF_getprop(
                    self.handle(),
                    name.as_char_ptr() as *mut i8,
                    buf.as_mut_ptr() as *mut core::ffi::c_void,
                    len as i32,
                )
            {
                Some(Property::from_vec(buf))
            } else {
                None
            }
        }
    }

    pub fn get_property<T: TryFrom<Property>>(&self, name: &CStr) -> Result<T>
    where
        crate::error::Error: From<<T as TryFrom<Property>>::Error>,
    {
        Ok(self.find_property(name).ok_or(ENOENT)?.try_into()?)
    }

    pub fn get_opt_property<T: TryFrom<Property>>(&self, name: &CStr) -> Result<Option<T>>
    where
        crate::error::Error: From<<T as TryFrom<Property>>::Error>,
    {
        self.find_property(name)
            .map_or(Ok(None), |prop| Ok(Some(prop.try_into()?)))
    }
}

pub struct Property {
    value: Vec<u8>,
}

impl Property {
    fn from_vec(data: Vec<u8>) -> Self {
        Self { value: data }
    }

    pub fn len(&self) -> usize {
        self.value.len()
    }

    pub fn value(&self) -> &[u8] {
        self.value.as_slice()
    }
}

pub trait PropertyUnit: Sized {
    const UNIT_SIZE: usize;

    fn from_bytes(data: &[u8]) -> Result<Self>;
}

impl<T: PropertyUnit> TryFrom<Property> for Vec<T> {
    type Error = Error;

    fn try_from(prop: Property) -> core::result::Result<Vec<T>, Self::Error> {
        if prop.len() % T::UNIT_SIZE == 0 {
            let mut ret = vec![];
            let val = prop.value();
            for i in (0..prop.len()).step_by(T::UNIT_SIZE) {
                ret.push(T::from_bytes(&val[i..i + T::UNIT_SIZE])?);
            }
            Ok(ret)
        } else {
            Err(EINVAL)
        }
    }
}

macro_rules! prop_int_type {
    ($type:ty) => {
        impl TryFrom<Property> for $type {
            type Error = Error;

            fn try_from(prop: Property) -> core::result::Result<$type, Self::Error> {
                Ok(<$type>::from_be_bytes(
                    prop.value().try_into().or(Err(EINVAL))?,
                ))
            }
        }

        impl PropertyUnit for $type {
            const UNIT_SIZE: usize = <$type>::BITS as usize >> 3;

            fn from_bytes(data: &[u8]) -> Result<Self> {
                Ok(<$type>::from_be_bytes(data.try_into().or(Err(EINVAL))?))
            }
        }
    };
}

prop_int_type!(i8);
prop_int_type!(i16);
prop_int_type!(i32);
prop_int_type!(i64);
prop_int_type!(u8);
prop_int_type!(u16);
prop_int_type!(u32);
prop_int_type!(u64);

#[macro_export]
macro_rules! compatible {
    ($node:tt, $table:tt) => {{
        {
            let mut ret: i32 = 0;
            for (name, _) in $table {
                ret |= $node.is_compatible(name);
            }
            ret
        }
    }};
}

#[macro_export]
macro_rules! compatible_info {
    ($node:tt, $table:tt) => {{
        {
            let mut ret = None;
            for (name, val) in $table {
                if $node.is_compatible(name) != 0 {
                    ret = val;
                    break;
                }
            }
            ret
        }
    }};
}

#[macro_export]
macro_rules! id_table {
    ($name:ident, $data_ty:ty, [ $($t:tt)* ]) => {
        const $name: [(&'static $crate::str::CStr, Option<$data_ty>);$crate::count_items!($($t)*)] = [$($t)*];
    };
}
