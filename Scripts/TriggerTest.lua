-- Attach this script to a GameObject that has a collider marked as
-- "Is Trigger" in the Properties panel. It logs to the console when another
-- object with a collider enters or leaves the trigger. `other` is the Entity that
-- caused it (same type as self.entity: .name, :IsValid(), :GetTransform()...).
--
-- Note: only the TRIGGER side receives these callbacks; the object that enters
-- does not (unless it is also a trigger against a non-trigger).
TriggerTest = {}

function TriggerTest:OnTriggerEnter(other)
    Log.Info(self.entity.name .. ": ENTER <- " .. other.name)
	DestroyGameObject(self.entity)
end

function TriggerTest:OnTriggerExit(other)
    Log.Info(self.entity.name .. ": EXIT <- " .. other.name)
end

-- OnTriggerStay fires every physics frame while they keep overlapping; it is
-- left commented out so it does not flood the Log. Uncomment it to see it.
-- function TriggerTest:OnTriggerStay(other)
--     Log.Info(self.entity.name .. ": STAY <- " .. other.name)
-- end

