# main/CMakeLists.txt (Main Component)

# 1. Define your source paths relative to this component directory
set(SOURCES
    "main.cpp"
    "ceforth.cpp"
    "ceforth_sys.cpp"
    "ceforth_task.cpp"
)
set(SOURCES1
    "main.cpp"
    "ceforth.cpp"
    "ceforth_sys.cpp"
    "ceforth_task.cpp"
    "esp32/xserver2.cpp"
    "esp32/xforth.cpp"
    "esp32/xgl_actor.cpp"
)

# 2. Register the component with ESP-IDF
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "." "esp32"
    REQUIRES lwip nvs_flash esp_http_server  # Add required hardware/network components here
)

# 3. Port custom compiler flags from your original Makefile
# NOTE: ESP-IDF automatically handles cross-compiling architectures, -std=gnu++17, and standard optimizations.
# We append your specialized performance flags below.
target_compile_options(${COMPONENT_LIB} PRIVATE
  -O2
  -fomit-frame-pointer
  -fno-stack-check
  -fno-stack-protector
  -ffast-math
  -funroll-loops
)
