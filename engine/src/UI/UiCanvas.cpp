#include "DonTopo/UI/UiCanvas.h"

namespace DonTopo
{
    UiNode& UiNode::addChild(std::string childName)
    {
        m_children.push_back(std::make_unique<UiNode>(std::move(childName)));
        return *m_children.back();
    }

    UiCanvas::UiCanvas()
    {
        // La raíz agrupa, no pinta: su rect es la pantalla entera y dibujarla
        // taparía la escena con un rectángulo blanco.
        m_root.drawable = false;
    }

    void UiCanvas::clear()
    {
        m_root.clearChildren();
    }

    void UiCanvas::buildDrawData(uint32_t width, uint32_t height, UiDrawData& out) const
    {
        out.clear();
        if (!m_visible || width == 0 || height == 0) return;
        UiSpriteBatch::build(*this, width, height, out);
    }
}
