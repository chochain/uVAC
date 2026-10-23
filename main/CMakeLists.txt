# main/CMakeLists.txt (Main Component)

# 1. Define your source paths relative to this component directory
set(SOURCES
    "src/main_repl.cpp"
    "src/ceforth.cpp"
    "src/ceforth_sys.cpp"
    "src/ceforth_task.cpp"
    "src/e4_run.S"
)
set(SOURCES1
    "src/esp32/xserver2.cpp"
    "src/esp32/xforth.cpp"
    "src/esp32/xgl_actor.cpp"
)

# 2. Register the component with ESP-IDF
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS
    "."
    "src"
    "src/esp32"
    REQUIRES lwip nvs_flash esp_http_server  # Add required hardware/network components here
    )

# 3. Port custom compiler flags from your original Makefile
# NOTE: ESP-IDF automatically handles cross-compiling architectures, -std=gnu++17, and standard optimizations.
# We append your specialized performance flags below.
target_compile_options(${COMPONENT_LIB} PRIVATE
  -O2
  -DXT0_U32=1
  -DE4_TRAMP=1
  -DE4_RTOP=1
  -fomit-frame-pointer
  -fno-stack-check
  -fno-stack-protector
  -ffast-math
  -funroll-loops
  )

set_source_files_properties(ceforth.cpp PROPERTIES COMPILE_OPTIONS "-O2;-fdump-tree-optimized")

