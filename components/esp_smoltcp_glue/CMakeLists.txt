# Wraps the Rust smoltcp staticlib into an IDF component.
#
# Build flow:
#   cargo build --release --target riscv32imac-unknown-none-elf
#       -> $(CMAKE_CURRENT_BINARY_DIR)/rust/.../libsmoltcp_glue.a
#   IDF then links that .a into the final ELF.

idf_component_register(
    SRCS "src/glue_stub.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

set(RUST_TARGET   "riscv32imafc-unknown-none-elf")
set(RUST_PROFILE  "release")
set(CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust")
set(RUST_LIB
    "${CARGO_TARGET_DIR}/${RUST_TARGET}/${RUST_PROFILE}/libsmoltcp_glue.a")

# Track every Rust source so an edit triggers a rebuild.
file(GLOB_RECURSE RUST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/.cargo/config.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml")

# Prefer rustup's cargo (~/.cargo/bin/cargo) — it's the proxy that honours
# rust-toolchain.toml. A standalone cargo in PATH (e.g. from Homebrew)
# ignores the toolchain pin and will fail with "can't find crate for core"
# under -Zbuild-std.
find_program(CARGO_EXECUTABLE cargo
    PATHS "$ENV{HOME}/.cargo/bin"
    NO_DEFAULT_PATH)
if(NOT CARGO_EXECUTABLE)
    find_program(CARGO_EXECUTABLE cargo REQUIRED)
endif()

add_custom_command(
    OUTPUT  "${RUST_LIB}"
    # Prepend ~/.cargo/bin to PATH so the rustc that cargo spawns is the
    # rustup proxy (which honours rust-toolchain.toml), not e.g. Homebrew's
    # stable rustc which would reject -Z flags.
    COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$ENV{HOME}/.cargo/bin:$ENV{PATH}"
            "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
            ${CARGO_EXECUTABLE} build
                --${RUST_PROFILE}
                --target ${RUST_TARGET}
                --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
                -Z build-std=core,alloc
    DEPENDS ${RUST_SOURCES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM)

add_custom_target(smoltcp_rust_build DEPENDS "${RUST_LIB}")
add_dependencies(${COMPONENT_LIB} smoltcp_rust_build)

# `--whole-archive` ensures Rust's #[no_mangle] FFI symbols aren't dropped.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    -Wl,--whole-archive "${RUST_LIB}" -Wl,--no-whole-archive)
