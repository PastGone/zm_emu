add_rules("mode.debug", "mode.release")
--
add_requires("libsdl", {configs = {static = true}})  -- 声明依赖 SDL2
add_requires("libsdl2_ttf", {configs = {static = true}})  -- 声明依赖 SDL2_ttf（文本渲染）
add_requires("libsdl2_mixer", {configs = {static = true,flac = false}})  -- 声明依赖 SDL2_mixer（音频播放/MP3 解码；禁用 FLAC 以规避系统 libflac cmake 配置问题，MP3 由内置 dr_mp3 解码）
add_requires("unicorn", {configs = {static = true,archs = {"arm"}}})-- 声明依赖 unicorn 库
add_requires("capstone", {configs = {static = true}}) -- 声明依赖 capstone 库
add_requires("libpng", {configs = {static = true}})   -- PNG 解码（截图功能）
add_requires("libjpeg-turbo", {configs = {static = true}})  -- JPEG 解码


target("zm_emu")
    set_kind("binary")
--    
    add_files("src/*.c")        
    add_files("src/**/*.c")
--    
    -- add_defines("LOG_USE_COLOR")   -- 启用颜色输出 log/log.h
    add_defines("HAVE_PNG")        -- 启用 PNG 解码支持（zm_layer 截图）
    add_defines("HAVE_JPEG")       -- 启用 JPEG 解码支持（zm_layer）
-- 
    add_packages("libsdl2")        -- 链接 SDL2 库
    add_packages("libsdl2_ttf")    -- 链接 SDL2_ttf 库
    add_packages("libsdl2_mixer")  -- 链接 SDL2_mixer 库
    add_packages("unicorn")        -- 链接 unicorn 库
    add_packages("capstone")       -- 链接 capstone 库
    add_packages("libpng")         -- 链接 libpng
    add_packages("libjpeg-turbo")  -- 链接 libjpeg-turbo

    -- ============================================================
    -- 4. 编译选项（对应原 Makefile）
    -- ============================================================
    set_languages("gnu11")         -- C 标准：gnu11
    set_warnings("all")            -- 开启所有警告
    add_cxflags("-Wno-unused-parameter", "-Wno-unused-function")  -- 抑制未使用参数/函数警告
    set_optimize("smallest")       -- 大小优化（-Os 等价）
    set_strip("all")               -- 链接时去除所有符号

--
-- If you want to known more usage about xmake, please see https://xmake.io
--
-- ## FAQ
--
-- You can enter the project directory firstly before building project.
--
--   $ cd projectdir
--
-- 1. How to build project?
--
--   $ xmake
--
-- 2. How to configure project?
--
--   $ xmake f -p [macosx|linux|iphoneos ..] -a [x86_64|i386|arm64 ..] -m [debug|release]
--
-- 3. Where is the build output directory?
--
--   The default output directory is `./build` and you can configure the output directory.
--
--   $ xmake f -o outputdir
--   $ xmake
--
-- 4. How to run and debug target after building project?
--
--   $ xmake run [targetname]
--   $ xmake run -d [targetname]
--
-- 5. How to install target to the system directory or other output directory?
--
--   $ xmake install
--   $ xmake install -o installdir
--
-- 6. Add some frequently-used compilation flags in xmake.lua
--
-- @code
--    -- add debug and release modes
--    add_rules("mode.debug", "mode.release")
--
--    -- add macro definition
--    add_defines("NDEBUG", "_GNU_SOURCE=1")
--
--    -- set warning all as error
--    set_warnings("all", "error")
--
--    -- set language: c99, c++11
--    set_languages("c99", "c++11")
--
--    -- set optimization: none, faster, fastest, smallest
--    set_optimize("fastest")
--
--    -- add include search directories
--    add_includedirs("/usr/include", "/usr/local/include")
--
--    -- add link libraries and search directories
--    add_links("tbox")
--    add_linkdirs("/usr/local/lib", "/usr/lib")
--
--    -- add system link libraries
--    add_syslinks("z", "pthread")
--
--    -- add compilation and link flags
--    add_cxflags("-stdnolib", "-fno-strict-aliasing")
--    add_ldflags("-L/usr/local/lib", "-lpthread", {force = true})
--
-- @endcode
--

