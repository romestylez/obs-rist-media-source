cmake_minimum_required(VERSION 3.28)
project(obs-rist-media-source VERSION 2.0.1 LANGUAGES C)

include(GNUInstallDirs)

find_package(libobs REQUIRED)
find_package(Threads REQUIRED)

set(LIBRIST_ROOT "" CACHE PATH "Root of a private libRIST 0.2.20 build")
set(LIBRIST_LIBRARY "" CACHE FILEPATH "Private static libRIST library")
set(LIBRIST_EXTRA_LIBRARIES "" CACHE STRING "Additional static libraries required by libRIST")

if(LIBRIST_ROOT OR LIBRIST_LIBRARY)
  find_path(LIBRIST_INCLUDE_DIR
    NAMES librist/librist.h
    HINTS "${LIBRIST_ROOT}/include"
    REQUIRED)
  if(NOT LIBRIST_LIBRARY)
    find_library(LIBRIST_LIBRARY
      NAMES rist librist
      HINTS "${LIBRIST_ROOT}/lib"
      REQUIRED)
  endif()
  set(PRIVATE_LIBRIST_INCLUDE_DIRS "${LIBRIST_INCLUDE_DIR}")
  set(PRIVATE_LIBRIST_LIBRARIES "${LIBRIST_LIBRARY}" ${LIBRIST_EXTRA_LIBRARIES})
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBRIST REQUIRED librist=0.2.20)
  set(PRIVATE_LIBRIST_INCLUDE_DIRS ${LIBRIST_INCLUDE_DIRS})
  set(PRIVATE_LIBRIST_LIBRARY_DIRS ${LIBRIST_STATIC_LIBRARY_DIRS})
  set(PRIVATE_LIBRIST_LIBRARIES ${LIBRIST_STATIC_LINK_LIBRARIES})
  set(PRIVATE_LIBRIST_LINK_OPTIONS ${LIBRIST_STATIC_LDFLAGS_OTHER})
endif()

add_library(${PROJECT_NAME} MODULE
  src/plugin-main.c
  src/rist-media-source.c)
target_include_directories(${PROJECT_NAME} PRIVATE ${PRIVATE_LIBRIST_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${PRIVATE_LIBRIST_LIBRARY_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE
  OBS::libobs
  Threads::Threads
  ${PRIVATE_LIBRIST_LIBRARIES})
target_link_options(${PROJECT_NAME} PRIVATE ${PRIVATE_LIBRIST_LINK_OPTIONS})
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)

if(WIN32)
  target_sources(${PROJECT_NAME} PRIVATE src/obs-rist-media-source.def)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 iphlpapi bcrypt)
elseif(UNIX AND NOT APPLE)
  target_link_options(${PROJECT_NAME} PRIVATE
    "LINKER:--exclude-libs,ALL"
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/obs-rist-media-source.map")
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/obs-rist-media-source.map")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden)

if(WIN32)
  install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "obs-plugins/64bit"
    LIBRARY DESTINATION "obs-plugins/64bit")
  install(DIRECTORY data/ DESTINATION "data/obs-plugins/${PROJECT_NAME}")
elseif(UNIX AND NOT APPLE)
  install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
  install(DIRECTORY data/
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${PROJECT_NAME}")
endif()
