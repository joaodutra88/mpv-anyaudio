-- anyaudio: watch the video of one source with the audio of another.
-- grab  (F6): on the source whose audio you want, remember its URL
-- apply (F7): on the source whose video you want, add that audio and select it
-- clear (F8): drop the added tracks and go back to the source's own audio

local options = require "mp.options"

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

mp.register_event("start-file", function() applied = nil end)

mp.add_key_binding("F6", "grab", grab)
mp.add_key_binding("F7", "apply", apply)
mp.add_key_binding("F8", "clear", clear)
