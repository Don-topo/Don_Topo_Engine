
function movef:Start()
end

function movef:Update(dt)
    local rb = self.entity:GetComponent("Rigidbody")
    if rb then
        rb:AddForce(0, 0, -10)
    end
end
