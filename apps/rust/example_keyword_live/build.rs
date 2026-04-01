fn main() {
    // Model data is now compiled by the build system (cmake/OveModels.cmake)
    // from models/*.tflite. No cc crate compilation needed.
    println!("cargo:rerun-if-changed=build.rs");
}
