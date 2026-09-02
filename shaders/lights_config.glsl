// Cuantas luces declara el bloque UBO, en UN solo sitio del lado GLSL.
//
// Estaba escrito a mano en los TRES shaders que declaran el array (pbr.frag,
// triangle.frag y fog.comp). Fallar en uno no da ningun error: std140 desplaza
// en silencio todo lo que va detras del array —viewPos, numLights,
// ambientIntensity— y ese shader lee el UBO corrido. Mismo patron y mismo
// arreglo que shadow_config.glsl, que hace esto con los tamanos del bloque de
// sombras; va en un fichero aparte porque MAX_LIGHTS no es una constante de
// sombras y meterla alli dejaria el nombre mintiendo.
//
// La otra copia es inevitable: la de C++ (UniformBufferObject.h), porque un
// shader no puede incluir un header de C++. Dos sitios, no cuatro, y los dos
// documentados el uno en el otro.
#ifndef DT_LIGHTS_CONFIG_GLSL
#define DT_LIGHTS_CONFIG_GLSL

// Tiene que valer lo mismo que MAX_LIGHTS en UniformBufferObject.h. Ver alli
// por que son 64 y que hay que tocar al cambiarlo.
#define MAX_LIGHTS 64

#endif
