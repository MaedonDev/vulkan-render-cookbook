find_package(Vulkan 1.3 REQUIRED)
find_package(glfw3 3.3 CONFIG REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(stage IN ITEMS vert frag)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.${stage}")
    set(output "${shader_dir}/triangle.${stage}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 "${source}" -o "${output}"
        DEPENDS "${source}"
        VERBATIM)
    list(APPEND shaders "${output}")
endforeach()
add_custom_target(chapter02_shaders DEPENDS ${shaders})

add_executable(chapter02_triangle main.cpp)
target_compile_features(chapter02_triangle PRIVATE cxx_std_20)
target_compile_definitions(chapter02_triangle PRIVATE "SHADER_DIR=\"${shader_dir}\"")
target_link_libraries(chapter02_triangle PRIVATE Vulkan::Vulkan glfw)
set_target_properties(chapter02_triangle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(chapter02_triangle chapter02_shaders)
