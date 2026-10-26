"""make_engine_package's path rewriting, for both hosts' spellings.

A Windows engine package used to keep every path as the runner wrote it
(D:/a/_temp/work/<title>/engine/...): CMake writes forward slashes, the
rewrite looked for str(Path), which has backslashes, and the check that
nothing names the build machine looked for the same thing. These run on
any host, with PureWindowsPath standing in for the Windows runner.
"""

from pathlib import PurePosixPath, PureWindowsPath

from tools.release.make_engine_package import PATH_RE, rewrite, spellings

WIN_BUILD = PureWindowsPath(r"D:\a\_temp\work\split2\engine")
WIN_SOURCE = PureWindowsPath(r"D:\a\split2-recomp\split2-recomp\xboxrecomp")

WIN_TARGETS = '''\
set_target_properties(xrengine::xbox_dsound PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "D:/a/split2-recomp/split2-recomp/xboxrecomp/src/audio"
)
set_target_properties(xrengine::xbox_dsound PROPERTIES
  IMPORTED_LOCATION_RELEASE "D:/a/_temp/work/split2/engine/src/audio/libxbox_dsound.a"
)
set_target_properties(xrengine::miniupnpc PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "D:/a/_temp/work/split2/engine/third_party/miniupnp/miniupnpc;d:/a/split2-recomp/split2-recomp/xboxrecomp/third_party/miniupnp/miniupnpc/include"
)
'''


def test_windows_paths_are_rewritten():
    out = rewrite(WIN_TARGETS, WIN_BUILD, WIN_SOURCE, 0)
    assert "D:/a" not in out and "d:/a" not in out
    assert '"${_xr_pkg}/build/src/audio/libxbox_dsound.a"' in out
    assert '"${_xr_pkg}/src/audio"' in out
    assert "${_xr_pkg}/build/third_party/miniupnp/miniupnpc;${_xr_pkg}/third_party" in out


def test_windows_check_sees_forward_slashes():
    # What the final check runs: a leftover runner path must be found.
    assert any(f.search(WIN_TARGETS) for f in spellings(WIN_BUILD))
    assert any(f.search(WIN_TARGETS) for f in spellings(WIN_SOURCE))
    out = rewrite(WIN_TARGETS, WIN_BUILD, WIN_SOURCE, 0)
    assert not any(f.search(out) for r in (WIN_BUILD, WIN_SOURCE) for f in spellings(r))


def test_path_re_keeps_the_drive():
    found = PATH_RE.findall(WIN_TARGETS)
    assert "D:/a/_temp/work/split2/engine/src/audio/libxbox_dsound.a" in found


def test_posix_paths_unchanged():
    build = PurePosixPath("/home/runner/work/_temp/work/split2/engine")
    source = PurePosixPath("/home/runner/work/split2-recomp/split2-recomp/xboxrecomp")
    text = ('IMPORTED_LOCATION_RELEASE "/home/runner/work/_temp/work/split2/engine/src/libplatform.a"\n'
            'INTERFACE_INCLUDE_DIRECTORIES "/home/runner/work/split2-recomp/split2-recomp/xboxrecomp/src"\n')
    out = rewrite(text, build, source, 1)
    assert out.startswith('get_filename_component(_xr_pkg "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)\n')
    assert '"${_xr_pkg}/build/src/libplatform.a"' in out
    assert '"${_xr_pkg}/src"' in out
    # A POSIX path is case-sensitive: a different case is another folder.
    assert spellings(build)[0].search(str(build).upper()) is None
