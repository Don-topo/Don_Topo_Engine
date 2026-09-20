#pragma once

#include <cmath>

namespace DonTopo
{
    // El reloj del camino SIN Animator: el personaje que no tiene componente
    // reproduce su clip 0 en bucle y es el backend quien lleva el tiempo.
    //
    // Vive aquí, y no en cada backend, porque estaba escrito dos veces y las dos
    // copias habían divergido (fila A13 del audit de animación): Vulkan
    // multiplicaba por ticksPerSecond y congelaba el reloj de un mesh oculto;
    // D3D12 ni guardaba ticksPerSecond —sumaba los segundos del frame a un
    // reloj que el compute lee en TICKS, así que iba entre 24 y 30 veces más
    // lento— ni miraba la visibilidad. Con Animator no se notaba nada, porque
    // ahí el tiempo lo calcula el AnimatorComponent y llega ya en ticks.
    //
    // animTime y durationTicks van en TICKS de Assimp (`aiAnimation::mDuration`,
    // lo que guarda AnimationClip::duration); dt, en segundos.
    inline float advanceMeshClock(float animTime, float dt, float ticksPerSecond,
                                  float durationTicks, bool visible)
    {
        // Sin ritmo o sin duración no hay clip que muestrear: mover el reloj
        // solo serviría para que el wrap no pudiera acotarlo nunca.
        if (ticksPerSecond <= 0.0f || durationTicks <= 0.0f) return animTime;
        // Oculto: no se ve, así que su reloj tampoco corre. Al volver a marcarlo
        // visible reanuda donde se quedó en vez de saltar hacia delante.
        if (!visible) return animTime;
        float t = animTime + dt * ticksPerSecond;
        if (t > durationTicks) t = std::fmod(t, durationTicks);
        return t;
    }
}
