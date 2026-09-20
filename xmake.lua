add_rules("mode.debug", "mode.release")
--
add_requires("libsdl2", {system = false,configs = {static = true}})  -- 声明依赖 SDL2
add_requires("libsdl2_ttf", {system = false,configs = {static = true}})  -- 声明依赖 SDL2_ttf（文本渲染）
add_requires("libsdl2_mixer", {system = false,configs = {static = true,flac = false}})  -- 声明依赖 SDL2_mixer（音频播放/MP3 解码；禁用 FLAC 以规避系统 libflac cmake 配置问题，MP3 由内置 dr_mp3 解码）
-- 图像解码/编码：统一走 SDL2_image，不再依赖系统 libpng/libjpeg。
-- 原因：add_syslinks("png16","jpeg") 里那两个库名是 **Unix 专有**的
-- （Debian/Arch 叫 png16/libjpeg，Windows 上既没有 "png16" 也没有 "jpeg"
--  这个库名，MSVC 下直接找不到），且要求使用者先 apt 装 -dev 包。
-- SDL2_image 自带 libpng/libjpeg/zlib（external/ 目录），由 xmake 统一
-- 下载编译，各平台一致。
add_requires("libsdl2_image", {system = false,configs = {static = true}})
add_requires("unicorn", {system = false,configs = {static = true,archs = {"arm"}}})-- 声明依赖 unicorn 库
add_requires("capstone", {system = false,configs = {static = true}}) -- 声明依赖 capstone 库


target("zm_emu")
    set_kind("binary")
--    
    -- xmake run 默认在二进制所在目录启动，而 applet 资源相对项目根存放；
    -- 这里把运行目录设回项目根，使相对路径 applet/... 始终可解析。
    set_rundir(os.projectdir())
--    
    add_files("src/*.c")        
    add_files("src/**/*.c")
    -- 
    set_languages("c23") -- 指定使用 C23 标准


--    
    -- ulibc：include/ 放各模块头（各 .c 用 #include "u_xxx.h" 引用），
    -- 根目录放总入口 ulibc.h，两个路径都要能搜到
    add_includedirs("src/ulibc/include", "src/ulibc")
--    
    -- add_defines("LOG_USE_COLOR")  -- <--- 添加这一行来启用颜色输出 log/log.h
    -- 重复符号容忍（宿主 libc 与 ulibc 有同名符号）。GNU ld / Apple ld64 /
    -- MSVC 三个链接器的写法各不相同，按平台选，否则换平台直接链接失败。
    if is_plat("macosx") then
        add_ldflags("-Wl,-multiply_defined,suppress")
    elseif is_plat("windows") then
        add_ldflags("/FORCE:MULTIPLE", {force = true})
    else
        add_ldflags("-Wl,--allow-multiple-definition")
    end

    -- PNG/JPEG 编解码已由 libsdl2_image 提供（自带 libpng/libjpeg/zlib），
    -- 这里不再 add_syslinks("png16","jpeg","z")。
    -- libm 只有类 Unix 需要单独链（MSVC/MINGW 的数学函数在 CRT 里）。
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("m")
    end

-- 
    add_packages("libsdl2") -- 链接 SDL2 库
    add_packages("libsdl2_ttf") -- 链接 SDL2_ttf 库
    add_packages("libsdl2_mixer") -- 链接 SDL2_mixer 库
    add_packages("libsdl2_image") -- 链接 SDL2_image 库（PNG/JPEG 解码）
    add_packages("unicorn") -- 链接 unicorn 库
    add_packages("capstone") -- 链接 capstone 库

    -- ============================================================
    -- 4. 编译优化选项
    -- ============================================================
    set_optimize("smallest") -- 大小优化
    set_strip("all")         -- 链接时去除所有符号

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

