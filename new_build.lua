#!/usr/bin/env lua
-- Brand spanking new Lua-based cross-platform piece of dogshit build system
-- Compiling for Windows was designed to be done with mingw on Linux.
-- Compiling on Windows may exist at a specific moment in time somewhere in the future.

local function parseShell()
    local args = {}
    local opts = {}

    for i=1,#arg do
        ---@diagnostic disable-next-line: unused-local dont care
        local arg = arg[i]

        if arg:sub(1, 2) == "--" then
            local rest = arg:sub(3)
            local v = string.find(rest, "=")
            if v then
                -- opt with value
                local key = rest:sub(1, v-1)
                local val = rest:sub(v+1)
                opts[key] = val
            else
                -- bool opt
                opts[rest] = true
            end
        elseif arg:sub(1, 1) == "-" then
            local o = arg:sub(2)
            for j=1,#o do
                opts[o:sub(j, j)] = true
            end
        else
            table.insert(args, arg)
        end
    end
    args[0] = arg[0]

    return args, opts
end

---@generic T
---@param unix T
---@param windows T
---@return T
local function unixOrWindows(unix, windows)
    return package.config:sub(1, 1) == "/" and unix or windows
end

---@param fmt string
---@return string
local function fixPath(fmt, ...)
    -- we shove it into a local because Lua multivalues CAN and WILL fuck us over
    local s = fmt:format(...):gsub("%/", package.config:sub(1, 1))
    return s
end

---@param path string
---@return boolean
local function fexists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
    end
    return f ~= nil
end

---@alias LibConfig {system: string[], custom: {[string]: string[]}}

local function isStaticLib(lib)
    return lib:sub(-2) == ".a" or lib:sub(-4) == ".lib"
end

local function libName(lib)
    local dot = string.find(lib, "%.")
    if string.sub(lib, 1, 3) == "lib" then return string.sub(lib, 4, dot-1) end
    return string.sub(lib, 1, dot-1)
end

---@param lib LibConfig
---@return string
local function linkFlagsOf(lib)
    ---@type string[]
    local flags = {}

    for i=1,#lib.system do
        table.insert(flags, "-l" .. lib.system[i])
    end

    for dir, libs in pairs(lib.custom) do
        table.insert(flags, "-L" .. dir)
        for i=1,#libs do
            if isStaticLib(libs[i]) then
                table.insert(flags, "-l" .. libName(libs[i]))
            else
                table.insert(flags, "-l:" .. fixPath("%s/%s", dir, libs[i]))
            end
        end
        for k, v in pairs(libs) do
            if type(k) == "string" then
                table.insert(flags, "-l" .. v)
            end
        end
    end

    return table.concat(flags, " ")
end

local args, opts = parseShell()

local lfs = require(opts.lfs or "lfs")

local buildDir = opts.buildDir or "build"

lfs.mkdir(buildDir)

Config = {}
Config.verbose = opts.verbose or opts.v or false
Config.compiler = opts.cc or "clang" -- Default to clang cuz its cooler
Config.linker = opts.ld or Config.compiler
Config.ar = opts.ar or "ar r"
Config.ranlib = opts.ranlib
Config.mode = opts.mode or "debug" -- Debug cool
Config.mode = Config.mode:lower() -- muscle memory is a bitch sometimes
Config.opts = opts

Config.cflags = opts.cflags or "-c -fPIC"
if opts.strict then
    Config.cflags = Config.cflags .. " -Wall -Werror -Wno-unused-but-set-variable" -- we disable that warning because fucking raygui
end

Config.lflags = opts.lflags or ""

if opts.staticDir then
    Config.lflags = Config.lflags .. " -L" .. opts.staticDir
end

---@type LibConfig
Config.tscLibs = {system = {}, custom = {}}
---@type LibConfig
Config.gameLibs = {system = {}, custom = {}}

Config.host = unixOrWindows("linux", "windows")
Config.target = opts.target or Config.host
Config.targetInfo = opts.targetInfo or Config.target
assert(Config.target == "windows" or Config.target == "linux", "bad target")

if Config.target == "linux" and not Config.ranlib then
    Config.ranlib = "ranlib"
end

if Config.mode == "debug" then
    local ourFuckingDebugger = "lldb"
    if (Config.compiler:match("gcc") or Config.compiler:match("cc")) then
        ourFuckingDebugger = "gdb"
    elseif not Config.compiler:match("clang") then
        ourFuckingDebugger = "" -- just pass -g and hope
    end
    Config.cflags = Config.cflags .. " -g" .. ourFuckingDebugger
    Config.cflags = Config.cflags .. " -O0"
elseif Config.mode == "release" then
    Config.cflags = Config.cflags .. " -O3"
    if Config.compiler:match"gcc" and not Config.compiler:match"mingw" then -- mingw just sucks
        Config.cflags = Config.cflags .. " -flto=auto"
    end
    if Config.compiler:match"clang" then
        Config.cflags = Config.cflags .. " -flto=full"
    end
    if Config.linker:match"clang" then
        Config.lflags = Config.lflags .. " -flto=full"
    end
    Config.lflags = Config.lflags .. " -O3"
elseif Config.mode == "turbo" then
    Config.cflags = Config.cflags .. " -DTSC_TURBO -O3 -ffast-math"
    if Config.compiler:match"gcc" then
        Config.cflags = Config.cflags .. " -flto=auto"
    end
    if Config.compiler:match"clang" then
        Config.cflags = Config.cflags .. " -flto=full"
    end
    if Config.linker:match"clang" then
        Config.lflags = Config.lflags .. " -flto=full"
    end
    Config.lflags = Config.lflags .. " -Ofast"
elseif Config.mode == "panic" then
    Config.cflags = Config.cflags .. " -O0"
    Config.lflags = Config.lflags .. " -O0"
else
    error("bad mode")
end

if opts.singleThreaded then
    Config.cflags = Config.cflags .. " -DTSC_SINGLE_THREAD"
end

if opts.march then
    if type(opts.march) == "string" then
        Config.cflags = Config.cflags .. " -march=" .. opts.march
    else
        Config.cflags = Config.cflags .. " -march=native"
    end
end

local platformSpecificCBullshit = {
    linux = "", -- none, we cool
    windows = unixOrWindows("-I /usr/local/include", ""), -- fuck you
}

Config.cflags = Config.cflags .. " " .. platformSpecificCBullshit[Config.target]

local libs = {
    linux = "libtsc.so",
    windows = "tsc.dll",
}
local slibs = {
    linux = "libtsc.a",
    windows = "tsc.lib",
}

local exes = {
    linux = "thesandboxcell",
    windows = "thesandboxcell.exe", -- me when no +x permission flag
}

Config.lib = libs[Config.target]
if opts.static then Config.lib = slibs[Config.target] end
Config.exe = exes[Config.target]

require("targetinfo." .. Config.targetInfo)

---@param file string
---@return string
local function cacheName(file)
    local s = file:gsub("%/", ".")
    return s
end

---@param cmd string
local function exec(cmd, ...)
    cmd = cmd:format(...)
    if Config.verbose then print(cmd) end
    if not os.execute(cmd) then
        error("Command failed. Fix it.")
    end
end

---@param file string
---@return number
local function mtimeOf(file)
    local attrs = lfs.attributes(file)
    -- yeah if it errors then the last build was on January 1st, 1970, 12:00 AM GMT
    if not attrs then return 0 end
    return attrs.modification or 0
end

---@param file string
---@return string
local function compile(file)
    local buildFile = fixPath("%s/%s.o", buildDir, cacheName(file))
    local src = mtimeOf(file)
    local out = mtimeOf(buildFile)
    if (src > out) or opts.nocache then
        exec("%s %s %s -o %s", Config.compiler, Config.cflags, file, buildFile)
    else
        if Config.verbose then
            print("Skipping recompilation of " .. file .. " into " .. buildFile)
        end
    end
    return buildFile
end

---@param out string
---@param ftype "exe"|"shared"
---@param specialFlags string
---@param objs string[]
local function link(out, ftype, specialFlags, objs)
    local ltypeFlag = ftype == "shared" and "-shared" or ""
    exec("%s %s -o %s %s %s %s", Config.linker, Config.lflags, out, ltypeFlag, table.concat(objs, " "), specialFlags)
end

-- Who needs automatic source graphs when we have manual C file lists
local libtsc = {
    "src/threads/workers.c",
    "src/utils.c",
    "src/cells/cell.c",
    "src/cells/grid.c",
    "src/cells/ticking.c",
    "src/cells/subticks.c",
    "src/graphics/resources.c",
    "src/graphics/rendering.c",
    "src/graphics/ui.c",
    "src/graphics/nui.c",
    "src/saving/saving.c",
    "src/saving/saving_buffer.c",
    "src/api/api.c",
    "src/api/tscjson.c",
    "src/api/value.c",
    "src/api/modloader.c",
}

if Config.target == "windows" then
    table.insert(libtsc, "src/threads/tinycthread.c")
end

local game = {
    "src/main.c", -- wink wink
}

local function buildTheDamnLib()
    local objs = {}

    for i=1,#libtsc do
        table.insert(objs, compile(libtsc[i]))
    end

    local lib = Config.lib
    local ranlib = Config.ranlib
    local ar = Config.ar
    if lib:sub(-3) == ".so" or lib:sub(-4) == ".dll" then
        link(lib, "shared", linkFlagsOf(Config.tscLibs), objs)
    elseif lib:sub(-2) == ".a" or lib:sub(-4) == ".lib" then
        -- DOES NOT LINK LIBS!!!!!!!!!
        exec("%s %s %s", ar, lib, table.concat(objs, " "))
        if ranlib then
            exec("%s %s", ranlib, lib)
        end
    end
end

local function buildTheDamnExe()
    local objs = {}

    for i=1,#game do
        table.insert(objs, compile(game[i]))
    end

    link(Config.exe, "exe", linkFlagsOf({system = {}, custom = {["."] = {Config.lib}}}) .. " " .. linkFlagsOf(Config.gameLibs), objs)
end

-- Depends on zip existing because yes
local function packageTheDamnGame()
    local toZip = {
        Config.exe,
        Config.lib,
        "-r data",
        "CREDITS.txt",
        "LICENSE",
        "README.md",
    }

    ---@param libs LibConfig
    local function zipCWDLibs(libs)
        local cwdLibs = libs.custom["."]
        if not cwdLibs then return end -- nothing to package
        for _, lib in ipairs(cwdLibs) do
            table.insert(toZip, lib)
        end
    end

    zipCWDLibs(Config.tscLibs)
    zipCWDLibs(Config.gameLibs)

    local zip = string.format('"TheSandboxCell %s.zip"', Config.target:sub(1, 1):upper() .. Config.target:sub(2))

    exec("zip %s %s", zip, table.concat(toZip, " "))
end

args[1] = args[1] or "game"

if args[1] == "lib" then
    buildTheDamnLib()
end

if args[1] == "game" then
    if opts.background or opts.b then
        repeat
            buildTheDamnLib()
            buildTheDamnExe()
            print("Compilation finished")
            local line = io.read("l")
        until not line
    else
        buildTheDamnLib()
        buildTheDamnExe()

        if opts.bundle or opts.package then
            packageTheDamnGame()
        end
    end
end

if args[1] == "bundle" or args[1] == "package" then
    packageTheDamnGame()
end

if args[1] == "clean" then
    os.remove(Config.lib)
    os.remove(Config.exe)
    for file in lfs.dir(buildDir) do
        os.remove(fixPath("%s/%s", buildDir, file))
    end
    lfs.rmdir(buildDir)
end
