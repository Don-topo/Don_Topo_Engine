-- Probe for checking triggers by hand. Unlike TriggerTest.lua (which destroys
-- its GameObject on Enter, to cover the dangling-pointer case), this one does NOT
-- touch the scene: it only logs, so you can enter and leave the trigger as many
-- times as needed.
--
-- Attach it to a GameObject with a collider marked "Is Trigger". The other
-- object needs a collider, but must NOT be a trigger: PhysX does not report
-- trigger-trigger pairs.
--
-- `other` is the Entity that caused it (same type as self.entity).
TriggerProbe = {
    -- Set to true to see Stay, which fires EVERY physics frame while they keep
    -- overlapping. It floods the Log on purpose: that is how you check it is
    -- really synthesized and not fired just once. (verStay = "show Stay"; the
    -- name is kept because scenes save it.)
    verStay = false
}

function TriggerProbe:Start()
    self.enters = 0
    self.exits  = 0
    Log.Info("TriggerProbe ready on " .. self.entity.name)
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
