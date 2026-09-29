-- Rotates the entity around Y. 'speed' (degrees/sec) shows up as editable in the
-- Properties panel automatically.
Rotator = {
    speed = 45
}

function Rotator:Awake()
    Log.Info("Rotator awake on " .. self.entity.name)
end

function Rotator:Update(dt)
    local t = self.entity:GetTransform()
    t:Rotate(Vec3.new(0, self.speed * dt, 0))
end



