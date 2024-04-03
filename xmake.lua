

--[[
原Makefile阅读效率太低了，新增xmake编译方式
zstd相比其他几个有绝对优势，其他的压缩插件都禁用了
]]



add_rules("mode.debug", "mode.release")
add_defines("_IS_NEED_DEFAULT_CompressPlugin=0")
add_defines("_IS_NEED_DEFAULT_ChecksumPlugin=0")
add_defines("_ChecksumPlugin_xxh128=1")
add_defines("_ChecksumPlugin_crc32=1")
add_defines("_IS_NEED_DIR_DIFF_PATCH=1")
add_defines("_CompressPlugin_zstd=1")
add_defines("_IS_USED_MULTITHREAD=1")


set_languages("c17", "cxx17")

add_includedirs("./zstd/lib")
add_includedirs("./xxHash")

target("zstd")
    set_enabled(true)
    set_kind("static")

    add_files("./zstd/lib/**.c")

target_end()

target("xxHash")
    set_enabled(true)
    set_kind("static")

    add_files("./xxHash/xxhash.c")

target_end()

target("HDiffPatch")
    set_enabled(true)
    set_kind("static")

    add_deps("zstd", "xxHash")

    add_files("./HDiffPatch/libHDiffPatch/**.c")
    add_files("./HDiffPatch/libHDiffPatch/**.cpp")
    add_files("./HDiffPatch/file_for_patch.c")
    add_files("./HDiffPatch/libParallel/*.cpp")

target_end()


target("DifDiffPatch")
    set_enabled(true)
    set_kind("static")

    add_files("./HDiffPatch/dirDiffPatch/**.c")
    add_files("./HDiffPatch/dirDiffPatch/**.cpp")


target_end()

target("hsync")
    set_enabled(true)
    set_kind("static")

    add_files("./HDiffPatch/libhsync/**.cpp")


target_end()


target("zstd")
    set_enabled(true)
    set_kind("static")

    add_files("./zstd/lib/**.c")

target_end()


target("hsync_make")
    set_enabled(true)
    set_kind("binary")

    add_files("./hsync_make.cpp")
    add_files("./hsync_import_patch.cpp")
    add_files("./client_download_demo.cpp")

    add_deps("HDiffPatch", "DifDiffPatch", "hsync", "zstd")
  
target_end()

target("hsync_demo")
    set_enabled(true)
    set_kind("binary")

    add_files("./hsync_demo.cpp")
    add_files("./client_download_demo.cpp")

    add_deps("HDiffPatch", "DifDiffPatch", "hsync")
   
   
target_end()


target("minihttp")
    set_enabled(true)
    set_kind("static")

    add_files("./minihttp/*.cpp")
    
target_end()

target("hsync_http")
    set_enabled(true)
    set_kind("binary")

    add_files("./hsync_http.cpp")
    add_files("./client_download_http.cpp")

    add_deps("HDiffPatch", "DifDiffPatch", "hsync", "minihttp", "zstd")


target_end()
