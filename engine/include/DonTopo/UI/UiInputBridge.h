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

    // Un caracter tecleado, en codepoint Unicode. Lo llama el callback de
    // caracteres de GLFW (glfwSetCharCallback) y lo VACIA fillUiInputKeys en el
    // frame siguiente.
    //
    // Hace falta un acumulador porque GLFW da los caracteres SOLO por callback:
    // no hay un "que se ha tecleado ahora" que consultar, igual que pasa con la
    // rueda del raton. Leerlo sin vaciarlo repetiria el texto para siempre.
    //
    // Quien llama decide CUANDO empujar: en el editor solo durante Play y con
    // ImGui sin el foco de texto, o escribir en un campo del inspector acabaria
    // tambien dentro del InputField del juego.
    void pushUiInputChar(uint32_t codepoint);

    // Tira lo acumulado sin entregarlo. Para el frame en el que quien llama NO
    // va a consumir el texto: sin esto, lo tecleado mientras la UI del juego
    // estaba apagada saldria de golpe al encenderla.
    void discardUiInputChars();
}
