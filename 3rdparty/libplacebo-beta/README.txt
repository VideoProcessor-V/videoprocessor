Fork: https://github.com/billslack2/libplacebo
Branch: codex/vp-master-beta
Commit: 8bccd6578c9e845f77205cec9e85fdb97bf36ed7
Upstream: 1dd022687b9ac1dd996855b51433bf770ae76aab
API: 377
 74d8a6cb930c68ef617b202c3ff3c59d919e086b 3rdparty/Vulkan-Headers (v1.4.345)
 97b54ca9e75f5303507699d27c6b4f4efe4641a1 3rdparty/fast_float (v6.1.0-275-g97b54ca)
 73db193f853e2ee079bf3ca8a64aa2eaf6459043 3rdparty/glad (v0.1.11a-302-g73db193)
 15206881c006c79667fe5154fe80c01c65410679 3rdparty/jinja (3.1.6)
 297fc8e356e6836a62087949245d09a28e9f1b13 3rdparty/markupsafe (3.0.3)
 242f35efa067a46c595645eeda7b1771ea1f83b1 demos/3rdparty/nuklear (4.12.8)

Build: MSYS2 mingw64 GCC 16.1.0; Meson 1.11.2; clean x64 Release.
D3D11, shaderc, lcms2 and libdovi enabled; Vulkan/OpenGL backends disabled.
All 13 library tests passed. See VP-FORK.md in the complete source archive.

Binary SHA256: 19023e283d1ff21a3b5a13e2754e091cc34231dc06c8a0818c5b75953dcb3722
Source archive SHA256: fa3acffa6266091520cbdb9becc18e672b109cd5a3b13a85f8984130489a68f4

All eight transitive DLLs remain byte-identical to the stable 7.360.1 bundle.
Stable and Beta have distinct DLL names, import libraries, headers and shader caches.
Gamut overflow correction (Experimental) defaults Off. It follows the original
gamut mapper and replaces the old fixed-Y/IPT LUT experiment. It may reduce
brightness or saturation. Uses the existing bool API and no environment switch.

FL35 build (2026-10-11): the libplacebo-377.dll in bin is NOT Bill's binary.
It is the same source (8bccd657, the archive above) with the two FL35
output-clamp patches in source/fl35-patches:
 0001 colorspace: use the luma limits for all channels of limited range RGB
 0002 colorspace: keep footroom and headroom in the limited range output clamp
Limited range is clamped to the legal codes (8-bit 1-254, 10-bit 4-1019),
identically on R, G and B, instead of 16-235/240 (which cut below-black and
above-white and tinted super-whites cyan).
Build: MSYS2 mingw64 GCC 16.2.0; Meson 1.12.1; options as in VP-FORK.md.
All 13 library tests passed. Exports (514) and imports are identical to
Bill's binary; every runtime symbol it needs is in the bundled runtime DLLs,
which are unchanged.
Binary SHA256: 8f63be99b2d8d65e1e08436178e9ae8fd0bd9a55a2dde9b6985a5f8f10c5e653
Complete patched source: source/libplacebo-7.377.0-vp-fl35-source.zip
Source archive SHA256: 92ffa9c053508d1fda4e613b7a328a240b31417c2c75bc62e48195cbd33ad5ea
GPU check: src/VideoProcessor-BetaTests (BetaGpuTests.cpp): code 40 -> 40 and
980 -> 980 on a 10-bit limited target (Bill's binary: 64 and 940/960/960).
This libplacebo applies the output encoding after PRE_OUTPUT hooks itself, so
VP's OutputEncodingRepair is off for the Beta (PL_API_VER >= 377).

Fork-only additions since (source/fl35-patches; any libplacebo rebuild must carry 0001-0004):
 0003 renderer: optional rgba32f FBOs (output_fbo_float32, fbo_float32; VP S2)
 0004 custom_mpv: pl_mpv_user_shader_set_texture (2026-10-12; VP delta 8 R1): a
      user shader's own //!TEXTURE is pointed at a caller texture (the sigma-delta's
      VPVEC vector field) and restored before destroy. One new export (515).
Build (0004): MSYS2 mingw64, options as in VP-FORK.md; all 13 library tests passed.
lib/libplacebo-377.lib regenerated from lib/libplacebo-377.def (MSVC lib.exe /def).
On every rebuild: add each fork export (0004: pl_mpv_user_shader_set_texture) to
lib/libplacebo-377.def and regenerate the import library with
  lib.exe /nologo /def:libplacebo-377.def /machine:x64 /out:libplacebo-377.lib
or VP's Beta renderer will not link (the export would be missing from the .lib).
Binary SHA256 (with 0001-0004): 15809853816eb8be355435504a531a7f73b92f4b68387d09419513cb5b509220
