-- Rota la entity sobre Y. 'speed' (grados/seg) aparece editable en el
-- panel Properties automáticamente.
Rotator = {
    speed = 45
}

function Rotator:Awake()
    Log.Info("Rotator despierto en " .. self.entity.name)
end

function Rotator:Update(dt)
    local t = self.entity:GetTransform()
    t:Rotate(Vec3.new(0, self.speed * dt, 0))
end


