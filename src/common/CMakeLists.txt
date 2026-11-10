# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project SPDX-License-Identifier:
# GPL-2.0-or-later

if(DEFINED ENV{AZURECIREPO})
    set(BUILD_REPOSITORY $ENV{AZURECIREPO})
endif()
if(DEFINED ENV{TITLEBARFORMATIDLE})
    set(TITLE_BAR_FORMAT_IDLE $ENV{TITLEBARFORMATIDLE})
endif()
if(DEFINED ENV{TITLEBARFORMATRUNNING})
    set(TITLE_BAR_FORMAT_RUNNING $ENV{TITLEBARFORMATRUNNING})
endif()
if(DEFINED ENV{DISPLAYVERSION})
    set(DISPLAY_VERSION $ENV{DISPLAYVERSION})
endif()

include(GenerateSCMRev)

add_library(
    common STATIC
    address_space.cpp
    address_space.h
    algorithm.h
    alignment.h
    announce_multiplayer_room.h
    assert.cpp
    assert.h
    atomic_ops.h
    bit_field.h
    bit_util.h
    bounded_threadsafe_queue.h
    cityhash.cpp
    cityhash.h
    common_funcs.h
    common_types.h
    concepts.h
    container_hash.h
    core_pinning.cpp
    core_pinning.h
    demangle.cpp
    demangle.h
    device_power_state.cpp
    device_power_state.h
    div_ceil.h
    dkb_cpu_trace.cpp
    dkb_cpu_trace.h
    jit_symbols.cpp
    jit_symbols.h
    nx2_sampler.cpp
    nx2_sampler.h
    dynamic_library.cpp
    dynamic_library.h
    elf.h
    error.cpp
    error.h
    event_wait.h
    fiber.cpp
    fiber.h
    fixed_point.h
    free_region_manager.h
    fs/file.cpp
    fs/file.h
    fs/file_lock.cpp
    fs/file_lock.h
    fs/fs.cpp
    fs/fs.h
    fs/fs_paths.h
    fs/fs_types.h
    fs/fs_util.cpp
    fs/fs_util.h
    fs/path_util.cpp
    fs/path_util.h
    hash.h
    heap_tracker.cpp
    heap_tracker.h
    hex_util.cpp
    hex_util.h
    host_memory.cpp
    host_memory.h
    input.h
    intrusive_red_black_tree.h
    literals.h
    logging.cpp
    logging.h
    lz4_compression.cpp
    lz4_compression.h
    math_util.h
    memory_detect.cpp
    memory_detect.h
    multi_level_page_table.cpp
    multi_level_page_table.h
    overflow.h
    page_table.cpp
    page_table.h
    param_package.cpp
    param_package.h
    parent_of_member.h
    point.h
    range_map.h
    range_mutex.h
    range_sets.h
    range_sets.inc
    recommended_settings.cpp
    recommended_settings.h
    ring_buffer.h
    ${CMAKE_CURRENT_BINARY_DIR}/scm_rev.cpp
    scm_rev.h
    scope_exit.h
    spin_recursive_mutex.h
    scratch_buffer.h
    settings.cpp
    settings.h
    settings_common.cpp
    settings_common.h
    settings_enums.h
    settings_input.cpp
    settings_input.h
    settings_setting.h
    slot_vector.h
    socket_types.h
    sparse_large_vector.cpp
    sparse_large_vector.h
    stb.cpp
    stb.h
    steady_clock.cpp
    steady_clock.h
    stream.cpp
    stream.h
    string_util.cpp
    string_util.h
    swap.h
    thread.cpp
    thread.h
    thread_cpu_clock.cpp
    thread_cpu_clock.h
    adpf.cpp
    adpf.h
    thread_queue_list.h
    thread_worker.h
    threadsafe_queue.h
    time_zone.cpp
    time_zone.h
    tiny_mt.h
    tree.h
    typed_address.h
    uint128.h
    unique_function.h
    random.cpp
    random.h
    uuid.cpp
    uuid.h
    vector_math.h
    zbic_compression.cpp
    zbic_compression.h
    zstd_compression.cpp
    zstd_compression.h
    fs/ryujinx_compat.h fs/ryujinx_compat.cpp
    fs/symlink.h fs/symlink.cpp
    cpu_features.cpp
    cpu_features.h
    httplib.h
    net/net.h net/net.cpp
    container/unordered_map.h container/unordered_set.h)

# The ZBIC decoder runs once per module at load, so a profile buys it nothing, and the aarch64
# PGO build returns errors for NSOs the plain build decodes (#95). -fno-profile-instr-use is the
# spelling both clang and clang-cl accept; it overrides -fprofile-use from CMAKE_CXX_FLAGS.
set_source_files_properties(zbic_compression.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${ZBIC_INCLUDE_DIR}"
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-unused-function;-Wno-missing-declarations;-Wno-shadow>;$<$<CXX_COMPILER_ID:MSVC>:/wd4505;/wd5245>;$<$<CXX_COMPILER_ID:AppleClang,Clang>:-fno-profile-instr-use>")

if(WIN32)
    target_sources(common PRIVATE windows/timer_resolution.cpp
        windows/timer_resolution.h)
    target_link_libraries(common PRIVATE ntdll)
endif()

if(NOT WIN32)
    target_sources(common PRIVATE signal_chain.cpp signal_chain.h)
endif()

if(ANDROID)
    target_sources(
        common
        PUBLIC fs/fs_android.cpp
        fs/fs_android.h
        android/android_common.cpp
        android/android_common.h
        android/id_cache.cpp
        android/id_cache.h
        android/multiplayer/multiplayer.cpp
        android/multiplayer/multiplayer.h
        android/applets/software_keyboard.cpp
        android/applets/software_keyboard.h
        android/applets/web_browser.cpp
        android/applets/web_browser.h)
endif()

if(ARCHITECTURE_x86_64)
    target_sources(
        common
        PRIVATE
        x64/rdtsc.cpp
        x64/rdtsc.h
        x64/xbyak.h)
    target_link_libraries(common PRIVATE xbyak::xbyak)
endif()

if(MSVC)
    target_compile_definitions(
        common
        PRIVATE # The standard library doesn't provide any replacement for codecvt
        # yet so we can disable this deprecation warning for the time being.
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    target_compile_options(
        common
        PRIVATE /we4242 # 'identifier': conversion from 'type1' to 'type2', possible
        # loss of data
        /we4254 # 'operator': conversion from 'type1:field_bits' to
        # 'type2:field_bits', possible loss of data
        /we4800 # Implicit conversion from 'type' to bool. Possible
        # information loss
    )
else()
    set_source_files_properties(
        stb.cpp
        PROPERTIES
        COMPILE_OPTIONS
        "-Wno-implicit-fallthrough;-Wno-missing-declarations;-Wno-missing-field-initializers")

    # Get around GCC failing with intrinsics in Debug
    if(CXX_GCC AND CMAKE_BUILD_TYPE MATCHES "Debug")
        set_property(
            SOURCE stb.cpp
            APPEND
            PROPERTY COMPILE_OPTIONS ";-O2")
    endif()
endif()

if(CXX_CLANG)
    target_compile_options(common PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-fsized-deallocation>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wunreachable-code-aggressive>)
    target_compile_definitions(
        common
        PRIVATE
        # Clang 14 and earlier have errors when explicitly instantiating
        # Settings::Setting
        $<$<VERSION_LESS:$<CXX_COMPILER_VERSION>,15>:CANNOT_EXPLICITLY_INSTANTIATE>)
endif()

if (BOOST_NO_HEADERS)
    target_link_libraries(common PUBLIC Boost::algorithm Boost::heap Boost::icl Boost::pool)
else()
    target_link_libraries(common PUBLIC Boost::headers)
endif()
target_link_libraries(common PRIVATE OpenSSL::SSL)
target_link_libraries(common PUBLIC Boost::filesystem Boost::context httplib::httplib nlohmann_json::nlohmann_json)

if (lz4_ADDED)
    target_include_directories(common PRIVATE ${lz4_SOURCE_DIR}/lib)
endif()

target_link_libraries(common PUBLIC fmt::fmt stb::headers Threads::Threads)
target_link_libraries(common PRIVATE lz4::lz4 zstd::zstd)

# Please refer to src/common/demangle.cpp
if (WIN32)
    target_link_libraries(common PRIVATE LLVM::Demangle)
endif()

if(ANDROID)
    # For ASharedMemory_create
    target_link_libraries(common PRIVATE android)
endif()

# DirectStorage (Windows): async NVMe reads for game files. The Microsoft.Direct3D.DirectStorage
# NuGet package is downloaded at configure time for its headers. The runtime DLLs are not staged
# next to the executable; dstorage.dll is loaded at run time when available, and reads fall back
# to std::fread when it is missing or a request fails.
cmake_dependent_option(ENABLE_DIRECTSTORAGE "Enable DirectStorage for game file reads" ON
                       "WIN32 AND MSVC AND ARCHITECTURE_x86_64" OFF)
if (ENABLE_DIRECTSTORAGE)
    set(DIRECTSTORAGE_VERSION "1.3.0")
    set(DIRECTSTORAGE_URL
        "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.directstorage/${DIRECTSTORAGE_VERSION}/microsoft.direct3d.directstorage.${DIRECTSTORAGE_VERSION}.nupkg")
    set(DIRECTSTORAGE_SHA512
        "589a83194e9e05654523b9ca10401982d235dbaa991c8535807d5b9f851d877281417d79a0210f77b46c0f65114f805f4a12f04b7bdb3de8a00fe20c78def791")
    set(DIRECTSTORAGE_ROOT "${CMAKE_BINARY_DIR}/_deps/directstorage")
    if (NOT EXISTS "${DIRECTSTORAGE_ROOT}/native/include/dstorage.h")
        file(DOWNLOAD "${DIRECTSTORAGE_URL}" "${CMAKE_BINARY_DIR}/_deps/directstorage.nupkg"
             EXPECTED_HASH "SHA512=${DIRECTSTORAGE_SHA512}"
             STATUS directstorage_status)
        list(GET directstorage_status 0 directstorage_error)
        if (directstorage_error)
            message(FATAL_ERROR "DirectStorage SDK download failed: ${directstorage_status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/_deps/directstorage.nupkg"
             DESTINATION "${DIRECTSTORAGE_ROOT}")
    endif()
    target_sources(common PRIVATE fs/dstorage_queue.cpp fs/dstorage_queue.h)
    target_include_directories(common PUBLIC "${DIRECTSTORAGE_ROOT}/native/include")
    target_compile_definitions(common PUBLIC LICHII_DIRECTSTORAGE)
    message(STATUS "DirectStorage ${DIRECTSTORAGE_VERSION} headers: ${DIRECTSTORAGE_ROOT}/native/include")
endif()

create_target_directory_groups(common)
