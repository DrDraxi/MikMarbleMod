project(MikMarbleMod)

set(TARGET MikMarbleMod)

set(${TARGET}_Sources
    "${CMAKE_CURRENT_SOURCE_DIR}/dllmain.cpp"
)

add_library(${TARGET} SHARED ${${TARGET}_Sources})
target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${TARGET} PRIVATE ImGui)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE winhttp)
