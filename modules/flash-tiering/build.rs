// Build script for key-spilling-module.
//
// With feature "backend-flashcache":
//   - Compiles fc_shim.c (FlashCache C callbacks)
//   - Links against libflashcache at runtime via dlopen
//
// With feature "backend-rocksdb" (default):
//   - No C compilation needed (RocksDB linked via the rocksdb crate)

fn main() {
    println!("cargo:rerun-if-changed=build.rs");

    #[cfg(feature = "backend-flashcache")]
    {
        use std::path::Path;
        println!("cargo:rerun-if-changed=src/backends/flashcache/fc_shim.c");

        // FlashCache source is at <repo>/deps/flashcache/src
        let fc_include_paths = [
            "../../deps/flashcache/src",
            "../../deps/flashcache/src/include",
        ];

        let mut build = cc::Build::new();
        build.file("src/backends/flashcache/fc_shim.c").warnings(false);

        for path in &fc_include_paths {
            if Path::new(path).exists() {
                build.include(path);
                let include_subdir = Path::new(path).join("include");
                if include_subdir.exists() {
                    build.include(&include_subdir);
                }
            }
        }

        build.compile("fc_shim");

        println!("cargo:rustc-link-lib=dylib=aio");
        println!("cargo:rustc-link-lib=dylib=rt");
        // Force linker to not drop libaio (needed by libflashcache.a)
        println!("cargo:rustc-cdylib-link-arg=-laio");
        println!("cargo:rustc-cdylib-link-arg=-lrt");

        // FlashCache build output at <repo>/deps/flashcache/build/
        let fc_build_dirs = [
            "../../deps/flashcache/build",
        ];
        let fc_dir = fc_build_dirs.iter()
            .map(|p| Path::new(p))
            .find(|p| p.exists())
            .unwrap_or_else(|| panic!(
                "FlashCache build dir not found. Build FlashCache first:\n\
                 cd FlashCache/build_dir && cmake3 .. -DCMAKE_BUILD_TYPE=Release && make -j8"
            ));
        let fc_build_abs = std::fs::canonicalize(fc_dir)
            .expect("Failed to canonicalize FlashCache build dir");
        println!("cargo:rustc-link-search=native={}", fc_build_abs.display());
        println!("cargo:rustc-link-lib=static=flashcache");
    }
}
