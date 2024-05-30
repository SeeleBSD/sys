OBJS+=	bronya.o

bronya.o: kernel.o
	$(rust_common_cmd) --extern build_error --extern core --extern alloc --extern compiler_builtins --extern bindings --extern kernel --emit=obj=$@ --crate-name bronya $S/dev/pci/drm/bronya/lib.rs
