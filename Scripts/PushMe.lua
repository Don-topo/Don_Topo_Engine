-- Pushes the GameObject through its Rigidbody, not its Transform: this is the way
-- that DOES collide (moving through the Transform is a teleport and goes through
-- walls, see the README).
--
-- Needs a Rigidbody that is NOT kinematic: a kinematic body ignores forces.
-- The prop names (fuerza = force, salto = jump) are kept because scenes save them.
PushMe = {
    fuerza = 50000,
    -- Upward impulse when Space is pressed
    salto = 30000
}

function PushMe:Start()
    self.rb = self.entity:GetComponent("Rigidbody")
    if not self.rb then
        Log.Error("PushMe: the GameObject has no Rigidbody")
    end
end

function PushMe:Update(dt)
    if not self.rb then return end

    -- Arrows: continuous force (it accumulates while the key stays down)
    if Input.IsKeyDown(Key.Right) then self.rb:AddForce(self.fuerza * dt, 0, 0) end
    if Input.IsKeyDown(Key.Left)  then self.rb:AddForce(-self.fuerza * dt, 0, 0) end
    if Input.IsKeyDown(Key.Up)    then self.rb:AddForce(0, 0, -self.fuerza * dt) end
    if Input.IsKeyDown(Key.Down)  then self.rb:AddForce(0, 0, self.fuerza * dt) end

    -- Space: instant impulse (AddImpulse is not multiplied by dt)
    if Input.IsKeyPressed(Key.Space) then
        self.rb:AddImpulse(0, self.salto, 0)
        Log.Info("Impulse! Y velocity = " .. string.format("%.1f", self.rb.velocity.y))
    end
end
