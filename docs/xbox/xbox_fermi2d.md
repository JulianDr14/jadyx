# Fermi2D: identificar operaciones sin soporte (0.3.4.9)

## Evidencia y objetivo

La captura Series 0.3.4.8 del 19 de octubre contiene 134 mensajes
`Operation is not copy`, el primero a 65,262415 s. El mensaje no identifica
la operación. No demuestra que esa operación cause los FPS bajos ni un efecto
visual concreto. El motor continúa y el backend D3D12 recibe una operación que
`TextureCacheRuntime::BlitImage` actualmente ignora.

Esta entrega identifica la petición; no implementa todavía blending/ROP.
Conserva la corrección de memoria de 0.3.4.8 y añade los diagnósticos pendientes
de compilación lenta de shaders/PSO. Pulse/video queda fuera del alcance.

## Cómo lo hemos investigado antes

- [Dimensiones SRV](xbox_srv_dimensions.md): registrar valores y sus dominios
  enum, formato, dirección y dimensiones antes de asociar un warning a un recurso.
- [Geometry passthrough](xbox_geometry_passthrough.md): seguir la petición desde
  el frontend hasta el backend; un aviso puede sobrevivir aunque exista lowering.
  Confirmar el fallo real mediante un gate controlado antes de corregirlo.
- [Copias BC](xbox_bc_block_copy.md): correlacionar origen/destino y comprobar
  datos exactos; la proximidad temporal de dos logs no identifica un mismo recurso.
- [Fase 4](xbox_d3d12_phase4.md): documenta que el motor 2D trataba las operaciones
  blend como copias, también en Vulkan. No asumir una regresión exclusiva D3D12.

La documentación primaria de [NVIDIA, clase Fermi 2D 0x902d](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/twod/cl902d.h)
permite contrastar offsets y campos. [Envytools, pipeline 2D](https://envytools.readthedocs.io/en/latest/hw/graph/2d/intro.html)
explica las diferencias entre copia, ROP, key y blend. Estas operaciones requieren
estados distintos; no se debe eliminar el aviso suponiendo que todas son copias.

## Qué registra 0.3.4.9

`Fermi2D diagnostic` incluye operación numérica y nombre del enum del emulador:
0 SrcCopyAnd, 1 ROPAnd, 2 Blend, 3 SrcCopy, 4 ROP, 5 SrcCopyPremult,
6 BlendPremult; los demás se nombran Unknown. Registra escrituras de operation
observadas por CallMethod y ConsumeSinkImpl desde la creación de ese motor.
Cero escrituras no prueba una restauración Pulse: también puede ser estado inicial.

Cada snapshot incluye registros crudos, con offsets **en bytes** y valores hex:

- 0x200–0x254: superficies destino/origen, formatos, layout, pitch, tamaño y dirección.
- 0x260–0x2b4: render enable, clip, color key, ROP, beta1/beta4, operación y patrón.
- 0x2e8–0x2fc: patrón monocromo. El contenido completo del patrón de color no se vuelca.
- 0x880–0x8dc: sample mode, rectángulo destino, derivadas y coordenadas origen
  32.32 intactas. Los índices internos de métodos equivalen a offset/4.

`Fermi2D result` correlaciona el snapshot con las direcciones y rectángulos efectivos
después del ajuste de pitch y con AccelerateSurfaceCopy=true/false. El tiempo host
incluye esa llamada y el fallback software, excluye FlushCaching y no mide tiempo
GPU ni garantiza que el backend haya implementado la operación correctamente.

Memoria fija: 32 estados por motor. Deduplicación exacta por clip/key/ROP/betas/
operación/patrón seleccionado, formatos, sample mode y layouts. Las direcciones,
tamaños y contenido del patrón no forman parte de la clave: se captura su primer
ejemplo, no cada variación de recurso. Tras 32 estados nuevos se omiten snapshots
adicionales. El Critical original permanece e incluye siempre operación y ROP.
El resumen al destruir el motor cuenta todas las peticiones no copia y snapshots
guardados; puede faltar si se termina el proceso abruptamente. No se lee memoria
guest adicional ni se añade sincronización GPU. El volcado inicial tiene coste de
logger: no usar ese instante para certificar mejora o empeoramiento de tirones.

## Gate y siguiente paso

Compilar y empaquetar .9; comprobar versión, firma, BlockMap y ausencia de payload.
En Series, reproducir la misma zona y devolver jadyx_log.txt/jadyx_diag.txt. Los
snapshots se generan automáticamente al aparecer una operación no copia.

Con operación/ROP y superficies reales, comprobar si se reduce matemáticamente a
SrcCopy o necesita lógica/blending. Construir un caso sintético con el mismo estado,
comparar resultado esperado y GPU (formatos/alpha/clip/pitch), implementar y repetir
en Series. Solo después contrastar efectos visuales y FPS con escenas equivalentes.
Un paquete compilado no certifica efectos corregidos ni mejora de rendimiento.

Validación de entrega (19 octubre 2026): build UWP correcto (94 operaciones y
rebuild final de 4 por include); paquete 0.3.4.9 firmado, CRC ZIP y 735 bloques
SHA-256 de 34 archivos verificados. EXE idéntico a bin y symbols/0.3.4.9;
boot.cfg library=1/play=1, sin payload de usuario ni PDB dentro del APPX.
SignedCms CheckSignature(true) correcto (firma matemática; no valida confianza
de cadena en la consola). APPX SHA-256:
4db9f336bdfc94fa7d7da13fe84d4803d827e6c980e3ab3a76b40557f89ea181.
No se ha ejercitado aún una operación no copia en Series con este paquete.
