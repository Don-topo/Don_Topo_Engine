-- Lowers the GameObject's AudioClip volume to zero and stops it.
-- A manual test of SetVolume/GetVolume per frame.
--
-- Leave "Play On Awake" OFF on this GameObject: when Play is pressed,
-- onPlayStart first runs this script's Start() (which already calls
-- clip:Play()) and THEN the engine walks the scene starting the clips with
-- playOnAwake on. They do not overlap (the second play cuts the previous voice of
-- the same clip), but the clip RESTARTS from the beginning right after it
-- started, which sounds like a click when entering Play.
AudioFade = {
    -- Seconds the full fade takes
    fadeTime = 3
}

function AudioFade:Start()
    self.clip = self.entity:GetComponent("AudioClip")
    if self.clip then
        self.clip:SetVolume(1.0)
        self.clip:Play()
    else
        Log.Error("AudioFade: the GameObject has no AudioClip assigned")
    end
end

function AudioFade:Update(dt)
    if not self.clip then return end

    local v = self.clip:GetVolume() - dt / self.fadeTime
    if v <= 0 then
        self.clip:SetVolume(0)
        self.clip:Stop()
        self.clip = nil
    else
        self.clip:SetVolume(v)
    end
end
