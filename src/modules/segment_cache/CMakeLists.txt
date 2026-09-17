ome_add_static_library(segment_cache
    DEPS
        bitstream
        bmff_container
        ffmpeg_wrapper
        mpegts_container
        ovlibrary
)

if(OME_BUILD_TESTS)
    file(GLOB _srcs "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
    ome_add_tests(ome_test_modules
        SRCS ${_srcs}
    )
endif()
