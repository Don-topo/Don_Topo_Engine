#pragma once

// Pegamento entre el input del juego (teclado y mando, o sea GLFW) y lo que
// UiCanvas entiende.
//
// Vive FUERA de UiCanvas a propósito: el canvas es CPU pura y determinista y no
// conoce ni GLFW ni Input, que es justo lo que permite probarlo sin ventana. Lo
// que falta sin esto es todo lo que no sea el ratón — el foco, el Tab, las
// flechas y el botón de aceptar del mando quedaban implementados y probados
// pero nadie los alimentaba, así que un juego de mando no podía ni moverse por
// un menú.

#include "DonTopo/UI/UiCanvas.h"

namespace DonTopo
{
    // Rellena keys (FLANCOS de este frame, no teclas mantenidas) y los tres
    // modificadores. No toca ni el ratón ni el tiempo: de eso se encarga quien
    // llama, que es el único que sabe en qué espacio está su cursor.
    //
    // Teclado: Tab, Enter (también el del teclado numérico), Escape y flechas.
    // Mando: la cruceta y el stick izquierdo mueven, A acepta y B cancela — el
    // mismo reparto que espera cualquiera que haya tocado un menú de consola.
    void fillUiInputKeys(UiInputState& out);
}
