idf_component_register(
    SRCS "main.c" "doa.c" "turn.c" "ui.c"
         "fonts/font_digits_124.c" "fonts/font_word_28.c" "fonts/font_small_21.c"
    INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -ffast-math -Wno-format)
