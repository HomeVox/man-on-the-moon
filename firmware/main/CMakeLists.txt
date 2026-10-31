file(GLOB_RECURSE MY_SRCS
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "assets/*.c"
    "assets/*.cc"
    "assets/*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
)

set(MY_INCS
    "."
)

idf_component_register(
    SRCS 
        "main.cpp"
        ${MY_SRCS}
    INCLUDE_DIRS 
        ${MY_INCS}
    EMBED_FILES
        "assets/sfx/boot_sfx.bin"
        "assets/earth/aarde_dag.jpg"
        "assets/earth/aarde_nacht.jpg"
        "assets/moon/maan.jpg"
        "assets/planets/mars.jpg"
        "assets/planets/jupiter.jpg"
)

# Kantoor: LVGL-geheugen uit PSRAM (hal/lv_mem_psram.c) altijd meelinken
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
