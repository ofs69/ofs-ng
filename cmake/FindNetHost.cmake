# FindNetHost.cmake
#
# Finds the .NET nethost library and headers.
#
# Targets:
#   NetHost::NetHost
#
# Variables:
#   NetHost_FOUND
#   NetHost_INCLUDE_DIRS
#   NetHost_LIBRARIES

find_program(DOTNET_EXECUTABLE NAMES dotnet)

if(NOT DOTNET_EXECUTABLE)
    if(NetHost_FIND_REQUIRED)
        message(FATAL_ERROR "Could not find 'dotnet' executable. Please install the .NET SDK.")
    endif()
    return()
endif()

# Get the dotnet root directory
execute_process(
    COMMAND ${DOTNET_EXECUTABLE} --info
    OUTPUT_VARIABLE DOTNET_INFO
    ERROR_QUIET
)

if(DOTNET_INFO MATCHES "Base Path: +([^\r\n]+)")
    get_filename_component(DOTNET_SDK_PATH "${CMAKE_MATCH_1}" DIRECTORY)
    get_filename_component(DOTNET_ROOT "${DOTNET_SDK_PATH}" DIRECTORY)
endif()

if(NOT DOTNET_ROOT)
    # Fallback to common locations if --info fails or has unexpected format
    if(WIN32)
        set(DOTNET_ROOT "C:/Program Files/dotnet")
    else()
        set(DOTNET_ROOT "/usr/share/dotnet")
    endif()
endif()

# Determine Runtime ID (RID) — prefer the value reported by dotnet itself
if(DOTNET_INFO MATCHES "RID: *([^\r\n]+)")
    string(STRIP "${CMAKE_MATCH_1}" DOTNET_RID)
endif()

if(NOT DOTNET_RID)
    if(WIN32)
        set(DOTNET_RID "win-x64")
    elseif(APPLE)
        set(DOTNET_RID "osx-x64")
    else()
        set(DOTNET_RID "linux-x64")
    endif()
endif()

# Find the Host pack directory
set(NETHOST_PACK_DIR "${DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.${DOTNET_RID}")

if(EXISTS "${NETHOST_PACK_DIR}")
    # Find the latest version
    file(GLOB NETHOST_VERSIONS RELATIVE "${NETHOST_PACK_DIR}" "${NETHOST_PACK_DIR}/*")
    list(SORT NETHOST_VERSIONS ORDER DESCENDING)
    list(GET NETHOST_VERSIONS 0 NETHOST_VERSION)
    
    set(NETHOST_NATIVE_DIR "${NETHOST_PACK_DIR}/${NETHOST_VERSION}/runtimes/${DOTNET_RID}/native")
    
    find_path(NetHost_INCLUDE_DIR
        NAMES nethost.h
        PATHS "${NETHOST_NATIVE_DIR}"
    )
    
    # The app statically links nethost in Release. libnethost.lib is compiled /MT (static release) and
    # has no debug counterpart, so Debug builds would get a CRT mismatch (/MDd vs /MT). Use the import
    # lib + nethost.dll for Debug instead — no CRT boundary, identical ABI, no DLL to ship in Release.
    if(MSVC)
        set(_ofs_nethost_static "${NETHOST_NATIVE_DIR}/libnethost.lib")
        set(_ofs_nethost_import "${NETHOST_NATIVE_DIR}/nethost.lib")
        set(_ofs_nethost_dll    "${NETHOST_NATIVE_DIR}/nethost.dll")
    else()
        set(_ofs_nethost_static "${NETHOST_NATIVE_DIR}/libnethost.a")
    endif()
    if(EXISTS "${_ofs_nethost_static}")
        set(NetHost_STATIC_LIBRARY "${_ofs_nethost_static}")
    endif()
    if(MSVC AND EXISTS "${_ofs_nethost_import}")
        set(NetHost_IMPORT_LIBRARY "${_ofs_nethost_import}")
    endif()
    if(MSVC AND EXISTS "${_ofs_nethost_dll}")
        set(NetHost_RUNTIME_DLL "${_ofs_nethost_dll}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NetHost
    REQUIRED_VARS NetHost_STATIC_LIBRARY NetHost_INCLUDE_DIR
)

if(NetHost_FOUND)
    set(NetHost_INCLUDE_DIRS ${NetHost_INCLUDE_DIR})
    set(NetHost_LIBRARIES ${NetHost_STATIC_LIBRARY})

    message(STATUS "Found NetHost (static): ${NetHost_STATIC_LIBRARY}")
    if(NetHost_RUNTIME_DLL)
        message(STATUS "  DLL (Debug): ${NetHost_RUNTIME_DLL}")
    endif()
    message(STATUS "  Includes: ${NetHost_INCLUDE_DIR}")
    
    if(NOT TARGET NetHost::NetHost)
        add_library(NetHost::NetHost INTERFACE IMPORTED)
        # SYSTEM include (-isystem), like every other vendored dep: the .NET pack headers
        # (nethost.h, coreclr_delegates.h, …) are third-party and not ours to fix. Without
        # this they propagate as a plain -I, so /WX and clang-tidy (--system-headers=0)
        # treat them as our code — e.g. coreclr_delegates.h uses size_t without including
        # <cstddef> in some pack versions, which fails the build as a clang-diagnostic-error.
        set_target_properties(NetHost::NetHost PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${NetHost_INCLUDE_DIR}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${NetHost_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "NETHOST_USE_AS_STATIC"
        )
        # Release: statically link libnethost.lib (/MT). The project sets CMAKE_MSVC_RUNTIME_LIBRARY
        # to /MT for Release so the CRTs match and no nethost.dll needs to ship.
        # Debug: link the import lib instead — libnethost.lib has no /MTd counterpart and Debug
        # keeps /MDd, so the import lib (no CRT of its own) avoids the LNK2038 mismatch.
        if(MSVC AND NetHost_IMPORT_LIBRARY AND NetHost_STATIC_LIBRARY)
            set_property(TARGET NetHost::NetHost PROPERTY
                INTERFACE_LINK_LIBRARIES
                    "$<IF:$<CONFIG:Debug>,${NetHost_IMPORT_LIBRARY},${NetHost_STATIC_LIBRARY}>")
        elseif(NetHost_STATIC_LIBRARY)
            set_property(TARGET NetHost::NetHost PROPERTY
                INTERFACE_LINK_LIBRARIES "${NetHost_STATIC_LIBRARY}")
        endif()
    endif()
endif()
