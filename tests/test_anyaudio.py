"""Drives anyaudio.lua inside a headless libmpv created the way embedding apps do (config=no).

Usage: python tests/test_anyaudio.py <path to libmpv-2.dll / libmpv.so>   (needs ffmpeg on PATH)
With mkvmerge on PATH it also runs the command the mkvmerge key builds and checks the file.
"""
import ctypes, json, os, shutil, subprocess, sys, tempfile, time

lib_path = os.path.abspath(sys.argv[1])
if hasattr(os, "add_dll_directory"):
    os.add_dll_directory(os.path.dirname(lib_path))
mpv = ctypes.CDLL(lib_path)
mpv.mpv_create.restype = ctypes.c_void_p
mpv.mpv_get_property_string.restype = ctypes.c_char_p
for fn in ("mpv_set_option_string", "mpv_initialize", "mpv_command_string", "mpv_get_property_string", "mpv_set_property_string", "mpv_terminate_destroy"):
    getattr(mpv, fn).argtypes = [ctypes.c_void_p] + [ctypes.c_char_p] * {"mpv_set_option_string": 2, "mpv_command_string": 1, "mpv_get_property_string": 1, "mpv_set_property_string": 2}.get(fn, 0)

tmp = tempfile.mkdtemp(prefix="anyaudio-")
video, dub, state = (os.path.join(tmp, n).replace("\\", "/") for n in ("video.mp4", "dub.mp4", "state.txt"))
ff = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi"]
subprocess.run(ff + ["-i", "testsrc2=s=1280x720:r=24:d=10", "-f", "lavfi", "-i", "sine=f=220:d=10",
                     "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", video], check=True)
subprocess.run(ff + ["-i", "testsrc2=s=640x360:r=24:d=10", "-f", "lavfi", "-i", "sine=f=440:d=10",
                     "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", dub], check=True)

h = mpv.mpv_create()
script = os.path.join(os.path.dirname(__file__), "..", "anyaudio.lua")
for k, v in {"config": "no", "vo": "null", "ao": "null", "idle": "yes", "pause": "yes",
             "scripts": os.path.abspath(script), "script-opts": "anyaudio-state_file=" + state}.items():
    assert mpv.mpv_set_option_string(h, k.encode(), v.encode()) == 0, k
assert mpv.mpv_initialize(h) == 0


def cmd(line):
    assert mpv.mpv_command_string(h, line.encode()) == 0, line


def tracks():
    return json.loads(mpv.mpv_get_property_string(h, b"track-list") or b"[]")


def wait(cond, what):
    end = time.time() + 15
    while time.time() < end:
        if cond():
            return
        time.sleep(0.1)
    raise AssertionError(f"timeout: {what}; tracks={tracks()}")


def same(a, b):  # mpv reports Windows paths with backslashes
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


def load(path):
    cmd(f'loadfile "{path}"')
    wait(lambda: same((mpv.mpv_get_property_string(h, b"path") or b"").decode(), path) and tracks(), f"load {path}")


load(video)
cmd("script-binding anyaudio/apply")  # nothing grabbed yet: must not add anything
time.sleep(1)
assert not [t for t in tracks() if t.get("external")]

load(dub)
cmd("script-binding anyaudio/grab")
wait(lambda: os.path.exists(state) and same(open(state).read(), dub), "grab writes the state file")

load(video)
cmd("script-binding anyaudio/apply")
wait(lambda: [t for t in tracks() if t["type"] == "audio" and t.get("external") and t.get("selected")], "external audio selected")
wait(lambda: not [t for t in tracks() if t["type"] == "video" and t.get("external")], "external video removed")
assert [t for t in tracks() if t["type"] == "video" and t.get("selected")][0]["demux-w"] == 1280

cmd("set audio-delay 0.5")
clipboard = lambda: (mpv.mpv_get_property_string(h, b"clipboard/text") or b"").decode()
saved = clipboard()  # put back below: running the test must not eat the user's clipboard
cmd("script-binding anyaudio/mkvmerge")
wait(lambda: clipboard().startswith("mkvmerge "), "mkvmerge command on the clipboard")
line = clipboard()
mpv.mpv_set_property_string(h, b"clipboard/text", saved.encode())
q = (lambda s: f'"{s}"') if os.name == "nt" else (lambda s: f"'{s}'")
out = video[:-4] + ".anyaudio.mkv"
# replace(): mpv 0.40+ reports Windows paths with backslashes, older builds as loaded
assert line.replace("\\", "/") == (f"mkvmerge -o {q(out)} --default-track-flag 1:0 {q(video)} -D -S -M --no-chapters"
                                   f" --no-global-tags -a 1 --sync 1:500 --default-track-flag 1:1 {q(dub)}"), line
if shutil.which("mkvmerge"):
    subprocess.run(line, shell=True, check=True, stdout=subprocess.DEVNULL)
    made = json.loads(subprocess.run(["mkvmerge", "-J", out], capture_output=True, check=True).stdout)["tracks"]
    assert [(t["type"], t["properties"]["default_track"]) for t in made[1:]] == [("audio", False), ("audio", True)], made
    assert abs(made[2]["properties"]["minimum_timestamp"] / 1e6 - 500) < 50, made[2]  # positive delay = dub plays later
else:
    print("mkvmerge not on PATH: command checked, not run")

cmd("script-binding anyaudio/clear")
wait(lambda: not [t for t in tracks() if t.get("external")], "clear removes the added tracks")

mpv.mpv_terminate_destroy(h)
print("ok: grab / apply / mkvmerge / clear under config=no")
