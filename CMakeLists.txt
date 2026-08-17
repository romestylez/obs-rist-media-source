cmake_minimum_required(VERSION 3.28)
project(obs-rist-stale-frame-filter VERSION 1.0.0 LANGUAGES C)

find_package(libobs REQUIRED)

add_library(${PROJECT_NAME} MODULE src/plugin-main.c)
target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION "obs-plugins/64bit"
  LIBRARY DESTINATION "obs-plugins/64bit")
install(DIRECTORY data/ DESTINATION "data/obs-plugins/${PROJECT_NAME}")
