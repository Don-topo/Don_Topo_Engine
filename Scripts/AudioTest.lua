AudioTest = {}

function AudioTest:Start()
    self.clip = self.entity:GetComponent("AudioClip")
    if self.clip then
        Log.Info("AudioTest: AudioClip encontrado, loop=" .. tostring(self.clip:GetLoop()))
    else
        Log.Error("AudioTest: el GameObject no tiene AudioClip asignado")
    end
end

function AudioTest:Update(dt)
    if not self.clip then return end

    if Input.IsKeyPressed(Key.Space) then
        self.clip:Play()
        Log.Info("AudioTest: Play")
    end
    if Input.IsKeyPressed(Key.Enter) then
        self.clip:Stop()
        Log.Info("AudioTest: Stop")
    end
    if Input.IsKeyPressed(Key.L) then
        local newLoop = not self.clip:GetLoop()
        self.clip:SetLoop(newLoop)
        Log.Info("AudioTest: Loop = " .. tostring(newLoop))
    end
end
