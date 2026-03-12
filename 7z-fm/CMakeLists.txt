cmake_minimum_required(VERSION 3.20)
project(7z-fm VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Qt 6 ──────────────────────────────────────────────────────────────
set(CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/6.9.3/macos" CACHE PATH "Qt prefix")
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

# ── LZMA SDK (embedded) ──────────────────────────────────────────────
set(LZMA_DIR "${CMAKE_SOURCE_DIR}/lzma")
set(LZMA_SOURCES
    ${LZMA_DIR}/7zArcIn.c   ${LZMA_DIR}/7zDec.c    ${LZMA_DIR}/7zBuf.c
    ${LZMA_DIR}/7zBuf2.c    ${LZMA_DIR}/7zStream.c  ${LZMA_DIR}/7zCrc.c
    ${LZMA_DIR}/7zCrcOpt.c  ${LZMA_DIR}/CpuArch.c   ${LZMA_DIR}/LzmaDec.c
    ${LZMA_DIR}/Lzma2Dec.c  ${LZMA_DIR}/Bcj2.c      ${LZMA_DIR}/Bra.c
    ${LZMA_DIR}/Bra86.c     ${LZMA_DIR}/BraIA64.c   ${LZMA_DIR}/Delta.c
    ${LZMA_DIR}/Ppmd7.c     ${LZMA_DIR}/Ppmd7Dec.c  ${LZMA_DIR}/7zAlloc.c
    ${LZMA_DIR}/Alloc.c     ${LZMA_DIR}/7zFile.c
)
# Suppress warnings in SDK code
set_source_files_properties(${LZMA_SOURCES} PROPERTIES
    COMPILE_FLAGS "-w"
    LANGUAGE C
)

# ── Application ──────────────────────────────────────────────────────
add_executable(7z-fm
    main.cpp
    archivereader.cpp
    filepanel.cpp
    mainwindow.cpp
    ${LZMA_SOURCES}
)

target_include_directories(7z-fm PRIVATE ${LZMA_DIR})
target_compile_definitions(7z-fm PRIVATE _FILE_OFFSET_BITS=64)
target_link_libraries(7z-fm PRIVATE Qt6::Widgets Qt6::Concurrent)
