fn main() {
    // Re-run the build script if the linker config or oveRTOS bindings
    // change.  The actual link-line / cfg setup is forwarded by the
    // `ove` crate via its build.rs.
    println!("cargo:rerun-if-changed=build.rs");
}
