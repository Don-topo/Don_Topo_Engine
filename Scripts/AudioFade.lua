-- Baja el volumen del AudioClip del GameObject hasta cero y lo para.
-- Sirve de prueba manual de SetVolume/GetVolume por frame.
AudioFade = {
    -- Segundos que tarda el fade completo
    fadeTime = 3
}

function AudioFade:Start()
    self.clip = self.entity:GetComponent("AudioClip")
    if self.clip then
        self.clip:SetVolume(1.0)
        self.clip:Play()
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
