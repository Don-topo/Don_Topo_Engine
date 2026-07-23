-- Sonda de verificación manual de triggers. A diferencia de TriggerTest.lua
-- (que destruye su GameObject en el Enter, para cubrir el caso del puntero
-- colgante), esta NO toca la escena: solo loguea, así se puede entrar y salir
-- del trigger las veces que haga falta.
--
-- Adjúntalo a un GameObject con un collider marcado "Is Trigger". El otro
-- objeto necesita collider, pero NO debe ser trigger: PhysX no reporta pares
-- trigger-trigger.
--
-- `other` es la Entity que lo provocó (mismo tipo que self.entity).
TriggerProbe = {
    -- Pon a true para ver el Stay, que dispara CADA frame de física mientras
    -- siguen solapando. Inunda el Log a propósito: es la forma de comprobar
    -- que se sintetiza de verdad y no una sola vez.
    verStay = false
}

function TriggerProbe:Start()
    self.enters = 0
    self.exits  = 0
    Log.Info("TriggerProbe listo en " .. self.entity.name)
end

function TriggerProbe:OnTriggerEnter(other)
    self.enters = self.enters + 1
    Log.Info("ENTER #" .. self.enters .. " <- " .. other.name)
end

function TriggerProbe:OnTriggerExit(other)
    self.exits = self.exits + 1
    Log.Info("EXIT #" .. self.exits .. " <- " .. other.name)
end

function TriggerProbe:OnTriggerStay(other)
    if self.verStay then
        Log.Info("STAY <- " .. other.name)
    end
end
