newoption {
    trigger     = "with-version",
    value       = "STRING",
    description = "Current version",
}

workspace "FC2JackalFix"
   configurations { "Release", "Debug" }
   architecture "x86"
   location "build"
   cppdialect "C++latest"
   kind "SharedLib"
   language "C++"
   targetdir "bin/%{cfg.buildcfg}"
   targetextension ".asi"
   buildoptions { "/dxifcInlineFunctions-" }

   defines { "rsc_CompanyName=\"FC2JackalFix\"" }
   defines { "rsc_LegalCopyright=\"MIT license\""}
   defines { "rsc_InternalName=\"%{prj.name}\"", "rsc_ProductName=\"%{prj.name}\"", "rsc_OriginalFilename=\"%{cfg.buildtarget.name}\"" }
   defines { "rsc_FileDescription=\"Far Cry 2 Jackal Fix\"" }
   defines { "rsc_UpdateUrl=\"https://github.com/joshhhuaaa/FC2JackalFix\"" }

   local major = os.date("%d")
   local minor = os.date("%m")
   local build = os.date("%Y")
   local revision = os.date("%H") .. os.date("%M")

   if _OPTIONS["with-version"] then
      local t = {}
      for i in _OPTIONS["with-version"]:gmatch("([^.]+)") do
         t[#t + 1], _ = i:gsub("%D+", "")
      end
      while #t < 4 do t[#t + 1] = 0 end
      major    = math.min(tonumber(t[1]), 255)
      minor    = math.min(tonumber(t[2]), 255)
      build    = math.min(tonumber(t[3]), 65535)
      revision = math.min(tonumber(t[4]), 65535)
   end

   local githash = ""
   local f = io.popen("git rev-parse --short HEAD")
   if f then
      githash = f:read("*a"):gsub("%s+", "")
      f:close()
   end

   local productVersion = major .. "." .. minor .. "." .. build .. "." .. revision
   if githash ~= "" then
      productVersion = productVersion .. "-" .. githash
   end

   defines { "rsc_FileVersion_MAJOR=" .. major }
   defines { "rsc_FileVersion_MINOR=" .. minor }
   defines { "rsc_FileVersion_BUILD=" .. build }
   defines { "rsc_FileVersion_REVISION=" .. revision }
   defines { "rsc_FileVersion=\"" .. major .. "." .. minor .. "." .. build .. "\"" }
   defines { "rsc_ProductVersion=\"" .. productVersion .. "\"" }
   defines { "rsc_GitSHA1=\"" .. githash .. "\"" }
   defines { "rsc_GitSHA1W=L\"" .. githash .. "\"" }

   defines { "_CRT_SECURE_NO_WARNINGS" }

   includedirs { "source" }
   includedirs { "source/includes" }
   files { "source/*.h", "source/*.hpp", "source/*.cpp", "source/*.hxx", "source/*.ixx" }
   files { "source/resources/Versioninfo.rc" }

   includedirs { "external/hooking" }
   includedirs { "external/injector/include" }
   includedirs { "external/injector/safetyhook/include" }
   includedirs { "external/injector/zydis" }
   includedirs { "external/inireader" }
   files { "external/hooking/Hooking.Patterns.h", "external/hooking/Hooking.Patterns.cpp" }
   -- Listed one by one rather than globbed. Wildcards over the submodules produced a project with
   -- no safetyhook or zydis sources in it at all, which compiles fine and only fails at link time
   -- once something actually calls safetyhook. Explicit paths are the form known to work here.
   -- os.linux.cpp is skipped; it is #if'd out on Windows anyway.
   files { "external/injector/safetyhook/include/safetyhook.hpp" }
   files {
      "external/injector/safetyhook/src/allocator.cpp",
      "external/injector/safetyhook/src/easy.cpp",
      "external/injector/safetyhook/src/inline_hook.cpp",
      "external/injector/safetyhook/src/mid_hook.cpp",
      "external/injector/safetyhook/src/os.windows.cpp",
      "external/injector/safetyhook/src/utility.cpp",
      "external/injector/safetyhook/src/vmt_hook.cpp",
   }
   files { "external/injector/zydis/Zydis.h", "external/injector/zydis/Zydis.c" }
   files { "data/bin/plugins/*.ini" }

   links { "winmm" } -- timeBeginPeriod, for loadingscreen.ixx

   characterset ("Unicode")

   pbcommands = {
      "setlocal EnableDelayedExpansion",
      "set file=$(TargetPath)",
      "FOR %%i IN (\"%file%\") DO (",
      "set filename=%%~ni",
      "set fileextension=%%~xi",
      "set target=!path!!filename!!fileextension!",
      "if exist \"!target!\" copy /y \"!file!\" \"!target!\"",
      ")" }

   function setpaths (gamepath, exepath, pluginspath)
      pluginspath = pluginspath or "plugins/"
      if (gamepath) then
         cmdcopy = { "set \"path=" .. gamepath .. pluginspath .. "\"" }
         table.insert(cmdcopy, pbcommands)
         postbuildcommands (cmdcopy)
         debugdir (gamepath)
         if (exepath) then
            debugcommand (gamepath .. exepath)
            dir, file = exepath:match'(.*/)(.*)'
            debugdir (gamepath .. (dir or ""))
         end
      end
      targetdir ("bin")
   end

   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"
      staticruntime "On"

project "FC2JackalFix"
   setpaths("E:/Games/SteamLibrary/steamapps/common/Far Cry 2/", "bin/FarCry2.exe", "bin/plugins/")
