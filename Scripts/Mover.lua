-- Mueve la entity con las flechas del teclado. Demuestra Input, props
-- múltiples y FixedUpdate.
Mover = {
    speed = 100,
    verbose = false
}

function Mover:Start()
    if self.verbose then Log.Info("Mover listo en " .. self.entity.name) end
end

function Mover:Update(dt)
    local t = self.entity:GetTransform()
    local d = Vec3.new(0, 0, 0)
    if Input.IsKeyDown(Key.Right) then d.x = d.x + self.speed * dt end
    if Input.IsKeyDown(Key.Left)  then d.x = d.x - self.speed * dt end
    if Input.IsKeyDown(Key.Up)    then d.z = d.z - self.speed * dt end
    if Input.IsKeyDown(Key.Down)  then d.z = d.z + self.speed * dt end
    t:Translate(d)
end

function Mover:OnDestroy()
    if self.verbose then Log.Info("Mover destruido") end
end
