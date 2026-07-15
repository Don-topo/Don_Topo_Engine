-- Adjunta este script a un GameObject que tenga un collider marcado como
-- "Is Trigger" en el panel Properties. Loguea en la consola cuando otro
-- objeto con collider entra o sale del trigger. `other` es la Entity que lo
-- provocó (mismo tipo que self.entity: .name, :IsValid(), :GetTransform()...).
--
-- Nota: solo el lado TRIGGER recibe estos callbacks — el objeto que entra no
-- los recibe (a menos que él también sea trigger frente a otro no-trigger).
TriggerTest = {}

function TriggerTest:OnTriggerEnter(other)
    Log.Info(self.entity.name .. ": ENTER <- " .. other.name)
end

function TriggerTest:OnTriggerExit(other)
    Log.Info(self.entity.name .. ": EXIT <- " .. other.name)
end

-- OnTriggerStay dispara cada frame de física mientras siguen solapando; se
-- deja comentado para no inundar el Log. Descoméntalo para verlo.
-- function TriggerTest:OnTriggerStay(other)
--     Log.Info(self.entity.name .. ": STAY <- " .. other.name)
-- end
