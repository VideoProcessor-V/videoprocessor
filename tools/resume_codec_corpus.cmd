@echo off
rem Resume the real-codec corpus (planner G18): finished cases are kept, cut-off ones redone.
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg_*") do for /d %%B in ("%%D\ffmpeg-*") do set FFMPEG=%%B\bin\ffmpeg.exe
if not defined FFMPEG set FFMPEG=ffmpeg
python "%~dp0make_codec_corpus.py" D:\hfr D:\hfr\coded --ffmpeg "%FFMPEG%"
