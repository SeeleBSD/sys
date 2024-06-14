rust_common_flags :=	--edition=2021 \
			-Drust_2018_idioms

rust_depfile =		rust.d

RSFLAGS := 		$(rust_common_flags) -O \
			--target=$(RUSTC_TARGET) \
			-Ccodegen-units=1 \
			-Cforce-unwind-tables=n \
			-Clto=n \
			-Cembed-bitcode=n \
			-Csymbol-mangling-version=v0 \
			-Crelocation-model=static \
			-Zfunction-sections=n \
			-Zemit-stack-sizes \
			-Ctarget-feature="-neon"
			
rustc_sysroot := $(shell $(RUSTC) --print sysroot)
RUST_LIB_SRC ?= /usr/local/lib/rustlib/src/rust/library

OBJS+=	core.o alloc.o bindings.o kernel.o build_error.o

bindgen_c_flags = ${CFLAGS} ${CPPFLAGS} ${PROF} -D__OpenBSD__ -D__BINDGEN__ -w --target=$(BINDGEN_TARGET)

rust_common_cmd = \
	RUST_MODFILE=$(modfile) $(RUSTC) $(RSFLAGS) \
	--sysroot=/dev/null \
	--crate-type rlib -L${.OBJDIR}
	

