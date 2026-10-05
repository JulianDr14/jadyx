# ASTC original mediante muestreo propio: prototipo SM 6.4

Investigación y primera implementación, 2 de octubre de 2026. Prioridad del usuario:
**payload ×1, calidad primero**. El primer gate es aislado y opt-in. La última
sección describe la integración experimental autorizada posteriormente por el usuario.
Ambas rutas están desactivadas por defecto; no hay certificación de FPS ni de Series.

## Decisión y fuentes

Un texel ASTC se localiza leyendo su bloque de 128 bits. Es posible conservar los
bloques y decodificar en un shader, sin otra codificación con pérdida. La
[especificación Khronos](https://github.com/KhronosGroup/DataFormat/blob/main/astc.txt)
describe el acceso por bloque y los perfiles de decodificación. Eso fundamenta la
funcionalidad; el coste se mide, no se deduce del tamaño comprimido.

Para ASTC 8×8 grande, sin redondeo de bordes/mips:

| Destino | Bytes por texel | Payload frente a ASTC 8×8 | Condición |
|---|---:|---:|---|
| ASTC original, sampler propio | 0,25 | ×1 | Decoder y filtros en shaders |
| ASTC + caché parcial | variable | objetivo ≤×2 | Contar índices, feedback y alineación |
| BC1 | 0,5 | ×2 | Color/alfa binaria, otra recompresión |
| BC3 / BC7 | 1 | ×4 | Alfa gradual, otra recompresión |
| RGBA8 completo | 4 | ×16 | Ruta actual de arrays/cube/volúmenes |

[Microsoft](https://learn.microsoft.com/en-us/windows/win32/direct3d11/texture-block-compression-in-direct3d-11)
documenta 8 bytes por bloque BC1 y 16 para BC3/BC7. BC7 mejora opciones de calidad,
pero no reduce los bytes de BC3. Para footprint ASTC `bw×bh`, los factores ideales
son `bw*bh/32` en BC1, `/16` en BC3/BC7 y `/4` en RGBA8. Con mips pequeños se
calculan bloques completos y no basta esa proporción.
[ARM](https://github.com/ARM-software/astc-encoder/blob/main/Docs/FormatOverview.md)
documenta los footprints y sus tasas.

La ruta vigente D3D12 ya hace ASTC GPU → temporal RGBA8 → BC3 en imágenes 2D de
una capa. Arrays/cubemaps/volúmenes usan RGBA8 por la corrupción observada en el
sampler BC de Series. Se conserva esa política. La
[tabla FL 11.0 de Microsoft](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware)
contempla BC en Texture2D, TextureCube y Texture3D: el fallo de Series registrado
en `xbox_d3d12_phase4.md` no demuestra una prohibición general del formato.

Series UWP expone FL 11.0 y SM hasta 6.4 según
[Microsoft/Chuck Walbourn](https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/).
Las [reglas UWP de recursos](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
se distinguen del GDK de juegos nativos. No encontramos una interfaz pública
UWP para añadir ASTC al sampler DXGI. Parte de la documentación Xbox requiere
acceso autorizado; esta investigación abarca documentación y repositorios públicos.

Alternativas revisadas:

- [FastBlockCompress](https://github.com/microsoft/Xbox-GDK-Samples/tree/main/Samples/Graphics/FastBlockCompress):
  encoders GPU BC1/BC3/BC5; los algoritmos sirven de referencia. El aliasado y
  tiling Xbox de la muestra usan XG/XMem/FlushPipelineX, no una ruta UWP portable.
- [DirectXTex](https://github.com/microsoft/DirectXTex),
  [DirectXTK12](https://github.com/microsoft/DirectXTK12) y
  [Compressonator](https://github.com/GPUOpen-Tools/compressonator): BC residente
  continúa teniendo los bytes del formato final.
- [DirectStorage/BCPack](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/storage/directstorage/directstorage-overview)
  y [Basis Universal](https://github.com/BinomialLLC/basis_universal): almacenamiento,
  distribución y descompresión/transcodificación; no convierten BC7 residente en 2 bpp.
- [Sampler Feedback](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_5.html):
  requiere SM 6.5; una caché en nuestra ruta necesitaría feedback propio.
- [Granite](https://github.com/Themaister/Granite/blob/master/assets/shaders/decode/astc.comp)
  y el decoder Vulkan de Eden fundamentan la reutilización del decoder existente.
  [Xenia](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_texture_cache.cc)
  y [Dolphin](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DX12Texture.cpp)
  aportan patrones D3D12, no un sampler ASTC listo para portar.
- [RTXNTC](https://github.com/NVIDIA-RTX/RTXNTC) y
  [JPEG en shaders](https://github.com/elias1518693/jpeg_textures) son precedentes
  de muestreo comprimido por software. Añaden otra codificación/entrenamiento y
  filtros propios; no son el primer experimento con calidad y bloques originales.

## Arquitectura y contrato

`d3d12_astc_probe.cpp` crea Device, ShaderCompiler, Scheduler y recursos propios.
`astc_sampling_probe=1` entra antes de construir Core::System. No carga un guest,
no emite entradas, no crea JIT ni cambia presupuestos de gameplay. Devuelve 0/14
y el frontend se cierra solo. Con la opción ausente no crea recursos del probe.

Un buffer DEFAULT agrupa todos los bloques originales de las imágenes. Cada mip
tiene 48 bytes de metadatos: dimensiones/profundidad/número de mips, offsets de
bloques y texels de referencia, footprint, tipo de imagen y sRGB. Los offsets
son índices de elementos. Los SRV son raw, acotados; se comprueban explícitamente
índices de bloque, metadatos y referencias. Los queries y resultados pertenecen
al harness y no forman parte del almacén de texturas.

La variante `D3D12_ASTC_SAMPLE` de `astc_decoder.comp` reutiliza el decoder, pero
interpola únicamente el texel solicitado. No escribe un bloque o imagen RGBA8.
Todavía decodifica el stream de endpoints y pesos del bloque; ese trabajo privado
se repite en cada tap. Se reinicia el estado privado entre taps para que no se
mezclen bloques. El mismo sampler explícito se compila en compute y fragment;
el fragment se dibuja a un target RGBA32F de 256×16, sin swapchain.

Contrato del primer gate:

- Footprints ASTC 2D admitidos: los 14 de 4×4 a 12×12; perfil LDR `decode_unorm8`.
  Los bloques originales retienen todos sus bits. La salida del gate es UNORM8,
  no una afirmación de equivalencia al perfil ASTC `decode_float16`.
- Load, point, bilinear; mezcla de dos mips con LOD explícito acotado. En volúmenes,
  el filtro espacial usa ocho taps y la profundidad disminuye por mip.
- Repeat, clamp y border transparente negro; coordenadas negativas incluidas.
  Las capas de arrays/Load de caras se proporcionan como índices enteros. Índices
  de capa inválidos y payload ausente devuelven negro, sin leer fuera del buffer.
- Cubemap por dirección, caras +X/-X/+Y/-Y/+Z/-Z, spill entre caras y promedio de
  tres caras para el tap ausente en una esquina.
- sRGB expande endpoints con `(C<<8)|0x80`, interpola, toma los bits altos y aplica
  EOTF RGB antes de filtrar; alfa no pasa por EOTF. LDR lineal replica los bytes
  del endpoint y toma los bits altos del resultado interpolado.
- ASTC inválido/HDR en perfil LDR devuelve magenta opaco. Ese comportamiento es
  del probe; el decoder histórico del renderer mantiene su política original.

Las reglas de sRGB/error/decode_unorm8 proceden de Khronos. Para las costuras se
contrasta también la [especificación funcional D3D, 7.18.12](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
D3D permite además repartir el tap ausente entre dos vecinos; el oracle comprueba
ambas reglas y registra si el hardware emplea la alternativa. Point en una
dirección que empate exactamente dos ejes tiene selección de cara ambigua:
se comprueba nuestra regla determinista, pero esos casos no se usan como oracle
de identidad con el sampler nativo.

Pendientes de integración: LOD implícito/derivadas, anisotropía, gather, todos los
parámetros de sampler guest (mirror, border arbitrario, comparación, índices de
array fraccionales y vistas), reinterpretación/aliasado, escrituras y coherencia,
invalidación, selección del perfil de precisión, y ASTC volumétrico auténtico.
No se reemplaza ninguna de esas operaciones por una aproximación en gameplay.

## Gates automáticos y referencias

El corpus es generado, determinista, sin juegos ni archivos externos: 14
footprints, mips parciales, arrays, seis caras, slices 3D con profundidad
9/4/2/1/1/1 y una array de benchmark de 512×512×8 con siete mips. Incluye
particiones 1–4, dual-plane, CEM mixtos y alpha gradual. Los nueve casos inválidos
cubren reservas, pocos pesos, conflicto dual-plane/particiones, pocos bits de
endpoint, HDR y void extents malformados. Otro void extent con canales de 16 bits
no replicados comprueba la truncación normativa y otro metadato comprueba payload
fuera de rango.

`DecompressForSamplingProbe` especializa el algoritmo CPU independiente para
LDR UNORM8/sRGB. El API normal conserva su especialización sin sRGB ni esas reglas.
No se atribuye al shader una diferencia causada por el oracle histórico:
éste redondeaba la interpolación y no implementaba esa expansión sRGB. Hay además
vectores con valores esperados por la especificación. Esto es un corpus amplio,
no prueba exhaustiva de todas las codificaciones ASTC; no se ejecutó ARM astcenc
como tercer decoder independiente.

Se validan todos los texels de las imágenes pequeñas y consultas de filtros,
direcciones/costuras, offsets, mips, negativos y capas. Se compara CPU con compute,
fragment y RGBA raw. Después se comparan cube/3D/sRGB con recursos/samplers nativos
RGBA8 de D3D12. CPU/direct requieren error ≤0,00002; hardware permite dos unidades
UNORM8 por precisión finita de filtrado, y contabiliza alternativas de esquinas.

Benchmark: 4096 queries, coherent/random, Load/point/bilinear/mip-linear,
1/4 muestras por píxel, compute/fragment. Calentamiento y cinco mediciones con
orden rotatorio de los tres PSO; mediana de timestamps GPU. Los resultados se leen
y se contrastan, evitando medir un shader sin efectos. El control RGBA raw usa
el mismo filtrado explícito; el control RGBA nativo usa el sampler de hardware.
No se transforma ese cociente en FPS de un juego.

Comandos, después del build incremental:

```powershell
powershell -ExecutionPolicy Bypass -File tools\xbox\astc-sampling-probe.ps1
powershell -ExecutionPolicy Bypass -File tools\xbox\astc-sampling-probe.ps1 -GpuValidation
```

El script no inicia la prueba si Eden está abierto. Mantiene Job 5120 MiB, recopila
pico de commit/working set, archiva log/diag/JSON y exige cierre, retorno 0,
32 benchmarks y ausencia de errores. GBV exige además su marcador real en el log;
sus tiempos instrumentados no son evidencia de rendimiento. Los archivos están
en `build-uwp/astc-probe-results/<fecha>-debug` o `-gbv`.

## Memoria: qué significa ×1

El payload permanece exactamente en 16 bytes por bloque. El multiplicador de
buffers residentes de origen incluye la asignación del buffer de bloques y del
buffer de metadatos. Se registra por separado cada recurso de referencia,
queries, resultados, target, readback y pico de uploads mediante
[GetResourceAllocationInfo](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc).
Las texturas de control se suben con GetCopyableFootprints, sin asumir row pitch.

**No se presenta ×1,096 de esos dos buffers como memoria completa del sampler o
del emulador.** PSO, driver, scratch privado, heaps de descriptors/query y temporales
del compilador no tienen ese mismo multiplicador. El log publica DXGI local y
AppMemoryUsage de todo el probe, y el runner publica el pico de commit del proceso.
Son mediciones distintas; PC tiene VRAM separada y no reproduce la UMA de Series.
La atribución del consumo adicional del driver/scratch queda pendiente: no se ha
demostrado un sampler completo residente ≤×2.

## Mediciones PC, 2 de octubre de 2026

Gate final: `build-uwp/astc-probe-results/20261002-192818-debug/`, PID 23544,
AppContainer AMD Radeon Pro 5300M, SM 6.4 explícito, debug layer. Cierre automático
en ~15,2 s, retorno 0, cero Error/Critical de Render. No se ejecutó gameplay.

- 21 imágenes, 117 mips, 44.833 bloques: 39.766 complejos, 14.903 dual-plane,
  27.623 multipartición, 4.727 mixed CEM, nueve inválidos; 14 footprints.
- 60.717 consultas CPU/direct/raw, compute y fragment: cero discrepancias;
  error máximo 0,000000298. No se conserva el bloque completo decodificado en GPU.
- 22.369 consultas frente al sampler RGBA nativo: cero discrepancias;
  error máximo 0,005099639 bajo la tolerancia de precisión de filtrado.
  Once consultas point de cubemap con empate entre ejes se excluyen de ese
  oracle hardware: la cara elegida puede variar según implementación. Siguen
  verificadas en el contrato determinista CPU/direct; cero alternativas de esquina
  utilizadas en esta GPU.
- Payload original: **717.328 bytes, ×1 exacto**. Metadatos: 5.616 bytes.
  Asignación de los dos buffers: **786.432 bytes, ×1,096335**. No representa el
  coste completo del sampler; su presupuesto total ≤×2 sigue sin demostrar.
- Asignaciones conocidas del harness en el pico: 53.936.128 bytes (51,44 MiB),
  incluidos controles RGBA raw/nativos, resultados, queries, readbacks y uploads.
  Control nativo: 11.927.552 bytes; uploads pico: 25.755.648 bytes.
  Observación después de crear recursos: AppMemoryUsage 806.768.640 bytes,
  DXGI local 639.283.200 bytes. Consumo adicional de PSO/driver/scratch no atribuido.
- Pico de commit del proceso: 768.475.136 bytes (732,875 MiB); máximo muestreado
  767.344.640. Pico working set: 610.598.912. Job 5120 MiB verificado.

DXIL directo: compute 473.548 bytes, fragment 473.566; SPIR-V 148.160/148.124.
Traducción, firma y creación de PSO juntas: 6.641,861/6.877,490 ms en el gate final.
En `20261002-192139-debug`, el mismo shader tardó 30.982,913/34.014,906 ms y el
pico de commit fue 1.884.708.864 bytes. La variación entre arranques se registra
sin atribuirla causalmente a una caché concreta. Los controles raw son ~23,7 KiB
DXIL y ~110–117 ms; los nativos ~7 KiB y ~11–12 ms en el gate final.

### Timestamps GPU

Medianas en microsegundos para **4096 queries**, cinco mediciones tras calentamiento.
La columna de muestras indica trabajo por query/píxel. Dataset y semilla coinciden
entre compute y fragment. Debug layer habilitada; GBV deshabilitado.

| Etapa | Accesos | Filtro | Muestras | ASTC µs | RGBA raw µs | RGBA nativo µs | ASTC/nativo |
|---|---|---|---:|---:|---:|---:|---:|
| compute | coherent | Load | 1 | 243.600 | 12.680 | 9.920 | 24.556 |
| compute | coherent | Load | 4 | 948.760 | 25.400 | 16.080 | 59.002 |
| compute | coherent | point | 1 | 241.960 | 14.200 | 10.240 | 23.629 |
| compute | coherent | point | 4 | 953.040 | 27.640 | 16.760 | 56.864 |
| compute | coherent | bilinear | 1 | 921.280 | 25.400 | 10.040 | 91.761 |
| compute | coherent | bilinear | 4 | 4035.400 | 55.200 | 17.120 | 235.713 |
| compute | coherent | mip-linear | 1 | 1171.520 | 36.720 | 10.000 | 117.152 |
| compute | coherent | mip-linear | 4 | 3262.720 | 53.480 | 11.520 | 283.222 |
| compute | random | Load | 1 | 195.920 | 12.920 | 11.840 | 16.547 |
| compute | random | Load | 4 | 1273.800 | 30.480 | 26.200 | 48.618 |
| compute | random | point | 1 | 346.040 | 16.920 | 14.960 | 23.131 |
| compute | random | point | 4 | 1300.360 | 31.720 | 25.600 | 50.795 |
| compute | random | bilinear | 1 | 1246.160 | 29.240 | 16.080 | 77.498 |
| compute | random | bilinear | 4 | 3398.920 | 42.160 | 21.000 | 161.853 |
| compute | random | mip-linear | 1 | 1375.200 | 28.520 | 14.880 | 92.419 |
| compute | random | mip-linear | 4 | 4382.520 | 59.760 | 25.480 | 171.998 |
| fragment | coherent | Load | 1 | 268.000 | 9.680 | 6.800 | 39.412 |
| fragment | coherent | Load | 4 | 1501.040 | 32.120 | 19.040 | 78.836 |
| fragment | coherent | point | 1 | 468.720 | 16.960 | 11.520 | 40.688 |
| fragment | coherent | point | 4 | 1523.920 | 33.680 | 18.880 | 80.716 |
| fragment | coherent | bilinear | 1 | 1239.960 | 27.200 | 11.640 | 106.526 |
| fragment | coherent | bilinear | 4 | 3873.240 | 42.680 | 14.680 | 263.845 |
| fragment | coherent | mip-linear | 1 | 1181.960 | 36.960 | 11.600 | 101.893 |
| fragment | coherent | mip-linear | 4 | 3205.640 | 45.760 | 8.680 | 369.313 |
| fragment | random | Load | 1 | 345.280 | 11.240 | 9.320 | 37.047 |
| fragment | random | Load | 4 | 1807.360 | 37.120 | 29.800 | 60.650 |
| fragment | random | point | 1 | 603.240 | 19.280 | 15.360 | 39.273 |
| fragment | random | point | 4 | 1808.240 | 37.840 | 28.680 | 63.049 |
| fragment | random | bilinear | 1 | 1572.960 | 33.400 | 17.000 | 92.527 |
| fragment | random | bilinear | 4 | 3477.200 | 37.040 | 17.440 | 199.381 |
| fragment | random | mip-linear | 1 | 1870.480 | 32.960 | 14.560 | 128.467 |
| fragment | random | mip-linear | 4 | 4869.320 | 60.760 | 21.960 | 221.736 |

El coste directo fue **16,547–369,313 veces** el control nativo en este corpus.
Es un cociente de kernels pequeños con un decoder inicial que decodifica pesos y
endpoints por tap; no predice FPS ni extrapola a Series. El resultado impide
integrar esta implementación por defecto. La funcionalidad ×1 de payload queda
demostrada en PC; rendimiento aceptable y coste residente completo siguen abiertos.

### Validación adicional y consola

La variante normal `D3D12_ASTC` del decoder conserva instrucciones SPIR-V idénticas
al original al eliminar registros debug/source; resultado en
`build-uwp/astc-regression/result.txt`. El build incremental UWP pasa.

GBV real: `20261002-193600-gbv` agotó 240 s antes de terminar validaciones;
`20261002-194240-gbv` agotó **900 s**. Este segundo ensayo completó ambas
comparaciones con los mismos errores máximos y cero mismatches/Error/Critical;
completó 20/32 benchmarks, pero carece de PASS/cierre/retorno 0: **gate incompleto**.
El runner archivó FAIL/timeout y paró únicamente su proceso sintético. Pico de
commit 5.365.018.624 bytes (5116,48 MiB), rozando Job 5120, conservado.
No se deduce de ese timeout un fallo funcional ni su causa exacta. La
[documentación GBV de Microsoft](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation)
explica que añade código a shaders/command lists y puede ralentizar mucho;
sus timestamps no se usan para rendimiento. La dependencia concreta de driver,
instrumentación y límite de memoria en este ensayo queda sin separar.

Un ensayo previo `20261002-193144-gbv` estaba mal etiquetado: la opción inicial
`gpu_validation=1` no existe en el frontend, por lo que ejecutó solo debug.
Se invalidó su resumen; los dos ensayos reales usan `debug_layer=gbv` y exigen
el marcador de activación. El script corregido evita -DebugLayer porque añadiría
`debug_layer=1` después y desactivaría GBV.

Smoke con `astc_sampling_probe` ausente: `20261002-1959-default-smoke`, PID23340,
Job5120, homebrew propio y debug layer. Sentinel, framebuffer y apagado automático
en 13,203 s, retorno0; cero Error de Render/Critical y ninguna creación del probe.
**Arranque/renderer PASS**, no prueba de FPS o identidad visual manual.
El log completo no está libre de Error: 1084 de Input por motores ausentes,
tres Core por metadatos NRO y tres BSD por red no inicializada; categorías
ya documentadas como ruido del frontend headless en `xbox_internal.md`.
Se conserva todo el log y su clasificación, sin tratarlos como errores ASTC.

El primer intento de smoke desde pwsh7 no pudo importar Appx; se repitió con
Windows PowerShell5 (`System32/WindowsPowerShell/v1.0/powershell.exe`) y pasó.
Invocar `powershell -File` para los scripts de AppContainer; no ejecutarlos dentro
de pwsh7 cuando el módulo Appx sea incompatible.
Series necesita ejecutar el mismo gate; el resultado PC no la certifica.

## Resultado y siguiente experimento

El éxito funcional de los bloques originales permite
continuar investigando; un coste alto de sampler directo impide integrarlo por
defecto. El siguiente experimento sería conservar ASTC más una caché decodificada
parcial y acotada. El límite debe cumplir:

`asignación_bloques + metadatos + caché + índices + feedback + auxiliares ≤ 2 * payload_ASTC`

Debe contabilizar alineación y consumo de shader/driver aparte, registrar misses
y usar feedback propio SM 6.4. No se ha implementado esa caché ni cambiado el codec.
Antes de integrar: el mismo gate en Series, presupuesto total real, semántica
guest completa y gameplay manual/T completas/cierre Q. JIT 115 MiB/core, límite
5120 MiB y las políticas de cachés/staging de gameplay permanecen intactos.


## Paquete de Series

Versión **0.2.71.0**, sin BOM en el manifest. Paquete firmado en
`build-uwp/astc-probe-series/eden-xbox.appx`, certificado y VCLibs en la misma
carpeta; exe/PDB archivados en `build-uwp/symbols/0.2.71.0`. Homebrew propio
incluido, sin dumps de juego, keys ni firmware. No depende del debug layer PC.

```powershell
tools/xbox/package-appx.ps1 -BootNro tools/xbox/boot_nro/boot.nro `
  -BootCfg @('astc_sampling_probe=1') -OutDir build-uwp/astc-probe-series
```

En Dev Mode, instalar ese paquete y poner la app en **Game**. Abrirla una vez:
corre el gate y se cierra sin entradas. Descargar `eden_log.txt` y
`eden_uwp_diag.txt` por Device Portal. Exigir `ASTC probe PASS`, ambas validaciones
sin mismatches, 32 benchmarks, `RunHeadlessBoot returned 0`, cierre y ausencia
de Error/Critical de Render. Comparar memoria y timestamps de consola por
separado. Esta entrega prepara el gate; no tiene ejecución ni aprobación Series.
Para volver al arranque normal, empaquetar sin `astc_sampling_probe=1`.

## Integración experimental autorizada para medir Game D

El usuario pidió después integrar y lanzar Game D con el límite de memoria.
`astc_guest_sampling=1`, desactivado por defecto, habilita almacenamiento de los
bloques ASTC originales y un importador del sampler GLSL en el SPIR-V guest de
fragment y compute. Mesa traduce el módulo combinado a DXIL SM 6.4. No modifica
el JIT 115 MiB/core, staging 256 MiB, Job 5120 MiB ni T480.

Esta primera integración conserva el layout block-linear original, incluido su
padding, en un buffer tipado R32G32B32A32_UINT con SRV acotado. Cada imagen añade
16 bytes de cabecera y 48 por mip. Los buffers son committed por imagen: la
alineación mínima de asignación puede elevar mucho el multiplicador de imágenes
pequeñas. **No hereda el ×1,096 del agrupamiento del gate aislado.** Agrupar o
deswizzlear a bloques compactos es una optimización posterior a medir esta ruta.

Cada draw/dispatch añade 96 bytes por binding al staging para parámetros de vista,
sampler, swizzle y sRGB. Soporta operaciones float 2D/2DArray sin MSAA, Load,
sample explícito, consultas de tamaño y número de mips. Hay implementación
experimental de sample implícito con derivadas en fragment; su equivalencia de
LOD y precisión de filtrado en gameplay sigue pendiente. Grad, gather, dref,
sparse, arrays de descriptors, anisotropía, reducción min/max y mirror-once
conservan toda la binding nativa. VS/GS/TCS/TES, cubemaps y 3D también usan la ruta
nativa. No se presenta esto como paridad completa del sampler.

Una operación incompatible materializa una textura compañera con el decoder
GPU anterior (BC3 si esa imagen ya lo admitía; RGBA8 en arrays). Se conservan los
bloques originales además de la compañera: esa imagen puede consumir **más**
que antes. Escrituras/copias invalidan el contenido original para muestreo hasta
el siguiente upload. Un error del importador preserva el shader original y su
binding nativa. Compilación/PSO y asignación reales aún deben observarse en Game D.

`ReportAstcGuest` registra payload compacto, padding guest, metadatos, asignación
de buffers, compañeras materializadas, bindings directas/nativas y memoria
app/DXGI. `baseline_native_alloc` es una estimación contrafactual del mismo
conjunto de imágenes, no una medición A/B del proceso. No incluye slack del pool,
recursos retirados, driver, shader ni memoria guest. La diferencia
`baseline_native_alloc - raw_alloc - fallback_native_alloc` solo describe esas
asignaciones lógicas; puede ser negativa. El monitor
`tools/xbox/measure-process-memory.ps1` registra commit y working set del proceso
cada 500 ms sin cerrar el juego. PC GPU dedicada no representa RAM unificada Series.

Gate integrado PC con debug layer, Job5120 y GBV desactivado:
`build-uwp/astc-probe-results/20261002-214527-debug` PASS, cierre y retorno0.
60717 consultas del decoder aislado y 22369 de control nativo conservan cero
discrepancias. El importador añade **7168 consultas por etapa compute/fragment**,
siete mips, ocho capas, GOB block-height log2 0–3, coordenadas negativas/clamp,
bloques complejos y SRVs RGBA nulos: cero discrepancias, error máximo
0,000000179 frente al oracle CPU. Los 32 benchmarks verifican también los valores;
en modo integrado la tercera ruta es el decoder importado, **no** un sampler
RGBA hardware. Las etiquetas históricas `median_rgba_native_us`/`ratio_native`
no deben interpretarse como comparación hardware en este modo; el log lo explicita.
Footprints adicionales, wrap/mirror/border arbitrario, sRGB del importador, offsets,
queries, LOD implícito y casos guest reales requieren gates específicos antes de
certificar calidad general. Series y gameplay/FPS permanecen pendientes.

Trampa resuelta: glslang emite un `BuiltIn WorkgroupSize` para el entry point
dummy de la librería (1×1×1). Importarlo hacía que Mesa sustituyera el LocalSize
del shader guest (8×8×1), aunque no se importara ExecutionMode. Eso dejaba gran
parte del output sin escribir y parecía corrupción de texturas nativas. Se omite
esa decoración; los grupos del guest se conservan. Las corridas fallidas anteriores
quedan archivadas y no certifican memoria ni calidad. La prueba con el shader
original y los mismos recursos adicionales pasaba y permitió aislar la causa.

Trampa del runner: casts PowerShell en `-ArgumentList` necesitan paréntesis;
sin ellos `[bool]False` llegaba como texto verdadero y activaba GBV. Se corrigió
el runner y se exige coincidencia entre GBV solicitado y marcador real del log.
La regresión del decoder D3D12 normal vuelve a pasar: instrucciones SPIR-V
idénticas a HEAD tras excluir registros de fuente/debug.

El smoke del renderer con el prototipo ausente pasó después de la integración:
`build-uwp/astc-guest-default-smoke{,-log,-diag}.txt`, retorno0, cierre en 11,813 s,
Job5120 verificado y cero Error/Critical de Render. Ruido headless Input/Core/BSD
se conserva en el log, como en el smoke anterior.

Primer lanzamiento Game D (`20261002-214816`) falló **durante precarga**, antes
de Run/gameplay: la caché previa contenía 13256 pipelines y el experimento las
recompilaba todas, aunque no se usaran. El proceso alcanzó 5368692736 bytes de
commit, prácticamente el cap5120; WER registra acceso inválido en Mesa. El intento
de cancelar ese arranque llegó después de ese fallo. No es una medición de ahorro ni gate de cierre
Q. Se conservan log/diag y memoria en `build-uwp/astc-game-d-results/`.
El frontend ahora omite esa precarga solo con `astc_guest_sampling=1`; compila
pipelines a demanda y mantiene la política normal cuando está desactivado. Un
A/B de memoria de arranque debe controlar esta diferencia de precarga por separado.

El segundo arranque, a demanda, llegó a Run y al primer ASTC 1024×1024×121,
11mips: buffer asignado 42663936 bytes, nativo contrafactual 681967616. Falló
Close con E_INVALIDARG. El relanzamiento debug confirmó mensaje525:
`ResourceBarrier: Before and after states must be different`. El primer upload
del buffer, recién creado en COPY_DEST, repetía COPY_DEST→COPY_DEST; se omite
esa barrera cuando no cambia el estado. Ambas corridas fallidas se archivan,
sin Q/retorno0 ni afirmación de ahorro real.

Relanzamiento upload corregido (`20261002-215555-uploadfix`, PID12528): pasó
ese upload y creó más pipelines, pero alcanzó 5368659968 bytes de commit y
falló a58,453s en Mesa con escritura a0x30. No se atribuye como fallo del
decoder GPU: ocurrió en traducción host antes de una medición completa.
El siguiente candidato serializa CompilePipeline solo durante el experimento
ASTC para acotar los temporales concurrentes; shader semántico y presupuestos
no cambian. No se presume que baste hasta observar el nuevo lanzamiento.

La serialización sola no bastó (`-serial`, PID16132): pico5368709120, acceso
inválido Mesa al traducir VS bfc4c0fb669a773a/PS d86343218f203bba después de
cargar guest DRAM. Sí registra primera binding directa y buffer original de
40,6875MiB frente a650,375MiB nativos contrafactuales, payload40,3383MiB:
asignación ×1,00866 del payload, diferencia609,6875MiB (93,744%). Esto no
certifica reducción de commit del proceso, calidad en pantalla ni gameplay.

Se añade un perfil **experimental** de pares VS/PS observados, por title,
`eden/cache/shader/<title>/astc_guest_warm.txt`, máximo128pares/256pipelines de
precarga. El cachebin normal aporta los entornos/shaders reales; las pistas no
sustituyen validación ni contienen DXIL. El par se persiste antes de la traducción
para poder precalentarlo antes de Run en el siguiente ensayo. Compute y pares no
observados compilan a demanda. La política normal sin ASTCguest no cambia.
La primera selección45pipelines pasó precarga14s, pero el shader fallido aún no
estaba en el perfil y volvió a alcanzar cap5120. La selección siguiente incorpora
ese par y evita compilarlo con guest DRAM ya ocupada; prueba aún pendiente.
Las diferencias de precarga y temporales deben separarse de ahorro de texturas.

Precarga del par fallido (`-warm2`, PID3792) a guest DRAM121MiB permitió traducir,
pero el PS resultante era2496951 bytes y el driver devolvió E_OUTOFMEMORY al crear
la PSO (6RT). También fallaron variantes de diagnóstico, salvo sin pixel shader;
no prueba por sí sola que la causa sea un RT. Pico5368209408 bytes. Se detuvo
esta precarga antes de Run; no había gameplay. No basta adelantar la compilación.

El helper ahora comparte **una sola llamada GuestLoad/decoder** entre fetch,
point, bilinear y ambos mips, con loops; evita que Mesa inline tres copias del
decoder por cada operación de textura del guest. No reduce resolución/canales
ni recodifica los bloques. Gate `20261002-221227-debug` PASS:7168 consultas por
etapa y las60717/22369 previas sin diferencias,32benchmarks,retorno0/cierre,
RenderError0. DXIL del sampler importado del ensayo437688→146628 bytes compute
y437750→146706 fragment; máximo error0,000000179, pico proceso741,453MiB.
El coste GPU sigue alto y no se deducen FPS. Relanzamiento Game D con este helper
y perfil de shaders observado pendiente de resultado.

El lanzamiento single-decode pasó precarga27,5s y creó la PSO anterior, pero
encontró más PSOs durante carga: E_OUTOFMEMORY incluso con PS187898bytes y
otras nativas. Llegó a bad_alloc a122,172s; no cierre Q ni ahorro total demostrado.
El candidato siguiente limita el importador a4 operaciones de sample/fetch por
shader (query no cuenta) y conserva el shader nativo completo si supera esa cifra.
También mantiene nativo el shader si al prepararlo hay menos de1GiB de margen
app para temporales de compilación. Es una restricción del experimento y reduce
cobertura ×1; las compañeras RGBA/BC3 cuentan en el reporte, incluido ahorro negativo.
No cambia resolución, canales, JIT, límite ni algoritmos de sampling admitidos.

Trampa del monitor: Get-Content podía bloquear brevemente `memory-summary.json`
y Set-Content hacía terminar el muestreo. CSV hasta ese punto sigue siendo válido,
pero `running=true` en el último snapshot podía quedar obsoleto. Ahora publica
snapshots mediante archivo temporal y reemplazo; si un lector impide reemplazar,
continúa muestreando y reintenta el siguiente tick. Los ensayos viejos con monitor
interrumpido no tienen pico completo certificado. Log/diag se conservaron aparte.
`tools/xbox/analyze-astc-memory.py` separa tamaños de recursos medidos, diferencia
contrafactual, memoria del proceso y exclusiones; nunca presenta esta diferencia
como un A/B de RAM total.

Gate candidato acotado `20261002-222444-debug` PASS con las mismas consultas,
32benchmarks y cierre0. ConstOffset y sample implícito compute conservan binding
nativa; el sampler con min/mag distintos también conserva la ruta nativa, hasta
verificar sus umbrales específicos. Evita habilitar semántica no ejercitada por
los fixtures. El helper directo sigue opt-in y con cobertura limitada.
## Resultado Game D integrado, límite 5120 MiB (2 oct, cierre del ensayo)

Evidencia: `build-uwp/astc-game-d-results/20261002-222748-bounded/` (log, diag,
CSV parcial, snapshot y `astc-memory-report.json`). PID9520, Job5120 verificado,
play1, fastmem0/cpu_profile0; JIT115MiB/core, staging256MiB y T480 conservados.
La última corrida llegó a Run y terminó a348,782s: menú abierto338,344s,
volver a biblioteca346,547s, shutdown348,641s, **retorno10**, no Q/retorno0.
RenderError/Critical0; esto no certifica calidad visual, FPS ni Series.

| Observación de recursos vivos | ASTC raw asignado | Nativo contrafactual | Copia nativa adicional | Diferencia frente al nativo |
|---|---:|---:|---:|---:|
| Array grande durante carga, 1024×1024×121/11mips | 40,6875 MiB | 650,375 MiB | 0 | 609,6875 MiB menos |
| Estado frecuente después de expulsar ese array, 3 imágenes | 1 MiB | 1,625 MiB | 1,625 MiB | 1 MiB más |
| Último conjunto no vacío, 5 imágenes | 1,4375 MiB | 2,125 MiB | 2,125 MiB | 1,4375 MiB más |

El primer caso conserva payload42297728 bytes con buffer42663936: ×1,00866
incluyendo padding/metadata/alineación de ese recurso. Es una comparación de
recursos con la expansión que se habría creado, **no 609,7 MiB menos de RAM total
medidos contra una corrida de control**. La cobertura actual se restringe por
operaciones/shader, tipo y sampler; al necesitar fallback se conservan ambos
recursos. Por ello el resultado posterior es ahorro negativo. Contadores finales
252bindings directas/107167nativas no equivalen a texels, bytes ni porcentaje de
texturas. Recursos retirados pendientes de fence y auxiliares siguen excluidos.

El diag completo registra hasta5063MiB de commit en muestras cada10s; últimas
ventanas rondan4896MiB. El monitor externo de esta corrida se interrumpió tras una
muestra: File.Replace recibió backup vacío por conversión de `$null` en PowerShell.
Su pico5010,54MiB solo cubre el periodo observado y no es el pico completo. El
analizador marca `process_snapshot_complete=false` aunque el snapshot antiguo diga
running=true, y conserva min/max y último conjunto no vacío antes de shutdown.
**No se ha demostrado reducción de commit del proceso ni RAM unificada Series.**
Una comparación total deberá controlar escena, precarga de PSOs, perfil JIT y
residencia; la selección experimental de shaders difiere de la ruta normal.

Correcciones posteriores al ensayo: descriptor ASTC y compañera se transfieren
explícitamente al mover Image; el objeto movido deja de poseer runtime/descriptor.
El move por defecto copiaba el handle y podía liberarlo al destruir el origen;
move assignment tampoco transfería campos ASTC. El monitor usa
`[NullString]::Value` en File.Replace y tolera bloqueo breve por lectores. No se
ha descartado el raw al materializar fallback: los costes anteriores siguen
representando la implementación actual.

Build incremental final pasa (objeto texture_cache+link). Smoke AppContainer normal
con debug/Job5120, opción ASTC guest ausente, sentinel/framebuffer y retorno0 a11,625s,
RenderError/Critical0. Evidencia `build-uwp/astc-guest-final-smoke/`; un segundo smoke
`astc-guest-monitor-concurrency/` valida monitor22muestras,182lecturas concurrentes,
snapshot final running=false, archivos copiados y sin error del monitor; retorno0
a11,656s. Lectores externos también deben tolerar un IOException breve durante
reemplazo. Esto valida arranque/monitor, no vuelve a certificar la ruta guest ASTC
con el último cambio de ownership. No nuevo Game D después del cierre humano.

El prototipo aislado y el sampler integrado permanecen opt-in/desactivados por
omisión. La muestra sintética acotada ya pasa consultas CPU/GPU y32benchmarks;
muestreo implícito general, anisotropía/gather/cubemap/3D guest y Series siguen
pendientes. El paquete Series aislado0.2.71.0 y sus símbolos anteriores permanecen
separados; no contienen esta integración posterior. Sin commit.
A/B manual ASTC2oct: predeterminada Job5120 PID14816 falló a65,453s creando textura
committed fallback con HRESULT0x8007000E, sinQ/retorno0. Monitor completo132muestras,
peakcommit4855345152B; fallar asignación no implica alcanzar el cap en última muestra.
Evidencia astc-ab/20261002-225357-default. Usuario cambia AMBAS pruebas a sin límite:
MemoryLimitMiB0, sin Job cap y sin presupuesto artificial de caches. Predeterminada
relanzada PID300, astc=gpu/guestSamplingfalse/play1/fastmem0/cpuProfile0/jitPrewarm1,
monitor activo, control manual hastaQ. Después usuario indicará prototipo, también
sin límite. No comparar esta fase con la corrida limitada como A/B equivalente.
Evidencia astc-ab/20261002-225729-default-unlimited, sincommit.
A/B sin límite predeterminada cerradaQ: PID300,156,454s Q tras115sRun, retorno0
159,141s/cierre159,172s. Monitor305muestras completo/runningfalse, peakcommit
7697985536B, maxsample7590395904B; evidencia/hash guardados en
astc-ab/20261002-225729-default-unlimited. Prototipo relanzadoPID16100 también sin
límite, astc=gpu+astc_guest_sampling1/play1/fastmem0/cpuProfile0/jitPrewarm1,
monitor separado activo; mismo recorrido manual yQ pendientes. Prewarm115/staging256
conservados; caches persistidas pueden variar y se separarán de ahorro ASTC al comparar.
Evidencia astc-ab/20261002-230116-prototype-unlimited, sincommit.
Usuario repite prototipo por compilación predominante: PID16100 conserva logs/diag/
CSV931muestras, peak8030654464B; diag termina480s sinQ/retorno0 confirmado. Archivado
como calentamiento en20261002-230116-prototype-unlimited, no comparación principal.
Relanzado prototipo PID10700 sin límite, mismas opciones/cachés persistentes y
monitor separado enastc-ab/20261002-231023-prototype-warm-unlimited. Control manual
hastaQ; predeterminada base20261002-225729-default-unlimited preservada con hashes.
Precarga experimental de pares observados no garantiza ausencia de nueva compilación.
Sincommit; resultado de esta repetición pendiente.
## Comparación solicitada: predeterminada frente a primer prototipo sin límite

El usuario pide registrar la precarga incompleta del relanzamiento como fallo y
comparar la primera corrida, aun dominada por compilación. Se conserva también
la repetición fallida, fuera de la comparación principal; no hay app viva.

| Medición PC | Predeterminada | Primer prototipo |
|---|---:|---:|
| Pico commit Windows del proceso | 7341,371 MiB | 7658,629 MiB |
| Máximo commit muestreado cada500ms | 7238,766 MiB | 7606,031 MiB |
| Pico working set | 5300,141 MiB | 5488,684 MiB |
| Muestras del monitor completo | 305 | 931 |
| Últimas dos ventanas de300frames | 27,86 / 29,99 FPS | 12,68 / 13,81 FPS |
| Pipeline stalls acumulados en ventanas no solapadas ReportPerfWindow | 0s | 258,761s de386,323s |
| Cierre Q/retorno0 confirmado | Sí | No |

**Resultado observado: prototipo +317,258MiB de pico commit (+4,32%). No hay ahorro
RAM total observado en estas corridas.** Duración, compilación y estado de caches
son distintos; no es una estimación causal del coste de ASTC puro ni un A/B con
shaders completamente calientes. PC commit excluye VRAM dedicada y no equivale a
RAM unificada de Series. Las ventanas de FPS incluyen compilación; no certifican
rendimiento estable. Datos/analizador reproducible en
`build-uwp/astc-ab/comparison-observed.json` y `compare-observed.py`.

En la última lectura ASTC del primer prototipo: raw57344000B (54,6875MiB), baseline
nativo864681984B (824,625MiB), compañera858718208B (818,9375MiB). Raw+compañera
873,625MiB: **49MiB extra** frente al baseline de esas imágenes. Se evitó609,6875MiB
transitoriamente en el array de carga, pero al necesitar sus copias nativas esa
ventaja desapareció. Contadores12098direct/185594native no son porcentajes de
texels o memoria. Ambos logs tienen RenderError0 y un DebugCritical FSA al escribir
un archivo (requested0x2335e0, actual0); no etiquetar las corridas globalmente limpias.
Prototipo termina diag480s sin Q/retorno0: causa del cierre no confirmada.

**Fallo registrado: precarga experimental incompleta tras repetir.** El código
actual limita a128pares VS/PS y256pipelines gráficos, lee primeros pares persistidos
y omite precarga compute. El log de la repetición confirma128pares; luego aparecen
stalls de6220,7ms y3585,9ms. Conservar cache no garantizaba precalentar todos los
shaders del recorrido. Es una limitación explícita de la política experimental,
no evidencia de que se hayan borrado todos los archivos de cache. Gate de segunda
corrida caliente fallido; pendiente resolver cobertura/tiempo/memoria de precarga,
sin ampliar automáticamente sus límites. Repetición20261002-231023-prototype-warm-
unlimited archivada separadamente,247muestras, tampoco Q/retorno0 confirmado.

Bases preservadas con hashes: predeterminada20261002-225729-default-unlimited;
prototipo20261002-230116-prototype-unlimited. Sin cambio de renderer ni commit al
hacer esta comparación. Ambas alternativas siguen disponibles; prototipo opt-in.
## Velocidad de ASTC Directo: workers y experimentos (3 octubre 2026)

El usuario pide acelerar el decoder y aprovechar varios hilos. Se distinguen dos
costes: compilar el decoder importado en cada pipeline y ejecutar su muestreo en
la GPU. El segundo ya es paralelo entre invocaciones GPU; añadir hilos CPU no
elimina la repetición del decode por texel, vecino bilinear o mip.

Cambio conservado: cuando ASTC Directo está activo, la compilación de pipelines
que no importan el decoder puede volver a usar los cuatro workers existentes,
si AppFree >= 4 GiB. Antes el mutex experimental serializaba también esas
traducciones. Los pipelines con decoder importado y los casos sin ese margen
conservan la protección de memoria transitoria. El permiso es una opción de
scheduling, excluida de la clave semántica del shader. ASTC Nativo no cambia.
No se añaden copias RGBA, threads permanentes, presupuesto JIT ni límite RAM.

Gate aislado final: `build-uwp/astc-direct-speed/final-safe/20261003-001213-debug/`,
AMD Radeon Pro 5300M, AppContainer, SM6.4, debug layer, Job5120, sin GBV. Cuatro
compilaciones del shader de referencia: serial384,633ms, paralelo135,798ms,
**2,832x**, DXIL idéntico byte a byte. AppFree4561657856B. Es un shader pequeño:
no certifica tiempo de compilación de Game D, velocidad de decode, FPS ni Series.
El pico commit del proceso aislado fue839643136B (800,746MiB), separado de VRAM.

Se probaron cache de estado preparado, decode parcial de pesos, reutilización
2x2 dentro de un bloque, omitir segundo mip de peso cero, unquantización entera
y extracción directa de colores constantes. Ninguno se conserva en el decoder.
Por ejemplo, reutilizar el quad redujo bilinear fragment/random1sample de2451,28
 a635,68us (3,86x), pero compute/coherent/trilinear1sample subió474,96->493,96us.
La compilación del helper importado subió aproximadamente1,16/1,19s a3,8/4,2s
compute/fragment; otras variantes presentaron el mismo compromiso. Estas son
mediciones sintéticas, no FPS ni una mejora general aceptada. Las tablas están en
`build-uwp/astc-direct-speed/comparison{2,3,4,6,7,-final}.json`; fuentes y binarios
de ensayo se conservaron fuera de Git. `after8` no es una variante válida: la
restauración mantuvo un mtime antiguo y Ninja reutilizó el shader previo.

Ahora el probe separa translate+sign de PSO. Con el helper original importado
(etiquetado `rgba_native` por compatibilidad cuando GuestIntegration está activo):
compute1136,741+41,783ms y fragment1127,982+41,638ms. La ruta `direct` del probe
es el sampler aislado anterior, no el helper guest; sus6,6--6,9s no deben mezclarse
con los1,1s del helper importado. El tiempo antiguo `translate_sign_pso_ms` no
permitía atribuir el coste exclusivamente a Mesa o al driver.

Validación ampliada: 17 imágenes UNORM,14 footprints, mips, capas, bordes de bloque,
constantes, modos inválidos y arrays blocklinear,23552queries por etapa CS/PS,
cero discrepancias, error máximo1,79e-7. Mantiene60717queries del sampler aislado,
22369 de referencia nativa y7168 del helper guest. Añade cuatro benchmarks de
colores constantes,4096queries cada uno, point/bilinear CS/PS. No atribuir a este
nuevo gate UNORM cobertura guest sRGB/cube/3D: esas rutas y sus límites anteriores
siguen documentados arriba. Las32 combinaciones originales de benchmark pasan.

Error aprendido: cambiar corpus del probe reemplazaba recursos con copias aún
pendientes en la command list. En `constant-before/20261003-000648-debug` provocó
error921 y device hung0x887A0006; no usar esa corrida para certificar el gate.
`UploadGuestBacking` ahora termina sus copias antes de que otra carga sustituya
los recursos. El gate final pasa, RenderError/Critical0 y retorno0. La barrera del
test paralelo también libera participantes ausentes si falla crear un worker.
Al restaurar GLSL de una copia, actualizar mtime explícitamente antes de Ninja.

El decoder GPU original queda conservado: **no hay aceleración de decodificación
demostrada en la versión final**. Siguiente foco: compartir el trabajo por bloque
sin inflar la traducción, o medir la cache parcial acotada ya diseñada. Wave/quad
requiere comprobar capacidades y correspondencia de lanes; no asumir que un
compute quad de SM6.4 corresponde a2x2 texels. Referencia Microsoft:
https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/hlsl-shader-model-6-0-features-for-direct3d-12
Bloques/colores/pesos ASTC: https://github.com/KhronosGroup/DataFormat/blob/main/astc.txt

Gameplay manual, coste de compilación de Game D, presión real en precarga y gate
Series quedan pendientes. Las dos alternativas son ASTC Nativo y ASTC Directo;
A/B anterior sin límite sigue archivado intacto. Sin commit.
Gate final Nativo: build incremental correcto, SPIR-V no-debug del decoder normal
idéntico, smoke homebrew debug11,516s/retorno0/Render0/Job5120 y sin app viva.
Artefactos final-default-smoke-{launcher,log,diag}.txt en astc-direct-speed.
Paquete Series ASTC Directo3oct solicitado: rama xbox-d3d12-astc-directo,
versión0.2.72.0, biblioteca/play1, Game D Game E y Game G
Game G incluidos desde los dumps locales. Nuevo parámetro -LibraryGames permite
incluir varios juegos sin game= ni selección automática. Config: astc=gpu,
astc_guest_sampling1, memory_limit_mib5120 (presupuesto caches; el límite OS de
Series lo impone la consola), jit_prewarm1, fastmem0/cpu_profile0/hotkeys0;
sin debug_layer, probe, duración ni entradas programadas. Build al día (no work).
MakeAppx y firma SignTool correctos, APPX22011659592B (~22GB/20,50GiB), certificado
yVCLibs junto al paquete en build-uwp/package-series-astc-directo-0.2.72.0/.
Inspección ZIP verifica2NSP, versión/config/DLLs/firma presente y hash del EXE
idéntico a los símbolos archivados build-uwp/symbols/0.2.72.0/. Informe package-info.json.
Keys/firmware/juegos solo dentro del paquete local ignorado, nada añadido a Git.
No instalación automática ni gate gameplay/Series: pendiente prueba humana con
modo Game en Device Portal. Sin commit ni push; no afirmar mejora RAM/FPS.
## Fallo Series Game D0.2.73.0 (3oct2026)
Logs entregados en Descargas, archivados sin alterar en
build-uwp/astc-series-results/20261003-0.2.73-game-d-oom/. Esta corrida es0.2.73.0,
con biblioteca y ROM desde eden-usb, posterior al paquete0.2.72 de este chat.
USB preflight passed, tamaño17150648656 y system.Load status0 a+52,937s;
Run emitido a+52,953s. La app aborta a+144,390s, sin Q ni retorno0.

Causa inmediata confirmada: runtime_error "D3D12: Create committed texture
fallback failed (HRESULT0x8007000E)", hilo1648; excepción escapa del hilo y
abort(). Antes CreateHeap también falla0x8007000e. No es un fallo de apertura
USB/NSP en esta cadena; la lectura inicial y carga del juego sí pasaron.

Cadena ASTC: a110,091s array1024x1024/121capas/11mips original42663936B
(40,6875MiB), baseline nativo682033152B(650,4375MiB). Se parchean varios shaders,
pero a128,219s uno tiene13calls y a142,306s otro5calls, excediendo el cap4:
conservan shader nativo. A142,494s EnsureAstcNative crea realmente la compañera
650,4375MiB (heap650MiB), además del original. Desde142,495s se repite el guard
AppFree<1GiB que conserva más shaders nativos. Pool termina1034MiB reservados.
A144,048s se registra otro array512x512/121capas/10mips, raw10944512B ybaseline
174522368B(166,4375MiB); inmediatamente después falla CreateHeap y committed.
La segunda baseline es contrafactual: el log no registra que su copia de166MiB
llegara a crearse. No afirmar el tamaño exacto de la asignación fallida sin otro
registro o stack simbolicado.

Última muestra heartbeat:3993--3994MiB de5120 a+139,406s, anterior a la compañera
650MiB; no es el pico en el instante de abort. No sumar AppMemory yDXGI sin probar
cómo los contabiliza la consola. No atribuir el fallo a alcanzarse exactamente
5120MiB; se confirma E_OUTOFMEMORY en asignaciónD3D12 bajo presión, con memoria
unificada y muestras anteriores al salto. La ruta×1 solo se mantuvo parcialmente:
el fallback recrea las texturas expandidas y elimina gran parte del ahorro.

Pendientes de corrección: cubrir esos shaders sin crecimiento peligroso de coste
compilación/transitorios, evitar duplicación nativa bajo presión manteniendo
semántica guest y manejar fallo de asignación sin excepción fatal del renderer.
Subir cap de llamadas sin validar compile/RAM/GPU no constituye una solución
probada. Gate Series Game D falla; sin gameplay/FPS/ahorro certificado. Este turno
solo diagnostica/archiva; no modifica runtime, límites, paquete ni commits.
## Corrección del arranque Series: candidato, 3 oct 2026

La medición aislada bajo Job5120 dejó aproximadamente1152MiB libres. Cinco sitios
ASTC importados en compute/fragment pasaron128consultas por etapa, con incremento
observado del pico de commit de unos610MiB. Trece sitios importados fallaron al
crear la PSO compute con0x8007000E. Evidencia:
build-uwp/astc-series-recovery-probe-fixed/20261003-210207-debug/.
Por tanto se descarta aumentar simplemente el cap a16. La primera variante del
fixture expandía el fetch volumétrico y no el binding5: no cuenta como gate de5.

Candidato: máximo5 sitios de decoder, selección por binding completo y prioridad
para arrays, después menor coste/número de binding. Los otros bindings retienen
la semántica nativa. Esto puede conservar un array Directo en un shader de13
sitios, pero un único binding con más de5 sitios todavía queda nativo. No promete
eliminar todas las compañeras ni lograr×1 global. El nuevo fixture verifica5
sitios totalmente Directos y13 con selección mixta; resultado pendiente.

Se elimina el guard AppFree<1GiB que cambiaba el SPIR-V a nativo bajo presión;
la selección ya no depende de esa muestra transitoria. La traducción importada
se serializa y exige768MiB de margen antes de iniciarse: si no hay margen solicita
cierre controlado, sin reinterpretar el shader como nativo. Ese umbral es una
precaución basada en el ensayo PC, no una garantía de presupuesto en consola.
DXIL ya cacheado no requiere esa traducción. Calidad y límites5120/JIT115 siguen.

Recuperación en desarrollo: el hilo GPU UWP notifica excepciones al frontend,
libera el fence del callback fallido y solo atiende ticks de sincronización
posteriores hasta cerrar. TickWork libera/notifica su waiter mediante scope guard;
al cerrar se descartan callbacks pendientes y se espera al callback activo antes
de devolver waiters. Host1x puede cancelar esperas en teardown sin avanzar counters.
La inyección de excepción confirmó la liberación de dos waiters, pero varias
corridas quedaron bloqueadas en CloseServices; no se certifica recuperación.
La instrumentación encontró servicios aún registrándose mientras CloseServices
esperaba destructores con server_lock ocupado. Candidato adicional: cerrar registro,
mover managers fuera del lock, solicitar stop a todos antes de join y ejecutar
la notificación de LoopProcess también para servidores rechazados durante startup.
Compilación y gate de cierre pendientes; las corridas colgadas se archivaron antes
de terminar exclusivamente sus procesos sintéticos. No se interrumpió gameplay.
Gate PC candidato, 3 oct 2026: build incremental UWP correcto (216 operaciones
por header de servicios y 6 finales). La barrera de cierre de servicios resuelve
el bloqueo de la inyección inmediata: dos waiters completados, shutdown344ms,
RunHeadlessBoot0 y worker joined. Evidencia gpu-failure-recovery-barrier-pass/.
Se inyectó runtime_error en callback, no OOM real ni device removed; consola y
retorno interactivo a biblioteca aún pendientes. RenderError esperado de inyección.

ASTC selective-recovery-probe/20261003-213558-debug/ pasa el runner completo:
60717 consultas raw,22369 nativas,7168 guest y23552 corpus adicional por etapas,
32 benchmarks sin inferencia FPS. Cinco sitios importados:256consultas CS/PS,
ambas SRV nativas null, cero diferencias; pico AppUsed4806311936B desde4167045120B.
Trece sitios originales: binding5 nativo de12 sitios y binding6 Directo de1,
256consultas CS/PS, cero diferencias y pico4681486336B. No son13 decoders Directos.
Ambos dejan~1145MiB antes de compilar y usan admission control768MiB. Registro
AppUsed del device y PeakPagedMemory del monitor son contadores distintos; no
mezclar sus picos. Runner passed=true/errors=[],Job5120,retorno0. Game D arranque
automático180s sin entradas y conJob5120 en curso, no gate de gameplay/FPS.
Revisión del gate Game D: el umbral fijo768MiB abortó preventivamente a+209s
(durante Run, antes de demostrar asignación fallida), retorno15 y shutdowncorrecto.
No certifica arranque. Pico del monitor5365297152B (~5116,75MiB), muestreado
5256011776B; no afirmar margen suficiente a partir de heartbeats anteriores.
Evidencia game-d-selective-bounded-startup/. Se retira esa admisión rígida: el
Device en PipelineOptions ahora solo registra Warning bajo768MiB, sin cambiar
SPIR-V ni solicitar cierre. Conserva mutex/cap5/selección por binding. Fallos reales
DXIL de pipelines importadas y PSOOOM notifican al frontend; tras PSOOOM no se
crean variantes diagnósticas adicionales. Build incremental11ops correcto;
segundo gate Game D5120/Run120s en curso. Paquete0.2.74 inicial no es artefacto final;
se debe reconstruir y actualizar símbolos antes de entregar. Ninguna instalación
Series ni nuevo commit/push. La RAM dedicada PC sigue separada de commit del Job.
Segundo ensayo real5120: el guard solo Warning no es seguro. Run empieza311,672s
tras pausa host~128s (38 hilos Suspended, CPU fijo, reactivación mismaPID12868).
A339,411s y342,060s CreateGraphicsPipelineState falla0x8007000E; el frontend
inicia shutdown a339,734s pero aún se están compilando más pipelines. Pico exacto
Job5368709120B y access violation write0x8 en DLL a342,593s, sin retorno0/15.
No atribuir la DLL a Mesa/AMD sin simbolicar; el fallo se observa con Jobagotado.
Evidencia game-d-selective-real-bounded-startup/. No se certifica OOM real: solo
la excepción sintética y el cierre preventivo anterior pasaron. Paquete de esta
variante tampoco debe entregarse como validado.

Candidato siguiente: restaurar admisión768MiB (fallar antes de entrar al driver,
sin expansión) y ejecutar la construcción runtime Directa síncrona hasta PSO,
además de esperar cada pipeline. El mutex de CompilePipeline por sí solo no
serializaba CreateGraphicsPipelineState ni impedía cargar más recursos al omitir
los draws aún no compilados. Ruta Nativa mantiene settings/workers anteriores;
precache de Directo sigue en workers del lector, antes deRun. Esto cambia el pacing
de compilación y puede aumentar stalls: ningún beneficio FPS prometido. Build/gates
pendientes. Es una corrección de seguridad, no prueba de que Game D quepa en Series.
Gate frío síncrono5120 sin perfiles PC: ShaderPrecaching0/JitPrewarm0 son opt-outs
solo de diagnóstico; archivos no borrados, límite/código115 intactos. Console0.2.73
registró cero perfiles por core y cero pareswarm, por lo que el PC precalentado
anterior tenía una diferencia relevante. Run+1,422s, parada preventiva83,750s con
664MiB libres, shutdown85s/retorno15 y sin AV. Pico4916711424B (~4688,94MiB).
No arranque certificado ni ahorro: todavía no ejerció el array grande. Evidencia
game-d-selective-cold-bounded-startup/. El guard y pacing síncrono recuperan cierre;
no arreglan por sí solos el coste de importar decoders en texturas no ASTC.

Candidato especialización por bindings realmente ASTC: base SPIR-V/DXIL nativa,
con la misma clave/formato de caché de disco. Antes de cada draw/dispatch se leen
los handles/TIC actuales con las mismas reglas de combinación de handles que
Configure; índices TIC fuera de límite no activan ASTC. Máscara por índices de
texture_descriptors elegibles (count1/float/noMS/2D oArray2D), filtro explícito
para PatchAstcGuestShader. Variantes RAM por pipeline base y lista ordenada de
bindings activos; se reevalúa en cada uso aunque no cambie estado fijo/shader,
por lo que rebind nativo/ASTC no reutiliza una variante equivocada. Base y variantes
conservan root/layout y semántica sampler original; archivos/shaders previos no
se purgan. Precache prepara bases nativas; los imports Directos se construyen
síncronos al encontrarlos. Reduce imports inútiles, no promete mejora FPS.
Arrays con único binding>5 sitios y operaciones incompatibles siguen nativos;
no afirmar eliminación general de compañeras. Retener base/variantes cuesta PSO/DXIL
y el conjunto no tiene todavía GC propio (igual que la caché base de pipelines).

Compilación MSVC detectó acceso a GetTICEntry privado y traslado/copia de std::map
con unique_ptr por el contenedor F14: se usa lectura TIC acotada mediante MemoryManager
y std::map para la tabla de variantes. Build UWP correcto. Fixture GPU nuevo con
máscaras0/1/2/3/0/2: en0 el SPIR-V queda byte idéntico y no importa biblioteca;
SRV nativas null únicamente en los bindings Directos. Primera corrida falló por
usar heap descriptor antes de crear recursos en constructor del fixture, no en
renderer: se corrigió comprobando su existencia. Full gate de máscaras en curso;
arranque Game D y paquete final pendientes. No nuevo commit/push.
Gate especialización GPU: astc-specialization-probe-fixed/20261003-221735-debug/
passed=true,Job5120,errors=[],retorno0. Se preservan60717/22369/7168/23552casos,
5sitios y13mixto; máscaras0/1/2/3/0/2,256consultas por etapa, sin diferencias fuera
de tolerancia nativa. En máscara0 el SPIR-V es byte idéntico, DXIL~7KiB frente
~146KiB con imports genéricos. Esto no mide cache de variantes de renderer ni FPS.

Game D con perfiles PC precargados: precache1,015s frente~160s previo (cambio de
trabajo: ahora bases nativas, no A/B general ni mejora GPUdecode). Array1024/121
queda42663936B raw,681967616B baseline y0fallback,26bindings directas/0nativas al
último contador. ~609,69MiB menos en ese recurso contrafactual; no ahorro medido
RAM total versuscontrol ni certificaciónXbox. Se detiene porguard a56,250s,
shutdown59,859/retorno15. Pico5080662016B (~4845,3MiB). Gate arranque no pasa.
Evidencia game-d-specialized-bounded-startup/.
Frío sinprecargas sigueparada preventiva~54s,retorno15/cierrecorrecto,66bindings
Directas/0nativas,pico4811591680B. game-d-specialized-cold-bounded-startup/.

EnsayoHeapOptimizeResources: HeapSetInformation(NULL,HeapOptimizeResources,&info)
conVersionHEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION/Flags0. Microsoft documenta
optimizar cachesLFH de todos los heaps y decommit cuando sea posible; la función
incluye UWP, pero la página de la estructura tiene etiqueta desktop, por lo que
no certificamos supportSeries hasta gate. Fuentes:
https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapsetinformation
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-heap_optimize_resources_information
AppContainerPC devuelve success=true/error0, ahorro~0,35--3,4MiB en intentos,
284--602us observados. No rescata768MiB yGame Dretorna15(~50,8s). No sumarlos para
inventar ahorro acumulado: hay asignaciones concurrentes. Log/diagarchivados de
game-d-specialized-heap-cold-startup antes de siguiente prueba; monitor no arrancó
a tiempo en ese ensayo. Solo bajo presión, sin expulsar recursos usados, APIs
fallidas conservan guard. GateConsolependiente.

Candidato presupuesto por sitios realmente importados: query-only no necesita
presupuesto de decoder; de1a5 sitios exige384/480/576/672/768MiB. Es una política
conservadora a validar, no bound formal de cualquier shaderNIR/driver. Máximo5 y
serialización hastaPSO en runtime,LFHantesde rechazar. Nuevo gate aislado deja512MiB
libres bajoJob5120 y compila solo la variante de1sitio (CS/PS), luego comprueba256
texels en cada etapa y registra pico. Los otros codecs de referencia no se
recompilan en esa ventana: no atribuir sus transitorios al decoderimportado.
Renderer ahora comprueba Handle antes deConfigure; unaPSOfallida no debe crear
texturas/companeras para un draw/dispatch que no podrá ejecutarse. Ruta normal
conPSOvalida no cambia. Build incremental15ops correcto, GPUgate en curso;
Game D/paquete final todavía pendientes. No nuevo commit/push.

### Recuperación ASTC Directo: gates PC del 4 de octubre de 2026

La especialización por texturas realmente enlazadas evita importar el decoder en
shaders de texturas ordinarias. La ruta de un binding no cambia por la memoria
libre: si falta presupuesto se solicita cierre controlado; no se crea entonces
una compañera RGBA8 como supuesto rescate. Las variantes se conservan en RAM y
la base nativa permanece para los draws sin ASTC. No hay todavía GC propio de
variantes ni persistencia DXIL de estas variantes.

El gate aislado astc-site-budget-v2-probe/20261003-224907-debug pasa con debug
D3D12, SM6.4 y Job de 5120 MiB: 60717 consultas directas, 22369 nativas, 7168
guest, corpus adicional, máscaras 0/1/2/3/0/2 y shaders de cinco sitios o trece
sitios con selección parcial. La prueba final de un sitio, CS y PS, deja
261267456 bytes libres, valida 256 consultas por etapa y observa un incremento
máximo de app_used de 69943296 bytes. No mide gameplay ni certifica Series.

La política anterior de 384 MiB para un sitio detuvo Game D a 59,17 s aunque el
array 1024x1024x121 ya estaba en ASTC original. El cierre terminó correctamente
con retorno 15; pico commit 4936429568 bytes. Evidencia: game-d-site-budget-cold/.
La política candidata actual exige 192/336/480/624/768 MiB para uno a cinco
sitios. Sigue siendo una heurística, no un límite formal de todo shader/driver.
El test aislado anterior avala su caso de un sitio; el arranque real frío de
Game D con esta política sigue en curso. No hay resultado Series ni nuevo commit.

### Pausa solicitada por el usuario (4 octubre 2026)
Se mantiene la precarga JIT de 115 MiB/core: el usuario rechaza desactivarla para
hacer arrancar Game D. El arranque frío sin precarga llegó al menú bajo 5120 MiB,
pero no satisface este objetivo. En esa corrida hubo cuatro errores RTV; el
candidato de copias de reinterpretación por familia pasa el gate de datos GPU y
una corrida posterior no registra esos errores 613, aunque falla por presupuesto
de compilación. Esto no certifica un arranque completo con JIT ni la Series.

La recuperación del staging libera el ring de 256 MiB tras completar sus fences;
el gate GPU comprueba datos, readback pinned y nueva asignación. En Game D con JIT
115 MiB/core liberó aproximadamente 257 MiB de app_used, pero el siguiente shader
de dos sitios se detuvo con unos 304 MiB libres frente a los 336 MiB exigidos.
La corrida se bloqueó al cerrar durante un experimento de omisión de submissions;
se archivaron hung-log.txt/hung-diag.txt en game-d-jit-staging-reclaim/ y se terminó
únicamente su proceso automatizado. Esa omisión se retiró. El candidato actual
cancela las esperas GPU antes de Pause cuando se ha reportado un fallo GPU.

Compila el candidato con umbrales 192/256/384/576/768 MiB para uno a cinco sitios
ASTC. Incluye un gate de presión de uno y dos sitios con consultas que ejercen
realmente las bindings Directo; la consulta de presión anterior de un sitio
muestreaba la binding nativa, por lo que solo avalaba su coste de compilación.
El nuevo gate y el cierre corregido aún NO se han ejecutado. Último build correcto:
build-uwp/astc-two-site-pressure-fixed-build.txt; diff --check correcto.
Se pausa a petición del usuario, sin builds ni eden-uwp activos. Cambios guardados
sin commit en xbox-d3d12-astc-directo. El paquete 0.2.74 existente está desactualizado:
no entregar ni certificar Series; pendientes gates de presión/cierre y Game D con
precarga 115 MiB/core dentro de 5120 MiB antes de reempaquetar.
### Reanudación: presión de compilador y cierre GPU (4 octubre 2026)
Con el EXE del build astc-two-site-pressure-fixed-build.txt, el gate aislado
astc_sampling_probe=pressure pasa: 256 consultas por etapa compute/fragment,
un sitio y dos sitios ASTC realmente muestreados, cero discrepancias. Un sitio:
app_free_before=260640768, peak_app_used=5180293120; dos sitios:
app_free_before=297172992, peak_app_used=5188280320, límite=5368709120 bytes.
Debug sin errores Render/Critical y retorno0/worker joined en22,55s. Evidencia:
build-uwp/astc-pressure-resumed/. Este corpus avala esos casos, no todo shader.
El gate gpu_failure_probe=1 pasa también con cancelación antes de Pause: libera
waiters, termina shutdown y retorna0/worker joined en1,53s. Evidencia:
build-uwp/gpu-recovery-resumed/. Un fallo inyectado no certifica OOM real.
Se lanza Game D con prewarm1/115MiBcore, ASTCDirecto, debug y Job5120 verificado;
prueba automatizada de arranque120s, sin inputs ni certificadoFPS. PID6588,
monitoreo en build-uwp/game-d-jit-resumed/, resultado aún pendiente.
### Precarga de variantes Directo antes de llenar DRAM (4 octubre 2026)
Game D con115MiB/core aún falla con el build de presión: a56,52s un shader de dos
sitios queda con255684608 bytes libres tras reclamar LFH, menos que256MiB. Esta
vez el cierre real funciona: retorno15/worker joined61,36s, sin cuelgue. Evidencia
game-d-jit-resumed/. No es gate de arranque ni de FPS.

La precarga anterior solo regeneraba bases nativas; los imports Directo seguían
ocurriendo durante gameplay. Ahora astc_variants_v1.bin guarda hasta256 hints con
clave PSO completa y máscara de índices locales de descriptores, antes de intentar
compilar. No guarda DXIL ni blobs del driver. Reader valida magic/tamaño de clave,
conteos<=256/128, orden, unicidad, índices<128, truncación y EOF. El cache guest
normal suministra los entornos; cada variante se regenera para el dispositivo.
ASTCDirecto carga en serie antes de JIT/Run, evita claves duplicadas y conserva
las variantes por clave/máscara para la selección por TIC real. Nativo conserva
su carga anterior en workers. No cambia resolución, canales ni presupuesto JIT.

Gates: build8ops correcto, harness desktop astc-warm-hint-test PASS (fuentes en
tools/xbox/tests/astc-warm-hints.cpp). El harness UWP standalone no pudo arrancar
por CRT APP; se recompiló con vcvars64 desktop, sin cambiar el build de la app.
Primera corrida registra12 hints y falla/retorno15; segunda los precarga en~6,6s,
supera el shader anterior y registra tres variantes nuevas; siguientes corridas
precargan15/16 hints y fallan con el mismo PS06522df295ca2121 ligado a nuevos VS.
Todas cierran con retorno15; son fallos registrados, no pruebas satisfactorias.
Evidencia game-d-variant-learning/, game-d-variant-warm{,2,3}/.

Candidato siguiente aprovecha máscaras ya observadas del mismo PS para regenerar
combinaciones distintas que existen en el cache guest, con cap128 variantes
adicionales. Se conservan claves PSO completas y solo se usa una variante cuando
su máscara coincide con los TIC reales: preparar un candidato no fuerza su uso.
Build astc-related-variant-prewarm-build.txt correcto. Primera corrida precarga42
variantes, supera los anteriores VS pero falla con otro PS2c023d8e8adb27ae y guarda
su hint (game-d-related-variant-warm/, retorno15). Segunda corrida en curso con ese
hint adicional, siempre Job5120 y prewarm115. Dependencia de cobertura del perfil,
RAM extra de PSOs retenidos y shaders de áreas nuevas siguen siendo limitaciones;
no soluciona por sí sola un primer arranque sin caché. Series aún pendiente.
### Gate de arranque con JIT115 y paquete Series0.2.75 (4 octubre 2026)
Game D llega al menú en PC con ASTCDirecto, JIT115MiB/core y Job5120. Corrida
game-d-related-variant-warm2/: 52 variantes precalentadas, cero imports Directo
nuevos durante Run; 120s de ejecución, retorno0/worker joined152,4s, RenderError0
y debug613=0. Capturas frame_2/frame_3 verificadas visualmente. Pico de commit
OS5270110208bytes=5025,969MiB, margen94,031MiB. Presupuesto115 aplicado a cada uno
de tres cores con perfil; ninguno se desactivó. Esto no certifica FPS/gameplay.

Se añade baseline separada astc-seed-v1: paquete puede incluir selección ASTC,
entornos guest d3d12.bin y registros JIT, nunca blobs PSO/host code. Shader loader
combina hints locales primero y lee la baseline adicional sin reemplazar el cache
normal; claves duplicadas no regeneran PSOs. JIT suplementa perfiles incompletos
con baseline validada por título/BuildId/core/hash y RX; registros locales tienen
precedencia en conflictos, cap262144 y115MiB/core siguen iguales. Baseline vive
en LocalState/eden/cache/astc-seed-v1, distinta de las cachés aprendidas. Se refresca
solo esa baseline al arrancar con paquete que la incluya, incluso con igual tamaño:
perfiles con262144 registros suelen conservar tamaño aunque cambie su contenido.
Copia111MiB de datos de disco observada62ms PC; no confundir con111MiB RAM retenida.

Gate de paquete con cachés normales de Game D apartadas temporalmente pasa:
build-uwp/astc-seed-clean-gate/. Carga262144 registros/core desde baseline, local0,
prewarm115MiB/core,52 variantes Directo preparadas y0 nuevas durante Run. Menú
verificado en frame_3.bmp,120s/retorno0/worker joined153,55s, RenderError0/debug613=0.
Pico5294796800bytes=5049,512MiB, margen70,488MiB. Las cachés originales se restauran
al terminar y sus seis hashes coinciden con la instantánea previa; caches generadas
por el gate quedan archivadas aparte. No hubo input programado ni gameplay.

El máximo recurso ASTC evitado frente al baseline nativo contrafactual es
639959040bytes=610,31MiB en esa corrida; al final son solo655360bytes=0,625MiB,
porque el atlas grande ya salió de la caché. No hay ahorro global de proceso
medido por A/B. En PC la GPU dedicada queda fuera del cap de commit; Series UMA
no está certificada por este gate. El margen70MiB es pequeño para escenas nuevas.

Build de baseline7ops pasa; smoke ASTCNativo homebrew debug11,55s/retorno0/Render0.
Ajuste final de refresh baseline3ops pasa y gate GPUfailure con baseline ya presente
recopia3+3 archivos, pasa cierre/retorno0 en1,36s. CPU hint parser PASS, parserPS
local-run/package y diffcheck pasan. local-run ahora rechaza parámetros desconocidos
(CmdletBinding), evitando que -RunSeconds150 se ignore y use defaults inadvertidos.

Paquete final firmado0.2.75.0: build-uwp/package-series-astc-startup-0.2.75.0/eden-xbox.appx,
45928232bytes (~43,8MiB), biblioteca/play1/ASTCDirecto/prewarm1/5120/fastmem0,
sin debug/probes/duración/juego automático. Incluye6 archivos de baseline Game D;
no ROMs,keys,firmware. Verificados ZIP/config/versión/firma presente, SHA de seed,
EXE igual al build y símbolos0.2.75.0. package-info.json registra los hashes.
0.2.74 previo sigue desactualizado: usar0.2.75. No instalación automática; usuario
instala en Device Portal modoGame y devuelve logs Series. Arranque/gameplay/UMA
Series y mejoraFPS siguen pendientes. Todo sin commit/push en xbox-d3d12-astc-directo.
### Series 0.2.75: precarga correcta, DEVICE_HUNG y abort (4 octubre 2026)
Logs de Descargas archivados en build-uwp/astc-series-results/20261004-0.2.75-game-d-device-hung/.
Paquete0.2.75, adapter SraKmd_arden; preflight USB17150648656 bytes y Load0 pasan.
52 variantes ASTCDirecto precalentadas antes de Run: cache termina a116172ms del
frontend; JIT conserva115MiB en cada core0/1/2, accepted255426/256119/258460,
rejected0, budget skipped6718/6025/3684; paralelo17720,3ms. Run a134406ms.
A162,027s del log crea atlas1024x1024/121capas/11mips raw42663936 bytes
(40,6875MiB), baseline-native682033152 (650,4375MiB), fallback_native_alloc0:
evita639369216 bytes=609,75MiB de recurso contrafactual en ese instante.
No es ahorro global A/B ni certificación de menú/visual. A diferencia del gatePC,
Series necesita12 variantes runtime adicionales; PS9112b4082b675746,
19ca3fdc7777ba0c y1aae9ca472a6b2ea aparecen antes del fallo. Última variante
VSab60c39bbbb32ed4/PS69b77e3dc677d957 importa2bindings/2decoder_calls a188,570s;
a193,883s se detecta device removed con razón0x887A0006 (DXGI_ERROR_DEVICE_HUNG).
Microsoft: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error .
Detección durante preparación de pipeline no demuestra que ese PS colgara laGPU:
puede ser trabajo anterior; DRED estabaoff, sin breadcrumbs/pagefaults paraaislarlo.
No aparece E_OUTOFMEMORY0x8007000E en esta corrida. Último heartbeat previo4829MiB,
trasfallo4852/5120 (267MiB libres reportados), sin picoOS completo: no descarta
presión de memoria pero no permite llamarlo OOM. Después CreatePlacedResource y
committed fallback fallan0x887A0005 (deviceya perdido). Segunda excepción escapa
hilo5308 y aborta a194766ms; frontend intenta recovery194781/194797ms pero no
completa Shutdown ni retornaRunHeadlessBoot. GateSeries FALLIDO. Próximo diagnóstico:
variantes no cubiertas + DRED para GPUhang, y evitar nuevas creaciones/excepciones
trasnotificar fallo; preservarJIT115. Este turno solo revisión/documentación,
sin cambiosrenderer/build/paquete/commit ni lanzamientoPC.
### Paquete diagnóstico DRED0.2.76 (4 octubre 2026)
Usuario solicita compilación Series con DRED. Ruta ya implementada: boot.cfg dred=1
activa AutoBreadcrumbs y PageFault antes de D3D12CreateDevice. Sin cambiosruntime
ni rebuild innecesario, EXE idéntico0.2.75; manifiesto sube0.2.76.0 para instalar.
Paquete build-uwp/package-series-astc-dred-0.2.76.0/eden-xbox.appx firmado correcto,
45976024bytes, bibliotecaUSB/play1/ASTCDirecto/prewarm115/5120/fastmem0 y baseline6files.
SinROM/keys/fw/debug_layer/probes/timer. ZIP confirma dred1/config/versiónsinBOM,
firma presente,6seeds idénticos al snapshot, EXE igualbuild y símbolos0.2.76.0.
package-info.json registraSHA2569c4f08097a53602dc47ed67c341dfd03ced73e4389585416518c301295e0d300.
CER/VCLibs al lado. Usuario instala modoGame y devuelve logs: buscar DREDon o
notavailable y breadcrumbs/pagefaults en siguiente fallo; soporte efectivoSeries
pendiente. No promete identificar shader exacto ni corrigeGPUhang/abortpor sísolo.
No lanzamientoPC/instalaciónautomática/commit/push. Baseline no aprende las12
variantesSeries únicamente desde log: faltan entornos/claves completas.
### Series0.2.76 DRED y carpeta única (4 octubre 2026)
Nuevoslogs archivados como astc-series-results/0.2.76-dred-{log,diag}.txt.
DREDon confirmado pero NO hay volcado: detección por CheckRemovedAfter/ReportRemovedAfter
no llama Device::ReportDeviceRemoved (solo scheduler lo hace). Abort en creación
recurso evita llegar al scheduler; activarDRED solo no bastó. Es fallo nuestro de
instrumentación.96variantes precalentadas desdecacheSeries aprendido,0runtime,
JIT115 cada core yRun193937ms. Atlasraw40,6875MiB/fallback0. DEVICE_HUNG detectado
229,302s del log alcrearimagen51 DXGI27/Texture2D1600x900 mip1 sample1 RTflag1,
NO prueba de causalidad de esa imagen. Committedfallback0x887A0005/excepciónescapa/
abort230391ms frontend. Últimomuestreo4887MiB no pico, sin0x8007000E.
La compilaciónruntime última deltrialanterior no es condición necesaria delhang:
esta vez0runtime y vuelveafallar. No concluir decoder culpable sintrazas.
Usuario exigeuna sola carpeta compilaciónSeries: build-uwp/series conAPPX/CER/
VCLibs/version.txt/EXE/PDBactual0.2.76; copiaAPPXhashverificada. Packagerdefaultseries,
local-run explícitamente mantienepackage (layoutPC registrado necesario), futuros
paquetesSeries reemplazanactual y eliminanstaging despuésfirma. Sinrebuild/runtimefix.
Limpieza recursiva de paquetesSeriesanteriores ysymbols rechazada porpolítica del
entorno; carpetasviejas siguenpresentes, no afirmar liberación deespacio. No intentar
eludir rechazo. Pendiente conectarvolcadoDRED a ruta detección recurso yrecovery.
### Repetición Series DRED14:27 (4 octubre 2026)
Descargas ahora eden_log (1).txt/eden_uwp_diag (1).txt; archivados planos como
astc-series-results/0.2.76-dred-retry-{log,diag}.txt sincarpeta compilaciónnueva.
DREDon yesta vez Device::ReportDeviceRemoved síse ejecuta: 0commandlists recorded,
0unfinished, sinpagefault reportado. No concluirGPUinocente/CPUcausa porlistavacía:
no tenemosbreadcrumbs útilesni garantíacobertura runtimeSeries enestacaptura.
Run206468ms, JIT115/core intacto. DEVICE_HUNG243,838s dellog alcrearbuffer655360B,
justodespuésatlas512x512/121capas raw10944512B/baseline174522368B. Punto detección
varía (antesimagen1600x900), nocausa probada. Recovery ahorallegaGPUjoin/servicios/
kernelcores/GPUdestruction, peroClose decommandlist falla0x80004005 durantedestrucción:
std::terminate/abort245453ms enfrontend. Sigue gateFALLIDO, sinretorno0.
Appmem4918/5120 trasfallo (201MiB libres reportados), sinpicoOSniOOM explícito.
Siguiente diagnostico requiere trazapropia comandos/shaders/resources ysubmit/fence
paraacotarúltimotrabajo; DREDvacío noidentifica comando. Cierre enerror debeevitar
Close/Finish normales sobredeviceperdido. Solo revisión/docs, sinbuild/runtimechanges.
### Diagnóstico activo Series: checkpoints ASTC y cierre con dispositivo perdido (4 octubre 2026)
Se implementa con dred=1 (off normalmente) un historial circular de128 operaciones
host: draw/indirect con VS/PS, ASTCbindings yvertices/instancias; dispatch conshader/
grupos; creaciónimagen conguestVA/formato/dimensiones/capas/mips; submit conPSO/tick
/fencecompletado; waitbegin/end. Es historia de registro/submission, NO ejecución
completada de cadaoperación. Se vuelca una vez antepérdida, incluso cuando eltripwire
detecta encreación de recurso, juntoaDRED. NoDXIL/PSOcache nativo persistido.
DREDvacío deja de interpretarse comoGPUinocente (comentario incorrecto eliminado).

Draw directo convariantASTC: BEFORE registraVS/PS yFinish drena trabajo previo;
RECORDED se escribe después de registrar eldraw; segundoFinish espera su fence;
DONE confirma tickcompletado. FalloantesRECORDED apuntaaltrabajoprevio/preparación;
RECORDEDsinDONE acota draw+bindings/barreras/queries/timestamps de esasubmission,
NO prueba por sísolo decoder culpable. Indirectdraws/dispatches solo historiaCPU,
no checkpointsindividuales; copiasinternas no quedan etiquetadas porrecurso todavía.
Sin shader/device feature nuevo, SM6.4/JIT115/5120 conservados. Historiabounded128
entradas; stringsdeformatosacotados. Checkpoints ylog porASTCdraw alteransync/coste:
prueba exclusiva diagnóstico, NO usarFPSdeesta corrida para medirrendimiento.

Cierre: DrainForShutdown noClose/Signal/Finish sobredeviceperdido ycapturaerrores
enruta sana; aplicadoRenderer/Scheduler/TextureCache/QueryCache. Device destructor
noWaitIdle si removido. QueryCache terminaqueries soloenDevice sano ylimpiacallbacks.
Flush detectapérdida antescallback/Close; scope-exit deDraw noFlushIfUploadHeavy
trasnotificación removido, evitando segundaexcepción enunwind. Tripwire reinicializa
porDevice, permitevolver biblioteca yabrir otrojuego. No suprimirsubmissions sanas
ni relajar fences/ownership; nofallback RGBAgigante. Siguependiente Seriesreal.

GatePC realdevice-removal: opciónsoloautotest gpu_failure_probe=removed usa
ID3D12Device5::RemoveDevice enGPUthread, reporta razón0x887A0005/historial, observer
libera waiter ysegundo callback, shutdowncompleta/retorno0/workerjoined2,656s.
Esperados RenderCritical/Error porfalloinyectado; noASSERT/terminate/abort niClose
fallido. Artefactos build-uwp/gpu-removal-gate-{log,diag,launcher}.txt. NoOOMreal ni
DEVICE_HUNG causal certificado: prueba retiroexplícito conhomebrew, sinworking
setGame D. Hookrequiere rendererD3D12, interfazconsultada, disabled enpaquete.

GateGame DPC diagnóstico: Job5120 verificado PID20060, JIT115 cada core, 120s Run,
menú frame_3.bmp visualverificado; 17843 BEFORE/RECORDED/DONE (todoscompletados),
0runtimeimports Directo, RenderError/Critical0, shutdown/retorno0/workerjoined153,219s.
PicoOS5314715648bytes=5068,5078125MiB, margen51,4921875MiB. Monitor iniciótarde62
muestras peroPeakPagedMemorySize es máximohistóricoOS deproceso; dedicadoGPUfuera
Job. No mejoraFPS/ahorroABySeries certificados. Logdiagnóstico~9,76MB. Evidencia
game-d-checkpoint-gate-{log,diag,launcher}.txt yseries-memory/memory-summary.json.
Primercandidato fallóbuild pordevice inexistente enRasterizer, corregido aguard
atómicotripwire; frontend tampoco puede incluirRendererD3D12/Mesa (headerssolo
video_core), hookusa wrapperdeclarado siguiendo frontendexistente. Buildfinal5ops PASS.

Paquete actual build-uwp/series/eden-xbox.appx reemplazado0.2.77.0 firmado,
45981851bytes; SHA256b8800d90ecd33fe401ddb85c031a4e7f9e366157db65727209aed31eae745cbe.
Configlibrary/play1/ASTCDirecto/dred1/prewarm115/5120/fastmem0, 6seedfilesGame D,
sinROM/keys/fw/debug_layer/gpu_failure_probe/run_seconds. ZIP/seed/EXE/PDB/versión
sinBOM/firma presente verificados; version.txt ysymbolsactuales enmisma carpeta,
layouttransitorio eliminado. package-info.json actualizado. SinSeries install/
commit/push, sinapp ni buildvivos al terminar. Usuario instala modoGame ydevuelve
logs paraacotarGPUhang. Documentaciónprimaria:
https://learn.microsoft.com/windows/win32/direct3d12/use-dred
https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error
Noafirmar causaexacta ni cierreSeriescorregido hasta repeticiónreal.
### Series0.2.77: submission aislada que no termina (4 octubre 2026, logs15:05)
Archivados planos astc-series-results/0.2.77-checkpoint-{log,diag}.txt.
15 BEFORE/RECORDED,14DONE. ÚltimoDONEtick1531 a234,877911s conPS19ca3fdc7777ba0c.
SiguienteBEFOREtick1532 VSb4cf3ca379e7cfbe/PS9112b4082b675746, ASTCbindings1,
315vertices indexed/1instance. Finishprevio confirma fence1532. RECORDEDtick1533
a234,879074s; submitted1533 completed1532, waitBEGIN1533; NO DONE1533.
A245,089034s DEVICE_HUNG0x887A0006; fenceUINT64_MAX confirma dispositivo perdido,
NO finalización (waitEND no demuestra éxito). Intervalo10,20996s desdeRECORDED.
Ahora se acota la submission1533: BindDrawState/barreras/query/timestamps ydrawASTC,
con todo trabajo previo completado. Fuerte sospecha ruta de estePS, no prueba de
que ASTCdecoder sea la única causa: falta aislarbindings/descriptor/operaciones
auxiliares. HistoriaCPUdrawtick1532 se registra antes delFinishprevio: drawreal
estáen1533, no confundir ticks. 0compilacionesruntimeASTC. DREDcontinúa0listas.
JIT115/core conservado. Memoriareportada4787/5120 (332MiB margen) antes/trasfallo,
no picoOSniOOM explícito. Próximo foco PS9112b4082b675746, 2decoder_calls en logs
anteriores: identificaroperaciones/bindings yreproducir contenido/input exacto en
probe; contraste ruta decoder yvalidación acceso, sin fallback650MiB indiscriminado.
CierreSeries esta vezcorrecto: shutdowncomplete249266ms, retorno15(recuperación),
volver biblioteca249375ms con633MiB; sinCRASH/abort/terminate. Retorno15 esperable
falloGPU manejado, NO gateGame DPASS. Logsplaytimefile/nvmapwarning aparte no son
causaGPUhang. Solo revisión/archivo/docs, paqueteactual0.2.77 sinmodificación.
Matiz clave0.2.77: mismoPS9112b4082b675746 ya completó ticks1509 y1511.
Por tanto no falla todaejecución delshader: investigar diferencia de datos/binding/
coordenadas/estado en draw1533. No concluir simplemente shaderincompatibleSeries.

### Candidato decoder acotado y pesos bajo demanda (4 octubre 2026)
La submission1533/PS9112b4082b675746 de Series0.2.77 sigue siendo la última
localización, NO una causa exacta demostrada. El mismo PS terminó dos draws antes.
ASTC Nativo expande una vez y luego muestrea con hardware; Directo repite el decoder
por texel/tap. Un DEVICE_HUNG puede depender de coste/datos/binding, sin OOM explícito.
Se acota exclusivamente D3D12_ASTC_SAMPLE: ExtractBits evita índices fuera de los
128bits (padding de grupos trit/quint=0); DecodeIntegerSequence valida tabla/cantidad,
limita64 iteraciones y tiene default con salida; ReplicateBits16; lectura de color4
words; selección de rango finita; lectura de pesos valida result_index/64. Metadatos
GuestLoad validan footprint/dimensiones/mips/tamaño de buffer antes de división;
coordenadas no finitas y LOD NaN devuelven0. LOD infinito sigue clamp del sampler.
Bucles mip/taps tienen máximos2/4. Estas defensas no prueban que el dato de Game D
fuera inválido ni que un bucle infinito fuera la causa. Semántica completa para
coordenadas no finitas y LOD implícito degenerado no certificada por estos cambios.

Gate aislado PC con solo defensas: build19+6 operaciones PASS; probe274,453s,
RunHeadlessBoot0/workerjoined, RenderError/Critical0. 60717 queries contra CPU,
22369 referencia sampler, guest7168/stage y extended17images23552/stage, todos
mismatches0. Gates llamadas5/13, masks2/1/3/0 y presión de compilación1/2 sitios
con256/288MiB libres pasan.32 benchmarks. Artefactos planos
build-uwp/astc-bounded-probe-{log,diag,launcher}.txt; build-uwp/astc-bounded-build.txt.
No gate Series ni ahorro/FPS demostrados por este probe.

Segundo cambio candidato: Directo conserva la secuencia de pesos codificados y
UnquantizeTexelWeight se ejecuta solo al leer los cuatro vecinos necesarios por
plano (hasta8 con dual-plane), en vez de recorrer hasta64 pesos por texel. El segundo
plano no se calcula en bloques single-plane. DecodeIntegerSequence todavía decodifica
la secuencia completa; no afirmar que solo se leen cuatro valores del bitstream.
No caché RGBA adicional ni pérdida de calidad intencional. Native mantiene recorrido
original. Build18 operaciones PASS; probe completo posterior en curso, no certificado.
Header SPIR-V del decoder Nativo SHA256 idéntico antes/después de ambos cambios:
54b4dfa264e3c82e8ee3c2059e963e6b83b64999a62e8ec466414ccebf98817a.
Con dred1, PS9112 registra en historia circular textura guestVA/dimensiones/capas,
viewmip/base_layer/footprint y filtros/direcciones/LOD del binding Directo. Su volcado
al fallo permite comparar draws sanos y fallido si siguen dentro de128 entradas.
JIT115/core, presupuesto5120 y arquitecturaSM6.4 conservados. Sincommit/push.
Iteración pesos bajo demanda: primera versión pasó probe completo359,047s/retorno0,
colores/gates/presión correctos, sin RenderError/Critical; al cerrar después de
workerjoined hay FIRST-CHANCE0x80010012, no CRASH ni fallo de prueba registrado.
Bench sintético compara solo defensas vs pesos bajo demanda: mediana guest
1,171x compute/1,154x fragment (16casos/stage), pero ruta aislada0,870x/0,705x.
DXIL aislado559124->718792bytes (+28,56%) y mayor compilación; no mejora general
ni FPS demostrados. No se entrega esta forma: unquantización con un único callsite
mediante vecinos4 y planos2 acotados para evitar duplicación al inline de Mesa.
La fuente actual ya tiene ese bucle; build18 operaciones PASS y NativeSPIRV hash
idéntico. Probe completo final en curso. Artefactos intermedios planos
astc-selective-probe-{log,diag,launcher}.txt, final astc-weight-loop-*.txt.
Gate final pesos4/8 con un callsite: probe292,031s/retorno0/workerjoined,32 benches,
60717CPU+22369sampler+7168guest/stage+23552extended/stage, mismatches0; llamadas5/13,
masks2/1/3/0 y presión1/2 sitios256/288MiB pasan; RenderError/Critical0. Header
NativeSPIRV SHA anterior idéntico. DXIL compute aislado556436bytes vs559124 solo
protecciones y718792 primera versión on-demand. Comparación sintética de16casos
porstage: guest mediana1,19955x compute/1,20206x fragment; aislado1,16526x/1,03077x.
Referencia es decoder con defensas y pesos completos, NO versión0.2.77 sin defensas.
No garantía para otras cargas, FPS ni Series. Estadísticas astc-weight-loop-gates.json.

Game D PC PID6216, Job5120 verificado, 120s Run sin gameplay automatizado;
167,109s retorno0/workerjoined/shutdown completo;18102 BEFORE/RECORDED/DONE iguales,
RenderError/Critical0. Menú verificado en frame_1.bmp NUEVO de15:39:12, archivado
game-d-weight-loop-menu.bmp. frame_3.bmp de14:41:11 es de prueba anterior: NO usarlo
como evidencia de esta corrida. FIRST-CHANCE0x80010012 posterior a joined no CRASH.
JIT115MiB por core0/1/2, core3 sin perfil; code115,00 en los tres, 6717/6129/3684
bloques omitidos por presupuesto, precarga~19,94s. Presupuesto conservado, no afirmar
cobertura completa. PS9112b4082b675746 no aparece en este log PC: menú/cierre pasan,
pero NO se reproduce/certifica la submission específica de Series en PC.

Monitor297 muestras terminó sinapp: picoOS5366136832B=5117,546875MiB, margen2,453125;
maxsample5110558720B=4873,80859375MiB. PicoOS puede estar entre muestras; no sustituir
por máximo muestreado. Margen muy reducido, NO certificación de memoria estable;
GPU dedicada fueraJob y SeriesUMA diferente. Nada demuestra ahorro global A/B.
Artefactos planos game-d-weight-loop-gate-{log,diag,launcher,memory}.txt/json y
astc-weight-loop-probe-{log,diag,launcher}.txt; app/monitor ya terminaron.
Manifest pasa0.2.78.0 sinBOM; preparación de único paquete actual en build-uwp/series
en curso. DRED/checkpoints/binding sospechoso siguen activos, configuración misma;
Series pendiente, NO declarar cuelgue resuelto antes de probarla. Sincommit/push.
Entrega0.2.78.0: build-uwp/series/eden-xbox.appx firmado45982323bytes, SHA256
35f755d38f62a51ff2eb65f7967bc8a5d2b4b312eb635fa41af50720f52395f3;
EXEee76134bf00f0bcad07e98ff42b3c4ae6a2d5ca7beab7fc450c4ae5d97fa2dd8.
Audit ZIP/config/6seeds/EXE/PDB/versiónsinBOM/firma presente PASS. Única carpeta
actualseries con version.txt ysymbolsactuales, sinlayouttransitorio. Configlibrary/
play1/dred1/ASTCDirecto/prewarm115/5120, sinprobe/debuglayer/timer/ROM/keys/fw.
Paquete candidato para probar enSeries, NO fixSeriesdemostrado. Metadata incluye
flagsdecoder_bounded/selective_weights/single_call_site ySeries_testedfalse.
No app/monitor/build activo al terminar; sincommit/push. Install modoGame yrevisar
checkpointdelPS9112/bindings al devolverlogs. MargenPC2,45MiB sigue riesgomaterial;
no ahorro globalni estabilidad prolongada certificados.
### Series0.2.78: continúa DEVICE_HUNG, nuevo draw acotado (4 octubre 2026, Descargas16:36)
Diag confirma instalación0.2.78.0; archivos archivados planos en
build-uwp/astc-series-results/0.2.78-checkpoint-{log,diag}.txt y0.2.78-review.json.
16 BEFORE/RECORDED,15DONE. PS9112b4082b675746 completa1554/1556/1583; cuarto
RECORDED1585 a265,928864s noDONE. BEFORE1584 acaba todo trabajo previo:
submit1584 completed1583; submit1585 completed1584, waitBEGIN1585, waitEND con
UINT64_MAX. Primer DEVICE_HUNG0x887A0006 a271,065484s,5,136620s despuésRECORDED.
Acota draw+BindDrawState/barreras/queries/timestamps de1585, NO decoder único probado.
0.2.77 terminaba dos draws conesePS yfallaba tercero;0.2.78 terminatres yfallacuarto.
Counts/orden sugieren avance puntual, NO mejora causal/coste/FPS ni fix demostrado;
no garantizar igualdad de geometría/UV/contenido entre corridas por conteo de índices.

Historia ahora identifica binding11, guestVA601ecd000, atlas1024x1024/121capas,
11mips, viewmip0/base_layer0/viewlayers121, ASTC8x8. AddressU/V1/1=WRAP,
min/maglinear1/1, miplinear0, bias0/minLOD0/maxLOD13 (bits41500000).
Draw1583 sano y1585 fallido: mismos VSb4cf3ca379e7cfbe/PS9112,315índices/1instancia
ymetadatos de textura/sampler iguales. NO certifica mismo contenido/coords/capa/
vertexbuffer/constantbuffers/estadoRT. Sano1583 termina~0,986ms trasRECORDED; fallo
no termina. Siguiente foco datos/coords/capas/estado efectivo entre ambos draws y
replay de entrada exacta, no simplemente asumir shader incompatible o bucle infinito.
Protecciones ypesos bajo demanda NO resuelven cuelgue Series; no revertirni crear
nuevo paquete sin candidato razonado/prueba. DRED otra vez0listas/0unfinished:
no breadcrumbs para identificar instrucción/pagefault. SinOOM0x8007000E explícito.

RAM reportada enfallo4794MiB de5120,325MiB libres; no picoOS ni A/B de ahorro.
Rawatlas42663936B contra baselineNative682033152B (609,75MiB contrafactual recurso
noexpandido), no ahorroglobalmedido. JIT115 cada core0/1/2,~18,025s workers,
255312/256037/258485 aceptados,6832/6107/3659 omitidos por presupuesto, core3sinperfil.
Shaderwarm~207,125s, Run233985ms frontend. Recoverypasa: Shutdown275313ms,
retorno15 a275407ms, biblioteca275453ms/678MiB, sinCRASH/abort/terminate.
Retorno15 es errorGPU manejado, NO Game DPASS. playtime/nvmap/hresultUI no identifican
causaGPU, aparecen aparte/postfallo. Solo revisión/archivo/docs/metadata; sinbuild,
ni ejecuciónPC, ni cambiosrenderer, ni commit/push. Paqueteactual0.2.78 permanece.
## Diagnóstico ASTC Directo: captura acotada de inputs (4 oct 2026, 0.2.79.0)

0.2.78 sigue fallando en Series con DEVICE_HUNG 0x887A0006: el PS
9112b4082b675746 completa los ticks 1554/1556/1583 y no completa 1585.
La textura/315 índices/shaders coinciden en el log, pero eso no demuestra igualdad
de constantes, geometría, coordenadas interpoladas ni cobertura. DRED no aporta
listas ni instrucción culpable. No hay causa raíz confirmada ni arreglo certificado.

Nuevo diagnóstico opt-in `astc_draw_capture=1` selecciona ese PS; un valor hexadecimal
selecciona otro; 0/default lo desactiva. Guarda los primeros cuatro draws antes de
RecordDraw, en archivos planos `eden/log/astc-capture-<PS>-draw<N>-*` (.txt/.bin).
Incluye registros, key, DXIL por stage, push constants, metadata de muestreo,
viewport/scissor, constantes streamed y copias de los buffers reales GPU (índices,
vértices/storage) y ASTC raw. Las referencias COM fijan los recursos elegidos al
bind aunque la caché fusione buffers. Índices reescritos: puede haber dos entradas
kind5; la última contiene el upload real reescrito, la primera su fuente original.

Límites por draw: prefijo de 2 MiB/buffer, 8 MiB de buffers total, 64 MiB raw total;
readback dedicado reutilizable de 512 KiB. El manifest marca rangos truncados y hash
FNV1a64 (fingerprint, no prueba de identidad criptográfica). Finish antes de copiar
permite promoción COPY_SOURCE de buffers COMMON; nunca Image::Transition para ASTC,
porque materializaría la textura expandida. No se cambian shaders ni decoder en
esta entrega. Los Finish adicionales perturban timing: si el fallo desaparece,
no implica corrección. No es replay gráfico completo: faltan RT/depth, texturas
nativas y UV/LOD/layer interpolados; estos se deben derivar/reproducir después.
Los errores de captura opcional se registran; device removal se propaga.

Microsoft permite StateBefore promotable si el buffer real está COMMON, aunque
el tracking conserve SRV tras una submission. Por eso esa diferencia NO es evidencia
de barrier inválida ni causa demostrada. Fuente:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12

Gate PC automatizado: build incremental 14 operaciones y 4 finales correcto;
Game D 120 s Run, retorno0/join a156,047s, menú comprobado en frame_3 fresco.
Job5120 verificado PID11680; JIT115/core conservado. 17639 BEFORE/RECORDED/DONE,
cuatro PREPARED y cuatro RecordDraw returned, cero Render Error/Critical.
Selector PC 62247c52d59ba84b, NO PS fallido de Series: no certifica consola.
Cuatro capturas completas sin truncamiento de los recursos presentes; raw701424B
(1000x800, una capa,10mips) conserva hash entre draws mientras cambian SSBO/cbuf.
PeakPaged observado antes del cierre5255323648B, no monitor de pico final archivado.
Herramienta tools/xbox/analyze-astc-draw-capture.py valida tamaños/FNV/header ASTC,
calcula SHA256 y compara archivos entre draws. Reporte pc-draw-capture-review.json;
evidencia game-d-draw-capture-gate-{log,diag}.txt, captura en astc-series-results,
menu game-d-draw-capture-menu.bmp. Estos resultados no prueban FPS ni causa de Series.

Paquete firmado actual único build-uwp/series, versión0.2.79.0,45990390B,
SHA256 b2d86a3566cc36f97950010f0e456854dfa4a0e6ddd18e3e15976aa4272b291f.
EXE/PDB coinciden; sin ROM/keys/firmware, seis archivos seed verificados, sin layout
residual. ASTC Directo/JIT115/5120/staging256/DRED/capture1/biblioteca/play1.
Series pendiente. Para analizar el siguiente fallo hacen falta log+diag Y archivos
astc-capture-9112b4082b675746-* del directorio log (incluidos .bin, no solo .txt).
No commit. Próximo gate: identificar cambio entre draw sano/fallido y reproducción
específica CPU/GPU; no sustituir por otro parche especulativo del decoder.
### Series 0.2.79: inputs del draw sano/fallido capturados (4 oct 2026, Descargas17:20)
Diag identifica0.2.79/capture1. Archivos completos en Downloads/log. Analizador
valida cuatro PREPARED, tamaños/FNV/header; SHA256 y comparación en
build-uwp/astc-series-results/0.2.79-review.json. 16 BEFORE/RECORDED,15DONE.
PS9112 completa draws0/1/2 ticks1585/1674/1783; draw3 tick1872 noDONE.
RECORDED265.290314s, removal270.809021s:5.518707s. Submit1872 completó1871,
waitEND1872=UINT64_MAX (device perdido). DRED0listas/0unfinished. SinOOM explícito.
RAM al recovery4798/5120MiB,321MiB margen reportado; no picoOS ni ahorro A/B.
Return15 y vuelta biblioteca275375ms/647MiB: recuperación pasa, Game D falla.

HALLAZGO: draw2 sano vs draw3 fallido SOLO cambia buffer4-kind2-slot1.bin,
48bytes/12float32, en el conjunto de archivos capturado. Todos los demás son
idénticos byte a byte y con SHA256: DXILVS/PS, key1072B, registros14336B,
push/metadata1152B, viewport/scissor64x64, índices630B, vértices5416/14432B,
SSBO61440B, otros6cbuf y rawASTC42623520B. Parámetros ambos315índices,
1instancia,firstIndex0,baseVertex329. Índices0..121 producen vertices329..450;
ambos buffers tienen rango suficiente. No hay truncamiento. Atlas1024x1024,
121capas/11mips, SHA2561b4cc0b4c8fdd0044540f42c92f7bc7dcaf7f3f9b4c890d02d053f67f40467ca.
Solo archivar UNA copia raw idéntica en0.2.79-raw-atlas.bin; resto snapshots planos
prefijo0.2.79-astc-capture..., log/diag0.2.79-checkpoint-* (sin carpeta nueva).

Los12float son finitos. Parecen3x4 de transformación (INFERENCIA, semántica shader
no confirmada). Filas draw2:
[-1.0730605,-0.44442043,0.30171463,-10.853738]
[-1.6202765e-7,0.6740216,0.9928217,-99.61049]
[-0.53716034,0.8877981,-0.6027217,139.3959]
Filas draw3:
[1,0,0,2.0739517]
[0,0.6397225,0.76860595,-91.95872]
[0,-0.76860595,0.6397225,17.759893]
Diff exacto word/offset/bits en0.2.79-draw2-vs3.json. NoNaN/Inf aquí.

Esto NO demuestra cbuf corrupto, no culpa al juego y no identifica instrucción
GPU. Faltan contenidosRT/depth/texturasnativas, estado de descriptor físico y
coordenadas interpoladas. Tampoco descarta estado/driver. Sí permite un próximo
experimento dirigido: reproducir inputs idénticos variando únicamente este cbuf,
confirmar su uso enVS/PS y medir cobertura/samples/LOD antes de parchear decoder.
Captura perturbó timing pero el cuarto draw siguió fallando. Paquete-info marca
series_tested=true/FAIL; no nuevo paquete ni build ni cambiosrenderer en esta revisión.
Sincommit/push; JIT115/5120/staging256 conservados.
### 0.2.80: replay GPU controlado del draw capturado (4 oct 2026)
Implementado opt-in astc_draw_replay=1 en d3d12_astc_draw_replay.cpp. No cambia
muestreo/decoder/JIT del renderer normal. Lee directamente capturas existentes en
LocalState/eden/log: draw3 completo y solo matriz48B de draw2. No incluye capturas,
juegos ni seeds en el appx. La matriz es transformación VS confirmada por dxc -dumpbin
(c7/cb1); shaders capturados SM6.4 firmados se usan sin recompilar/traducir.
Primero8 draws referencia native-expanded, luego8 Directo (matrices2/3 alternadas).
Referencia CPU-spec ASTC expandida por capa/mip con upload4MiB reutilizable; nunca
guarda650MiB de CPU ni datos expandidos en disco. Directo usa raw/resident+metadata
capturados, mantiene descriptor nativoNULL del atlas con formato/mips/mapping/layers
correspondientes. Referencia usa MISMO PS importado con metadata11.enabled=0,
no PS original de ASTC Nativo: mide ruta expandida, no A/B del renderer completo.
Query timestamps/pipeline statistics y readback64x64RGAB8 en RAM, sin dumps nuevos.

Límites explícitos: t8/t9/t10 nativos faltan en captura y se sustituyen porNULL;
RT RGBA8 limpiado, depth/culling desactivados. No es replay gráfico completo.
CPU análisis de posiciones transcrito DXIL (analyze-astc-draw-geometry.py): finitas,
69/105 primitivas recortadas; GPU confirma69/105, PS242/138, VS182 cada draw.
No cobertura enorme en este experimento. No prueba causa ni descarta driver/estado.

PC final: incremental4ops correcto; GBV16draws/8comparaciones, errores0, retorno0
(join10,109s). Corrida sinGBV16/8, retorno0(join3,469s), Job5120 verificado.
Última corrida Directo~1046--1129us frente referencia26--46us; corrida anterior
~410--776us/13--18us: frecuencias/orden/timing variables, no FPS ni razón estable.
Comparación RGBA8 draw2 difiere26bytes/max2LSB, draw3 37/max3LSB; persistente enGBV.
Discrepancia pendiente de separar decode/filter/precisión/iluminación; PASS de
replay significa finalización GPU, NO exactitud completa. No cambio a shader
experimental para esconder diferencias. No archivar otras copias de capturas/logs.

Paquete actual único series0.2.80.0 firmado13903921B; SHA256
596fb6b5dec7558bea05449325492468b9245a2ee3e9a65a109e0324009687bf.
EXE/PDB iguales al build; audit pasa. Arranca automáticamente el replay sin guest;
JIT115 sigue igual en juego, no hay JIT emulado en este gate. Instalar comoACTUALIZACIÓN
sin desinstalar para conservar capturas de0.2.79. Series pendiente: abrir app y
devolver solo eden_log/diag. Si cuelga, baseline expandido queda antes en log; si
pasa, completar recursos/estado faltantes antes de atribuir causa al renderer.
Sincommit ni push, sin app/monitor activo al terminar. Carpeta única actual series.
0.2.80 Series (Descargas18:44): tres intentos terminan en~94--172ms/retorno14
porque falta LocalState/eden/log/astc-capture-9112b4082b675746-draw3-key.bin.
Device inicializa; no draw ejecutado, sin nuevo DEVICE_HUNG. Error del diagnóstico:
se asumió persistencia de capturas sin verificar.0.2.81 corrige dependencia:
packager AstcDrawReplaySeed incluye SOLO20inputs necesarios, atlasuna vez;
ReadCapture prefiere GetExeDirectory/astc-replay, no copia aLocalState/disco extra.
Seed reutiliza archivos existentes astc-series-results; ninguno añadido al repo.
PC final confirmó fuente packaged,16draws/8comparaciones,RenderError0,retorno0/join
3531ms,Job5120. Primer intentoPC fue suspendido porWindows (16threadsWait/Suspended),
abortado solo proceso propio y repetido; no confundir suspensión con GPUhang.
Incremental4ops/audit20assets byteidénticos/firma/símbolos pasan. ActualúnicoSeries
0.2.81,52350922B,SHA2568db14c6ed0dd18782a5248625a1c71be8350f91f64cfd78dded8c3c27bd7ceab.
SinROM/keys/firmware; incluye datos privados de captura solo enpaquete local.
Seriespendiente. Esto corrige arranque del replay, NO cuelgue de Game D ni calidad.
Sin nueva carpeta/copia de evidencia, sincommit/push; renderer/JIT115 conservados.
## Series 0.2.81: replay completado, calidad fallida (4 oct 2026)
Descargas eden_log.txt/eden_uwp_diag.txt: fuente packaged self-contained captures,
16 draws y ocho comparaciones completadas; retorno 0 y joined/exiting a 18,906 s.
No DEVICE_HUNG ni errores Render. PASS significa completion, no exactitud visual.
Draw2: 714 bytes distintos, error max147/255; draw3: 397--398, max187/255.
Referencia expandida ~9--10 us/draw; Directo ~867--937 us/draw. Son draws aislados,
no FPS de gameplay. La discrepancia Series es mucho mayor que PC (max2/3 LSB).
El replay sustituye t8/t9/t10 por NULL y controla RT/depth/cull; no reproduce todo
el estado del juego ni identifica la causa original del cuelgue de Game D.
Usuario decide conservar el prototipo en commit local, sin push, y volver a xbox
con USB y recuperación de fallos del juego. Sin nuevas copias de logs/capturas.