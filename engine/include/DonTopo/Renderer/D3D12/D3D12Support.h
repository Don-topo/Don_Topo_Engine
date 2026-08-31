#pragma once

// Solo existe cuando el backend DirectX 12 se compila (DTE_ENABLE_D3D12=ON).
// Los consumidores deben guardar sus llamadas con el mismo #ifdef.
#ifdef DT_D3D12_ENABLED

#include <string>

namespace DonTopo::D3D12 {

// Resultado de preguntarle al sistema si hay un adaptador capaz de DX12, sin
// llegar a crear el device. Lo usa el selector de backend del editor: ofrecer
// DirectX 12 en una máquina que no lo soporta llevaría a un fallo al arrancar,
// que es justo el momento en el que peor se puede avisar.
struct SupportInfo {
    bool supported = false;
    std::string adapterName;  // Descripción del adaptador elegido; vacío si no hay
    std::string error;        // Motivo; solo tiene contenido cuando supported == false
};

// Enumera los adaptadores por DXGI y devuelve el primero que acepte
// D3D_FEATURE_LEVEL_11_0. Descarta los adaptadores software (WARP). No crea
// device ni deja ningún objeto vivo tras devolver.
SupportInfo querySupport();

// UTF-16 -> UTF-8. La descripcion de un adaptador y el JSON de estadisticas de
// D3D12MA llegan en wchar_t, y el resto del motor habla std::string en UTF-8
// (el log, el project.json, ImGui). Cadena vacia si `wide` es nullptr o vacia.
//
// Vive aqui porque estaba escrita DOS veces —esta y otra en D3D12Renderer.cpp,
// byte a byte la misma salvo el estilo de llaves— y las dos convierten lo que
// acaba en el mismo log (H48).
std::string narrow(const wchar_t* wide);

}  // namespace DonTopo::D3D12

#endif  // DT_D3D12_ENABLED
