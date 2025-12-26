cmake_minimum_required(VERSION 3.10)
project(FastFileSearch)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(FastFileSearch WIN32
    main.cpp
    MFTReader.cpp
    MFTReader.h
    NtfsStructs.h
    FatReader.cpp
    FatReader.h
    FatStructs.h
    exFatReader.cpp
    exFatReader.h
    exFatStructs.h
    resource.h
    FastFileSearch.rc
    app.manifest
)

target_link_libraries(FastFileSearch
    comctl32
    comdlg32
    shell32
    ole32
    user32
    kernel32
    advapi32
)

target_compile_definitions(FastFileSearch PRIVATE 
    UNICODE 
    _UNICODE 
    _CRT_SECURE_NO_WARNINGS
)

target_link_options(FastFileSearch PRIVATE 
    /MANIFEST:NO
)

if(MSVC)
    target_compile_options(FastFileSearch PRIVATE /W4 /EHsc /utf-8)
endif()

add_executable(test_console
    test_console.cpp
    MFTReader.cpp
    MFTReader.h
    NtfsStructs.h
    FatReader.cpp
    FatReader.h
    FatStructs.h
    exFatReader.cpp
    exFatReader.h
    exFatStructs.h
)
target_link_libraries(test_console
    kernel32
    user32
)
target_compile_definitions(test_console PRIVATE 
    UNICODE 
    _UNICODE 
    _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
    target_compile_options(test_console PRIVATE /W4 /EHsc /utf-8)
endif()
target_link_options(test_console PRIVATE 
    /MANIFEST:NO
)
