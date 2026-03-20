FILE(GLOB app_sources
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "."
    REQUIRES fatfs sdmmc)
