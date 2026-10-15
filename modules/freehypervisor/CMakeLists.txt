# SPDX-License-Identifier: Apache-2.0

if(CONFIG_FREEHYPERVISOR)
  zephyr_include_directories(include)
  zephyr_library()
  zephyr_library_include_directories(include)
  zephyr_library_sources(
    src/adapter.c
    src/memory.c
    src/threading.c
    src/wait.c
    src/time.c
    src/irq.c
  )

  zephyr_linker_sources(SECTIONS linker/percpu.ld)

  if(CONFIG_FREEHYPERVISOR_CORE)
    if(NOT DEFINED FREEHYPERVISOR_RUST_LIB)
      message(FATAL_ERROR "FREEHYPERVISOR_RUST_LIB must name the prepared Rust static library")
    endif()
    if(NOT EXISTS "${FREEHYPERVISOR_RUST_LIB}")
      message(FATAL_ERROR "Rust static library not found: ${FREEHYPERVISOR_RUST_LIB}")
    endif()
    # Keep the archive and its whole-archive modifiers in one compiler-driver
    # argument. Passing three independent CMake items lets Zephyr group all
    # options before all libraries, leaving Rust's unreferenced `.percpu`
    # template out of some archive layouts.
    zephyr_link_libraries(
      "-Wl,--whole-archive,${FREEHYPERVISOR_RUST_LIB},--no-whole-archive"
    )
    # The archive is intentionally passed as one linker-driver argument, so
    # CMake cannot infer it as a file dependency.  Record that dependency
    # explicitly; otherwise an incremental Cargo rebuild can leave Zephyr's
    # final ELF linked against stale Core/config/image objects.
    set_property(TARGET zephyr_interface APPEND PROPERTY
      INTERFACE_LINK_DEPENDS "${FREEHYPERVISOR_RUST_LIB}"
    )
  endif()
endif()
