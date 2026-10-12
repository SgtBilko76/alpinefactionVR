
if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(SHADER_COMPILER shader_compiler)
else()
    # Wine's builtin d3dcompiler (vkd3d-shader) emits ~20x larger, unrolled
    # bytecode that stalls/hangs mobile GPUs (Quest via DXVK/Turnip). Point
    # AF_NATIVE_D3DCOMPILER at a Microsoft x86 d3dcompiler_47.dll to use it.
    set(AF_NATIVE_D3DCOMPILER "" CACHE FILEPATH "Microsoft x86 d3dcompiler_47.dll used for shader builds under Wine")
    if (AF_NATIVE_D3DCOMPILER)
        configure_file(${AF_NATIVE_D3DCOMPILER} ${CMAKE_BINARY_DIR}/bin/d3dcompiler_43.dll COPYONLY)
        set(SHADER_COMPILER ${CMAKE_COMMAND} -E env WINEDLLOVERRIDES=d3dcompiler_43=n WINEDEBUG=-all
            wine ${CMAKE_BINARY_DIR}/bin/shader_compiler.exe)
    else()
        message(WARNING "AF_NATIVE_D3DCOMPILER not set: shaders are compiled with Wine's d3dcompiler")
        set(SHADER_COMPILER wine ${CMAKE_BINARY_DIR}/bin/shader_compiler.exe)
    endif()
endif()

function(add_shader TARGET_NAME SRC_FILE TARGET_LEVEL)
    set(SHADER_OUTPUT ${CMAKE_BINARY_DIR}/shaders/${TARGET_NAME}.bin)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${SHADER_COMPILER} -O3 -t ${TARGET_LEVEL} ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/${SRC_FILE} -o ${SHADER_OUTPUT}
        MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/${SRC_FILE}
    )
    add_custom_target(${TARGET_NAME} ALL DEPENDS ${SHADER_OUTPUT})
    add_dependencies(${TARGET_NAME} shader_compiler)
endfunction()

add_shader(standard_vs standard_vs.hlsl vs_4_0)
add_shader(character_vs character_vs.hlsl vs_4_0)
add_shader(transformed_vs transformed_vs.hlsl vs_4_0)
add_shader(shadow_standard_vs shadow_standard_vs.hlsl vs_4_0)
add_shader(shadow_character_vs shadow_character_vs.hlsl vs_4_0)

add_shader(outline_character_vs outline_character_vs.hlsl vs_4_0)
add_shader(outline_standard_vs outline_standard_vs.hlsl vs_4_0)

add_shader(standard_ps standard_ps.hlsl ps_4_0)
add_shader(standard_nogas_ps standard_ps.hlsl ps_4_0 -DMAX_GAS_REGIONS=0)
add_shader(ui_ps ui_ps.hlsl ps_4_0)
add_shader(shadow_debug_ps shadow_debug_ps.hlsl ps_4_0)
add_shader(outline_ps outline_ps.hlsl ps_4_0)

add_shader(gamma_vs gamma_vs.hlsl vs_4_0)
add_shader(gamma_ps gamma_ps.hlsl ps_4_0)
