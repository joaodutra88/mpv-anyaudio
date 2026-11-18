-- anyaudio: watch the video of one source with the audio of another.
-- grab  (F6): on the source whose audio you want, remember its URL
-- apply (F7): on the source whose video you want, add that audio and select it
-- clear (F8): drop the added tracks and go back to the source's own audio
-- mkvmerge (Ctrl+F7): copy the mkvmerge command that makes the current result a file

local options = require "mp.options"
local utils = require "mp.utils"

local o = {
    -- A file, not a variable: some hosts (Nuvio) create a fresh mpv for every playback.
    state_file = (os.getenv("TEMP") or os.getenv("TMPDIR") or "/tmp") .. "/mpv-anyaudio.txt",
}
options.read_options(o, "anyaudio")

local applied -- URL whose tracks this script added to the current file

local function say(text, seconds)
    mp.osd_message("anyaudio: " .. text, seconds or 4)
    mp.msg.info(text)
end

local function saved_url()
    local f = io.open(o.state_file, "r")
    if not f then return nil end
    local url = f:read("*a")
    f:close()
    return url ~= "" and url or nil
end

local function tracks_from(url)
    local found = {}
    for _, t in ipairs(mp.get_property_native("track-list", {})) do
        if t.external and t["external-filename"] == url then found[#found + 1] = t end
    end
    return found
end

local function grab()
    local path = mp.get_property("path")
    if not path then return say("nothing is playing") end
    local f, err = io.open(o.state_file, "w")
    if not f then return say("cannot write " .. o.state_file .. ": " .. err, 6) end
    f:write(path)
    f:close()
    say("audio source saved: " .. (mp.get_property("filename") or path))
end

local function apply()
    local url = saved_url()
    if not url then return say("no audio source saved: use grab on the source with the audio you want", 6) end
    if url == mp.get_property("path") then return say("this is the saved source: open the other one first", 6) end
    if applied == url then return say("audio already applied") end
    say("loading audio...", 30)
    -- async: opening a remote file can take seconds and would freeze playback
    mp.command_native_async({ "audio-add", url, "select" }, function(ok, _, err)
        if not ok then return say("could not load audio: " .. tostring(err), 6) end
        applied = url
        -- audio-add opens the whole file; its video must not become a selectable track
        for _, t in ipairs(tracks_from(url)) do
            if t.type == "video" then mp.commandv("video-remove", t.id) end
        end
        say("audio now from " .. url:match("[^/\\]*$"))
    end)
end

local function clear()
    if not applied then return say("nothing to remove") end
    for _, t in ipairs(tracks_from(applied)) do mp.commandv(t.type .. "-remove", t.id) end
    applied = nil
    mp.set_property_number("audio-delay", 0)
    mp.set_property("aid", "auto")
    say("back to this source's own audio")
end

local function on_disk(path)
    return not path:find("^%a[%w+.-]+:") -- http:, edl:...; a drive letter is a single char
end

local windows = package.config:sub(1, 1) == "\\"

local function quote(s)
    if windows then return '"' .. s .. '"' end
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

local function mkvmerge()
    local video = mp.get_property("path")
    if not (applied and video) then return say("nothing applied: use apply first") end
    if not (on_disk(video) and on_disk(applied)) then
        return say("mkvmerge needs both sources as files on disk", 6)
    end
    local cwd = mp.get_property("working-directory")
    local audio = utils.join_path(cwd, applied)
    video = utils.join_path(cwd, video)
    -- pasted into PowerShell, double quotes run $(...), escape with ` and also end at curly quotes
    if windows and ((video .. audio):find("[$`]") or (video .. audio):find("\226\128[\156-\158]")) then
        return say("rename the file or folder: $, ` or curly quotes in its path are not safe to paste into PowerShell", 8)
    end

    -- shortcut: takes mpv's ff-index as mkvmerge's track ID (same in mkv, mp4, avi and ts); a stream mkvmerge
    -- skips would shift it, then match the tracks through `mkvmerge -J` instead.
    -- The dub becomes the only default audio, so the file plays like this session did.
    local dub, own = nil, {}
    for _, t in ipairs(mp.get_property_native("track-list", {})) do
        if t.type == "audio" and not t.external then own[#own + 1] = "--default-track " .. t["ff-index"] .. ":0 " end
        if t.type == "audio" and t["external-filename"] == applied and (t.selected or not dub) then dub = t end
    end
    if not dub then return say("the added audio is gone: apply it again") end

    local id = dub["ff-index"]
    -- mpv and mkvmerge share the sign: positive delays the audio
    local ms = math.floor(mp.get_property_number("audio-delay", 0) * 1000 + 0.5)
    local command = ("mkvmerge -o %s %s%s -D -S -M --no-chapters --no-global-tags -a %d --sync %d:%d --default-track %d:1 %s")
        :format(quote(video:gsub("%.[^.\\/]*$", "") .. ".anyaudio.mkv"), table.concat(own), quote(video), id, id, ms, id, quote(audio))
    mp.msg.info(command)
    if mp.set_property("clipboard/text", command) then
        say("mkvmerge command copied to the clipboard")
    else
        say("mkvmerge command written to the terminal / log", 6)
    end
end

mp.register_event("start-file", function() applied = nil end)

mp.add_key_binding("F6", "grab", grab)
mp.add_key_binding("F7", "apply", apply)
mp.add_key_binding("F8", "clear", clear)
mp.add_key_binding("Ctrl+F7", "mkvmerge", mkvmerge)
