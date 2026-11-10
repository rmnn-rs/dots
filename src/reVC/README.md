<img src="https://github.com/mrxenginner/reVC/blob/miami/res/images/logo_1024.png?raw=true" alt="reVC logo" width="200">


![screen_ 1613086852](https://user-images.githubusercontent.com/1521437/107714115-fa18f580-6ccc-11eb-9de5-eb4cd04865d3.png)
![screen_ 1613087193](https://user-images.githubusercontent.com/1521437/107714106-f4bbab00-6ccc-11eb-96a9-13821d9b9684.png)
![screen_ 1613086989](https://user-images.githubusercontent.com/1521437/107714103-f38a7e00-6ccc-11eb-88a3-c8c2033c51d6.png)
![screen_ 1613087332](https://user-images.githubusercontent.com/1521437/107714111-f84f3200-6ccc-11eb-902e-d757481d579a.png)

## Modding

Asset modifications (models, texture, handling, script, ...) should work the same way as with original GTA for the most part.

Mods that make changes to the code (dll/asi, CLEO, limit adjusters) will *not* work.
Some things these mods do are already implemented in re3 (much of SkyGFX, GInput, SilentPatch, Widescreen fix),
others can easily be achieved (increasing limis, see `config.h`),
others will simply have to be rewritten and integrated into the code directly.

## Building from Source

<details><summary>Linux Premake</summary>

For Linux using premake, proceed: [Building on Linux](https://github.com/mrxenginner/reVC/wiki/Building-on-Linux)

</details>

<details><summary>Linux Conan</summary>

Install python and conan, and then run build.
```
conan export vendor/librw librw/master@
mkdir build
cd build
conan install .. reVC/miami@ -if build -o reVC:audio=openal -o librw:platform=gl3 -o librw:gl3_gfxlib=glfw --build missing -s reVC:build_type=RelWithDebInfo -s librw:build_type=RelWithDebInfo
conan build .. -if build -bf build -pf package
```
</details>

<details><summary>FreeBSD</summary>

For FreeBSD using premake, proceed: [Building on FreeBSD](https://github.com/mrxenginner/reVC/wiki/Building-on-FreeBSD)

</details>

> :information_source: premake has an `--with-lto` option if you want the project to be compiled with Link Time Optimization.

> :information_source: There are various settings in [config.h](https://github.com/mrxenginner/reVC/tree/miami/src/core/config.h), you may want to take a look there.

> :information_source: reVC uses completely homebrew RenderWare-replacement rendering engine; [librw](https://github.com/aap/librw/). librw comes as submodule of re3, but you also can use LIBRW enviorenment variable to specify path to your own librw.
