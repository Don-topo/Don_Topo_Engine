-- Empuja el GameObject con el Rigidbody, no con el Transform: esta es la forma
-- que SÍ colisiona (mover por Transform es un teletransporte y atraviesa las
-- paredes, ver el README).
--
-- Requiere Rigidbody y que NO sea kinematic: un kinematic ignora las fuerzas.
PushMe = {
    fuerza = 50000,
    -- Impulso hacia arriba al pulsar Space
    salto = 30000
}

function PushMe:Start()
    self.rb = self.entity:GetComponent("Rigidbody")
    if not self.rb then
        Log.Error("PushMe: el GameObject no tiene Rigidbody")
    end
end

function PushMe:Update(dt)
    if not self.rb then return end

    -- Flechas: fuerza continua (se acumula mientras la tecla siga pulsada)
    if Input.IsKeyDown(Key.Right) then self.rb:AddForce(self.fuerza * dt, 0, 0) end
    if Input.IsKeyDown(Key.Left)  then self.rb:AddForce(-self.fuerza * dt, 0, 0) end
    if Input.IsKeyDown(Key.Up)    then self.rb:AddForce(0, 0, -self.fuerza * dt) end
    if Input.IsKeyDown(Key.Down)  then self.rb:AddForce(0, 0, self.fuerza * dt) end

    -- Space: impulso instantáneo (AddImpulse no se multiplica por dt)
    if Input.IsKeyPressed(Key.Space) then
        self.rb:AddImpulse(0, self.salto, 0)
        Log.Info("Impulso! velocidad Y = " .. string.format("%.1f", self.rb.velocity.y))
    end
end
