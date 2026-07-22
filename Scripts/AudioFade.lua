-- Baja el volumen del AudioClip del GameObject hasta cero y lo para.
-- Sirve de prueba manual de SetVolume/GetVolume por frame.
--
-- Deja "Play On Awake" DESACTIVADO en este GameObject: al pulsar Play,
-- onPlayStart ejecuta primero el Start() de este script (que ya llama a
-- clip:Play()) y DESPUÉS el motor recorre la escena arrancando los clips con
-- playOnAwake activo. No se solapan (el segundo play corta la voz anterior del
-- mismo clip), pero el clip se REINICIA desde el principio justo después de
-- haber arrancado, lo que se oye como un chasquido al entrar en Play.
AudioFade = {
    -- Segundos que tarda el fade completo
    fadeTime = 3
}

function AudioFade:Start()
    self.clip = self.entity:GetComponent("AudioClip")
    if self.clip then
        self.clip:SetVolume(1.0)
        self.clip:Play()
    else
        Log.Error("AudioFade: el GameObject no tiene AudioClip asignado")
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
