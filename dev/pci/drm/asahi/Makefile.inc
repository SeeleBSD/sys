OBJS+=	asahi.o

asahi.o: kernel.o
	$(rust_common_cmd) --extern build_error --extern core --extern compiler_builtins --extern bindings --extern uapi --extern kernel --extern macros --emit=obj=$@ --crate-name asahi $S/dev/pci/drm/asahi/lib.rs
