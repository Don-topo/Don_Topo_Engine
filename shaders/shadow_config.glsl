// Tamanos del bloque de sombras del UBO, en UN solo sitio del lado GLSL.
//
// Estos tres numeros estaban escritos a mano en los SEIS shaders que declaran el
// bloque (pbr.frag, triangle.frag, triangle.vert, outline.vert, shadow.vert y
// fog.comp). Cambiarlos obligaba a acertar seis veces, y fallar no da ningun
// error: std140 desplaza en silencio todo lo que va detras de lightSpaceMatrix y
// el shader lee el UBO corrido. Es el patron de H76 —constantes duplicadas entre
// consumidores del mismo bloque— aplicado al lado del shader.
//
// La otra copia inevitable es la de C++ (UniformBufferObject.h): un shader no
// puede incluir un header de C++. Dos sitios, no ocho, y los dos documentados el
// uno en el otro.
#ifndef DT_SHADOW_CONFIG_GLSL
#define DT_SHADOW_CONFIG_GLSL

// Cascadas de la luz key cuando es direccional.
#define SHADOW_CASCADES 4

// Huecos reservados a la luz KEY: 4 si es direccional (una por cascada), 6 si es
// de punto o un foco muy abierto (una por cara del cubemap), 1 si es un foco
// normal. Nunca coexisten: solo hay una key y solo tiene un tipo.
#define SHADOW_KEY_MATRICES 6

// Capas para las luces SECUNDARIAS, detras de las de la key. Una por foco
// estrecho, seis por luz de punto o foco muy abierto. Con seis caben una luz de
// punto O seis focos.
//
// El freno es la MEMORIA: estas capas viven en el mismo array que las de la key,
// o sea a su misma resolucion, y a 2048 cada una son 16 MB. Darles un array
// propio mas pequeno permitiria subirlas sin pagarlo, pero obliga a un binding
// nuevo en los seis shaders del bloque.
#define SHADOW_EXTRA_LAYERS 6

// Total del array del UBO. Tiene que valer lo mismo que SHADOW_MATRICES en
// UniformBufferObject.h.
#define SHADOW_MATRICES (SHADOW_KEY_MATRICES + SHADOW_EXTRA_LAYERS)

#endif
