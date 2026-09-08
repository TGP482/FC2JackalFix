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
   buildoptions { "/dxifcInlineFunctions-" }

   defines { "rsc_CompanyName=\"FC2JackalFix\"" }
   defines { "rsc_LegalCopyright=\"MIT license\""}
   defines { "rsc_InternalName=\"%{prj.name}\"", "rsc_ProductName=\"%{prj.name}\"", "rsc_OriginalFilename=\"%{cfg.buildtarget.name}\"" }
   defines { "rsc_FileDescription=\"Far Cry 2 Jackal Fix\"" }
   defines { "rsc_UpdateUrl=\"https://github.com/TGP482/FC2JackalFix\"" }

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

   -- -- Local builds show DEV, CI releases use the actual version
   local nRelease = 2
   local szVersion = _OPTIONS["with-version"] and ("V" .. major) or ("V" .. nRelease .. " DEV")

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
   defines { "rsc_Version=\"" .. szVersion .. "\"" }
   defines { "rsc_VersionW=L\"" .. szVersion .. "\"" }

   defines { "_CRT_SECURE_NO_WARNINGS" }

   includedirs { "source" }
   includedirs { "source/includes" }
   files { "source/**.h", "source/**.hpp", "source/**.cpp", "source/**.hxx", "source/**.ixx" }
   files { "source/resources/Versioninfo.rc" }

   -- Pattern scanning is the one dependency both builds share: it only reads the image, so there is
   -- nothing in it for a loader to arbitrate.
   includedirs { "external/hooking" }
   files { "external/hooking/Hooking.Patterns.h", "external/hooking/Hooking.Patterns.cpp" }

   links { "winmm" } -- timeBeginPeriod, for loadingscreen.ixx

   characterset ("Unicode")

   pbcommands = {
      "for %%P in (\"!JFDIR!.\") do set \"JFDIR=%%~fP\"",
      -- Parentheses keep both SET commands inside the FOR loop body.
      "for %%S in (\"$(TargetPath)\") do (set \"JFSRC=%%~fS\" & set \"JFNAME=%%~nxS\")",
      "set \"JFDST=!JFDIR!\\!JFNAME!\"",
      -- No game install found, skip
      "if not exist \"!JFDIR!\\\" goto :JFDONE",
      "if /I \"!JFSRC!\"==\"!JFDST!\" goto :JFDONE",
      -- Loud on failure. The game holds the asi open while it runs, so the copy silently did
      -- nothing and the build still reported success, leaving the previous asi in place. That is
      -- what a fix that works one run and not the next looks like.
      "copy /y \"!JFSRC!\" \"!JFDST!\" >nul || (echo FC2JackalFix: could not replace \"!JFDST!\", close the game and build again & endlocal & exit /b 1)",
      ":JFDONE",
      "endlocal",
      "exit /b 0" }

   function setpaths (gamepath, exepath, pluginspath)
      pluginspath = pluginspath or "plugins/"
      if (gamepath) then
         -- Keep deployment variables local to this build step
         local cmdcopy = {
            "setlocal EnableExtensions EnableDelayedExpansion",
            "set \"JFDIR=" .. (gamepath .. pluginspath):gsub("([^/\\])$", "%1/") .. "\"",
         }
         for _, cmd in ipairs(pbcommands) do
            table.insert(cmdcopy, cmd)
         end
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

   -- Cleared before the projects, or everything below would inherit the Release filter.
   filter {}

local gamedir = os.getenv("FC2DIR") or "C:/Program Files (x86)/Steam/steamapps/common/Far Cry 2/"

-- The stock build: an ASI plugin loaded by Ultimate ASI Loader, hooking and patching for itself.
project "FC2JackalFix"
   targetextension ".asi"

   includedirs { "external/injector/include" }
   includedirs { "external/injector/safetyhook/include" }
   includedirs { "external/injector/zydis" }
   includedirs { "external/inireader" }

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

   -- The FCSE build's own entry point and settings module. Both declare things this one already
   -- has - a second `export module settings` in one project does not even compile.
   removefiles { "source/fcse/**" }

   setpaths(gamedir, "bin/FarCry2.exe", "bin/plugins/")

-- The same fixes as a plugin for FCSE, which owns the process and arbitrates hooks between mods.
-- Everything under source/ is shared; what differs is the entry point, where settings live, and
-- that hooking goes through the loader. See README.md, "Running under FCSE".
project "FC2JackalFix-FCSE"
   targetname "FC2JackalFix"
   targetextension ".dll"

   defines { "JACKALFIX_FCSE" }

   -- The shim headers stand in for injector, safetyhook and IniReader under the names common.hxx
   -- already includes, so no feature module changes to reach FCSE instead.
   includedirs { "source/fcse/shim" }
   includedirs { "external/fcse" }

   removefiles {
      "source/dllmain.cpp",              -- fcse_main.cpp is the entry point here
      "source/core/settings.ixx",        -- source/fcse/settings.ixx registers with FCSE instead
      "source/core/updatecheck.ixx",     -- it offers the ASI release, which is not what is installed
      "source/system/largeaddressaware.ixx", -- FCSE.exe is linked large address aware already
      "source/ui/jackalfixmenu.ixx",     -- FCSE's Mod Configuration Menu carries the rows
   }

   setpaths(gamedir, "bin/FCSE.exe", "bin/plugins/")
