# eden-xbox: notas internas

Cuaderno de trabajo del port: cómo compilar, en qué fase vamos y lo que hemos aprendido por las malas.
Los docs "de verdad" son [`uwp_build.md`](uwp_build.md) (toolchain) y [`xbox_deploy.md`](xbox_deploy.md)
(empaquetar y desplegar). Aquí va el resumen rápido y todo lo que no cabe en ellos.

**Reglas de la casa**
- Los commits van solo a nuestro fork: `origin` = `JulianDr14/eden-xbox`, fork de
  `eden-emulator/mirror` (el espejo oficial de Eden en GitHub), rama `xbox`.
  - `eden` (espejo de Eden) y `upstream` (juanresendiz813/eden-xbox) son solo lectura, con el push
    deshabilitado.
  - `legacy` (`JulianDr14/eden-xbox-legacy`) es el repo anterior, que queda como archivo.
- Eden prohíbe la IA en su proyecto: nada de este fork se envía a Eden.
- Nada de keys, firmware ni juegos en el repo. El único payload de prueba es el homebrew de
  `tools/xbox/boot_nro/`.
- GPLv3: el código queda abierto y se mantienen las atribuciones.
- Los binarios de Mesa (`spirv_to_dxil.dll`) no se commitean: se compilan con el script.

---

## 1. Carpetas

```
Documents\Programacion\Xbox\
  eden-xbox\          # este repo
    build-uwp\        # build UWP (ninja), el paquete y los símbolos
  mesa-build\         # Mesa para spirv_to_dxil (fuera del repo)
    mesa-26.2.3\      #   fuentes con los 2 parches aplicados
    venv\             #   meson 1.12.1, mako, pyyaml, packaging
    build-uwp\        #   build meson; el DLL sale en src\microsoft\spirv_to_dxil\
```

## 2. Compilar y probar

Todo se hace desde **PowerShell o cmd**, nunca desde Git Bash, porque el make de msys2 y los temporales
fallan ahí.

| Paso | Comando | Cuándo |
|---|---|---|
| Configurar | `tools\xbox\build-env.bat cmake --preset uwp-x64` | La primera vez o si cambia CMake |
| Compilar Eden | `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp` | Siempre; son ~580 pasos desde cero |
| Payload NRO | En `tools\xbox\boot_nro\`, con `TMP`/`TEMP`/`TMPDIR` apuntando a esa carpeta: `C:\devkitPro\msys2\usr\bin\make.exe` | Si cambia `main.c` |
| Payloads deko3d | `powershell -ExecutionPolicy Bypass -File tools\xbox\build-deko3d-examples.ps1 [-Examples 2,4]` (necesita `switch-glm` y `deko3d`; el 10 y el 11 son nuestros: blits y clears con máscara, y dispatch/draw indirectos) | Para las pruebas de GPU de la fase 4 |
| Mesa | `powershell -ExecutionPolicy Bypass -File tools\xbox\build-spirv-to-dxil.ps1` | Una vez, y cuando cambie `tools\xbox\mesa\`; con `-Reconfigure` si cambian opciones |
| Empaquetar | `powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1` | Para cada prueba |

- **Salida del empaquetado:** `build-uwp\package\eden-xbox.appx`, `eden-xbox.cer` y
  `Microsoft.VCLibs.x64.14.00.appx`. Incluye `dxil.dll` (del Windows SDK) y `spirv_to_dxil.dll`.
- **Versión:** antes de cada prueba en consola se sube `Version` en `dist/uwp/AppxManifest.xml`. Así
  sabemos qué build generó cada log.
- **Símbolos:** los `.pdb` de cada versión se archivan en `build-uwp\symbols\<versión>\` para poder
  leer los crashes de la consola.

### Compilar solo algunos archivos (para cazar errores rápido)
En lugar de todo el target, se pueden compilar objetos sueltos:
```
tools\xbox\build-env.bat ninja -C build-uwp src/video_core/CMakeFiles/video_core.dir/renderer_d3d12/<archivo>.cpp.obj
```

### Probar en el PC (loose register)
- Comando: `powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 [-NoBuild]`.
  - Compila, empaqueta, registra el layout con `Add-AppxPackage -Register` y lanza la app.
  - Al terminar imprime el diag.
  - Con `-DebugLayer` activa la capa de debug de D3D12, y sus errores y avisos acaban en
    `eden_log.txt` como `D3D12 debug layer [id]: ...`. Necesita la función opcional "Herramientas
    de gráficos" de Windows y solo sirve en el PC.
- **Dónde quedan los archivos en el PC:**
  - El diag, en `%LOCALAPPDATA%\Packages\EdenEmuProject.EdenXbox_4qge6yz81zw0w\LocalState\`.
  - El log **no** está ahí, sino en `LocalState\eden\log\eden_log.txt`.
- Desde PowerShell, `cmd` puede resolver al `cmd` de devkitPro. Hay que usar
  `& "$env:SystemRoot\System32\cmd.exe" /c ...`, y lo mismo pasa con `tar`.
- Al rehacer el layout **con la misma versión**, la app pierde el permiso de lectura y falla con
  "Failed to obtain loader". Se arregla con:
  `icacls <layout> /grant "*S-1-15-2-1:(OI)(CI)RX" /T`
- D3D12 en el PC se comporta igual que en la consola para todo lo que hemos probado. Por eso cada
  cambio se prueba primero en el PC.

### Probar en la Series
1. Abrir el Device Portal en `https://<ip-xbox>:11443`.
2. Instalar el appx junto con la dependencia VCLibs.
3. Poner la app en modo **Game**. Sin eso el límite de memoria es mucho menor.
4. Ejecutarla y recoger `eden_uwp_diag.txt` y `eden_log.txt`. Los dejamos en `Descargas`.

Qué buscar en esos archivos:
- En el diag: `RunHeadlessBoot returned 0`.
- En el log: las líneas `D3D12: ...` con el adaptador, las capacidades, el `shader path ready` y el
  `blit pipeline built`.
- Si el shader path falla, el log dice `presenting through the CPU` junto con el motivo.

### Probar un juego (volcados propios del usuario)
Las keys, el firmware y los juegos son del usuario y **nunca** van al repo ni a un appx publicado. En
el PC viven fuera del repo, en `..\eden-data\` (`keys\`, `firmware\` con los `.nca` y `games\`).

- **Empaquetar:**
  `tools\xbox\package-appx.ps1 -Game ..\eden-data\games\game.nsp -RunSeconds 180`
  - Van a `layout\userdata\` y el `boot.cfg` recibe `game=game.nsp`.
  - Con juego, el appx se empaqueta sin comprimir (`/nc`): un volcado cifrado no se comprime, y
    comprimirlo tarda minutos.
- **En el primer arranque** la app copia `userdata` a `LocalState`, siempre que falte el archivo o
  cambie su tamaño (`SeedUserData`, con una línea `seed ...` en el diag):
  - las keys van a `eden\keys`;
  - el firmware va a `eden\nand\system\Contents\registered`;
  - los juegos van a `games`.
- **Con el juego ya copiado** (`LocalState` sobrevive a las actualizaciones), basta con
  `-Game game.nsp`. Si no existe el archivo, solo se escribe el nombre en `boot.cfg`. Así el appx
  vuelve a pesar unos MB.
- **En el PC:** lo mismo con `local-run.ps1 -Game`. Para pasar `-BootCfg` hay que
  llamarlo con `&`, porque con `-File` el array llega como un solo texto.
- **Tiempo:** un juego no emite centinelas. Sin `-RunSeconds` corre 120 s.
- **Opciones de diagnóstico** (`-BootCfg @("...")`):
  - `log_filter=*:Debug` para el detalle del log;
  - `renderer=null` para saber si un bloqueo es de GPU o de CPU.
- **Para ver el progreso:** en `eden\log` quedan `frame.bmp` y un `frame_<n>.bmp` cada ~10 s.
- **Si el proceso muere sin línea `CRASH`:**
  - buscar `C++ throw` en el diag;
  - con `-DebugLayer`, buscar `device removed` y `DRED` en el log.

---

## 3. Fases

| Fase | Qué | Gate | Estado |
|---|---|---|---|
| 0–2 (boot) | Toolchain UWP, AppContainer, JIT con W^X, boot headless | **Gate 2:** el NRO llega al centinela del JIT en la Series | ✅ |
| 1 (render) | Renderer D3D12: device, swapchain en el CoreWindow, probe de capacidades, framebuffer del guest vía CPU | **Gate 3:** se ve el patrón en la tele | ✅ 0.2.6.0, commit `cc2ea38d9` |
| 2 | Shaders SPIR-V → Mesa `spirv_to_dxil` → DXIL firmado con `dxil.dll`; el blit de Eden en la GPU | **Gate 4:** el PSO se crea en la Series con DXIL firmado | ✅ 0.2.7.0 (sin commit todavía) |
| 3 | Infraestructura del rasterizador; diseño completo en [`xbox_d3d12_phase3.md`](xbox_d3d12_phase3.md) | Uno por sub-fase (3a.1–3d) | ✅ Cerrada: 3a–3d validados en la Series (3d en 0.2.13.0) |
| 4 | Pipelines (detalle debajo) | Primer draw 3D de un homebrew | — |
| 5 | Paridad (detalle debajo) | — | — |

**Fase 3.** Tomando como modelo el backend de Vulkan:
- Scheduler con command lists y fences.
- Staging ring.
- Descriptor heaps: un heap shader-visible usado como ring, más heaps de CPU, y deduplicación de
  samplers (el límite es 2048).
- `BufferCacheRuntime`, `TextureCacheRuntime` con Image, ImageView, Sampler y Framebuffer.
- `FenceManager` y `QueryCacheLegacy`.

**Prueba PC de 3a.2:** el frame inicial salió de un buffer dedicado (16 MiB para una petición de
12 MiB en la ventana local 2048×1536) y el patrón 1280×720 salió del stream (3.686.400 bytes). El
boot terminó con retorno 0, sin warnings ni errores de Render. Durante la primera implementación se
detectó que consultar las fences en cada petición impedía compartir una región entre rangos no
solapados de frames consecutivos y creaba buffers dedicados innecesarios. La consulta se hace solo
al envolver el ring, que es cuando el cursor puede volver a pisar memoria anterior.

**Prueba Series de 3a.2 (0.2.9.0, logs):** el frame inicial usó un dedicado de 8 MiB para
8.294.400 bytes y el patrón usó el stream con peticiones de 3.686.400 bytes. Hubo exactamente un
marcador de cada ruta, `RunHeadlessBoot returned 0`, cero warnings/errores de Render, cero device
removed y ningún fallback por CPU. Falta confirmar el resultado visual antes de cerrar el gate.

**Prueba PC de 3a.3:** el present creó páginas offline RTV, sampler y CBV/SRV/UAV; creó el anillo
shader-visible de 262.144 slots y el heap visible de 2048 samplers; copió el SRV al anillo, guardó
la tabla del sampler lineal y confirmó su deduplicación en el segundo frame. Terminó con retorno 0
y sin warnings/errores de Render. La auditoría encontró una trampa importante: reservar una tabla
puede hacer flush al envolver un heap. Las tablas se reservan ahora antes del staging y de grabar
comandos, y `SetDescriptorHeaps` se ejecuta después de toda operación capaz de resetear la lista.

**Prueba Series de 3a.3 (0.2.10.0, logs):** se crearon las tres páginas offline usadas por el
present, el anillo visible de 262.144 slots y el heap visible de 2048 samplers. El SRV se copió al
anillo, la tabla del sampler se guardó y el segundo frame confirmó su deduplicación. El boot terminó
en 10,906 s con retorno 0, usando 850 MiB de 5120 MiB, sin warnings/errores de Render, device removed
ni fallback por CPU. Falta confirmación visual para cerrar formalmente 3a.

**Fase 3b (PC y Series):** `d3d12_buffer_cache` instancia la caché genérica completa con buffers committed
en heap `DEFAULT`, staging `UPLOAD`/`READBACK`, copias, clears, bindings de índice/vértice y reporte
de memoria DXGI. El gate escribe un patrón de 4096 bytes, ejecuta `UPLOAD → DEFAULT → READBACK`,
espera el tick y compara cada byte. Pasó en PC con retorno 0 y sin errores de Render. Los buffers
vuelven explícitamente a `COMMON` tras cada lote de copias; es conservador, pero evita mezclar una
promoción a `COPY_DEST` con un uso posterior como fuente dentro de la misma command list.

**Prueba Series de 3b (0.2.11.0, logs):** el runtime arrancó, ejercitó staging de stream y readback
dedicado, y completó `UPLOAD → DEFAULT → READBACK` comparando correctamente los 4096 bytes. El boot
terminó en 11,016 s con `RunHeadlessBoot returned 0`, usando 850 MiB de 5120 MiB, sin
warnings/errores de Render, `DXGI_ERROR_DEVICE_REMOVED` ni fallback por CPU. Los errores de motores
de input ausentes y del archivo opcional `playtime.bin` son ajenos al renderer y no bloquean el gate.

**Fase 3c (PC):** `d3d12_texture_cache` instancia la caché genérica y aporta recursos `DEFAULT`
1D/2D/3D, tabla de formatos DXGI (incluidos typeless y depth/stencil), seguimiento persistente de
estado, copias imagen↔buffer e imagen↔imagen, y objetos `ImageView`, `Sampler` y `Framebuffer`. Las
vistas persistentes SRV/UAV/RTV/DSV salen de los allocators offline de 3a.3; también existen SRV/UAV
nulos válidos. ASTC y ETC2 se marcan como convertidos porque D3D12 no los expone de forma nativa.

El dato de Eden está empaquetado por filas, pero D3D12 exige `RowPitch` múltiplo de 256 y offset de
footprint múltiplo de 512. Para no cambiar el layout que consume la caché genérica, cada transferencia
usa un buffer `DEFAULT` temporal: `CopyBufferRegion` reempaqueta/desempaqueta las filas en la GPU y
`CopyTextureRegion` opera sobre el footprint alineado. Esto cubre mips pequeños, capas y texturas 3D.
El gate usa deliberadamente 13×7 RGBA8 (52 bytes por fila, no alineados), hace
`UPLOAD → textura → READBACK`, compara 364 bytes y crea SRV/UAV/RTV/DSV, vistas nulas, sampler y
framebuffer. Pasó con `RunHeadlessBoot returned 0` y sin warnings/errores de Render ni device removal.
Los blits filtrados llegaron en la fase 4.4 (`d3d12_blit_image`); ASTC/ETC2 y conversiones shader
avanzadas son trabajo de paridad de fase 5.

**Prueba Series de 3c (0.2.12.0, logs):** creó el runtime, ejercitó el reempaquetado de una textura
13×7 RGBA8 desde 52 a 256 bytes por fila y recuperó correctamente los 364 bytes. Creó las vistas
SRV/UAV/RTV/DSV, incluidos el primer heap DSV (tipo 3), vistas nulas, sampler y framebuffer. El
present siguió por el scheduler, el boot terminó en 10,890 s con retorno 0 y 850 MiB usados de
5120 MiB. No hubo warnings/errores de Render, `DXGI_ERROR_DEVICE_REMOVED` ni fallback por CPU.

**Fase 4.**
- Una clave tipo `FixedPipelineState` que genera el PSO.
- Una root signature fija por grupo de etapas.
- Remapeo de bindings de los shaders del guest.
- Caché en disco de DXIL junto con la descripción del PSO.

**Fase 5.**
- Shaders de utilidad: ASTC, unswizzle, conversiones y MSAA.
- Stream output.
- Quads y fans.
- Wide lines.
- Logic op.
- Audio XAudio2 2.9 nativo; diseño y gates en `xbox_audio.md`.

El plan largo, con la investigación, está en `~/.claude/plans/ancient-shimmying-creek.md`.

---

## Sincronización con Eden

**Primera fusión** (26 sep 2026): del fork de juanresendiz813, basado en Eden `5219b9f3d` (10 jun),
al Eden `37fe911952`. Fueron 271 commits de Eden y 10 conflictos. Así se resolvieron, para la próxima
vez:

| Archivo | Resolución |
|---|---|
| `AGENTS.md`, `CLAUDE.md` | Los nuestros. Los de Eden son su política contra la IA |
| `src/audio_core/CMakeLists.txt` | Eden hizo SDL3 incondicional; lo volvemos a excluir en `WindowsStore` (arrastra DLLs de escritorio y la app no activa) |
| `src/common/settings_enums.h` | El `GpuAccuracy` de Eden (se quitó `Medium`), más nuestro `Direct3D12` al final de `RendererBackend` |
| `src/core/hle/service/service.h` | El de Eden: también quitó el `constexpr` de `FunctionInfoTyped` |
| `src/dynarmic/CMakeLists.txt` | Nuestra opción `DYNARMIC_UWP_APPCONTAINER` sobre los nombres nuevos de plataforma (`OPENBSD`…) |
| `src/core/hle/kernel/svc/svc_debug_string.cpp` | Eden quitó el hilo de volcado; solo añadimos el observador del centinela |
| `src/dynarmic/.../block_of_code.cpp` | Eden pasó al allocator por defecto de Xbyak; recuperamos el nuestro (solo reserva, `VirtualAllocFromApp`) **solo en Windows** y se lo pasamos al `CodeGenerator` |
| `src/common/host_memory.cpp` | Ahora hay un `Init()` que devuelve `bool`; nuestra rama UWP (reserva privada con commit bajo demanda) va dentro con `return false` en vez de excepciones |
| `src/common/virtual_buffer.cpp` | Eden lo borró (#4219) y lo sustituyó por `sparse_large_vector`, que ya reserva y confirma bajo demanda. Allí portamos lo de UWP: `VirtualAllocFromApp`/`VirtualProtectFromApp` y `AddVectoredExceptionHandler` resuelto por nombre |

**Sin conflicto de texto, pero hubo que adaptar código:**
- `sparse_large_vector.cpp:65`: un `reinterpret_cast<u64>(ULONG_PTR)` de Eden que MSVC rechaza;
  cambiado a `static_cast`.
- **Runtimes D3D12:** la caché genérica ahora pide:
  - `BufferCacheRuntime::{CurrentTick, IsFree, Wait}`.
  - Un `Buffer(runtime, addr, size, sparse_compatible)`.
  - `TextureCacheRuntime::{FlushDeferredClear, CanDownloadMsaa}`.
  - `TextureCacheParams::HAS_MSAA_DOWNLOADS`.
- `Common::Log2Ceil64` pasó a llamarse `Common::Log2Ceil<T>`.

## 4. Lo que sabemos

Revision comparada de los logs PC/Series del 29 sep: ver
[`xbox_log_review_2026-09-30.md`](xbox_log_review_2026-09-30.md). Recuentos confirmados,
con tres trampas de interpretacion: `caches see` es presion sintetica global, no bytes
residentes solo en caches; los ocho avisos de storage fallback Series ocurren en precarga,
que tambien retraduce los environments; el assert de BufferQueueProducer comprueba slots
fuera del maximo activo, no demuestra reutilizacion prematura. Los ReadBlock ausentes
rellenan con cero, pero no prueban corrupcion sin identificar consumidor y mapping.

### La consola (Xbox Series X, UWP Dev Mode, medido con el probe)
- **Adaptador y API:** el adaptador es `SraKmd_arden`, con D3D12 a **FL 11.0**, **SM 6.4** y root
  signature 1.2.
- **Recursos:** binding tier 3 y resource heap tier 2.
- **Memoria:** UMA cache-coherent. Presupuesto de GPU de 4147 MiB. En modo Game la app tiene un
  límite de 5120 MiB.
- **Waves:** siempre de 64 (wave64). Hay int64, fp64, depth bounds y VP/RT index sin GS.
- **Lo que no hay:** logic op, PS stencil ref, operaciones nativas de 16 bits, ROVs, enhanced
  barriers, triangle fans, dynamic depth bias, barycentrics, stencil front/back independiente y
  samplers no normalizados.
- **Typed UAV load:** el flag general dice "yes", pero la consulta por formato dice "no" en todos.
  Sospechamos que es la consulta la que no está soportada. Pendiente de verificar con una prueba
  real.
- **Swapchain:** 1920x1080, `R8G8B8A8_UNORM`, FLIP_DISCARD y 3 buffers.
- **Bug conocido (de la investigación):** `ID3D12PipelineState::GetCachedBlob()` provoca device
  removed. Nunca lo usamos; se cachea DXIL.
- **Sin Agility SDK:** estamos limitados a lo que trae el sistema.

### La cadena de shaders
- **El recorrido:** el SPIR-V de Eden pasa por `spirv_to_dxil` y se firma con
  `IDxcValidator::Validate(InPlaceEdit)`, que es el validador de `dxil.dll` 1.8. La consola lo
  acepta.
- **Parámetros de la traducción:** `DXIL_ENVIRONMENT_VULKAN`, `shader_model_max = 6.4` y reglas del
  validador 1.4.
- **Bindings:** space = set y register = binding. Un sampler combinado se convierte en `t#` + `s#`.
- **Datos fuera de los descriptor sets:** los push constants van a un CBV en `b0 space30` y el
  runtime data a `b0 space31`.
- **Clip space:** Vulkan tiene la Y hacia abajo. Se voltea con `DXIL_SPIRV_Y_FLIP_UNCONDITIONAL`.
- **Trampa:** Mesa desreferencia **siempre** `debug_options` y `logger`. Pasar `nullptr` crashea
  dentro del DLL, y el síntoma es una lectura de 0x0 en `spirv_to_dxil+0x403d`. Hay que pasar un
  struct en cero.
- **Carga de los DLL:** los dos se cargan en runtime desde la raíz del paquete con `LoadLibraryA`
  (`Common::DynamicLibrary`). Si falta alguno, el renderer cae a la ruta CPU y la app no se rompe.

### Compilar Mesa para UWP
- **Por qué un cross file:** meson corre en un entorno x64 de **escritorio**, porque sus sanity
  checks ejecutan programas. Lo de UWP entra por el cross file: `/LIBPATH:<VCTools>\lib\x64\store`,
  `WindowsApp.lib`, `/APPCONTAINER`, `c_winlibs=[]`, `needs_exe_wrapper=true` y `/DMESA_UWP`.
- **Parche 1** (`src/compiler/nir/meson.build`): sin drivers de gallium ni de vulkan, NIR se compila
  como stub y quedan 185 símbolos sin resolver. La condición pasa a
  `(not with_gfx_compute and not with_spirv_to_dxil)`.
- **Parche 2** (`src/util/os_misc.c`): `GetConsoleWindow` no existe en UWP. Se añade
  `&& !defined(MESA_UWP)`.
- **`-Dmesa-clc=auto`:** `system` falla porque no tenemos `mesa_clc`.
- **tar:** el `tar.exe` que aparece primero en el PATH es el de devkitPro y no entiende las rutas
  `C:`. Hay que usar `%SystemRoot%\System32\tar.exe`, o WinRAR. Los errores de symlinks al extraer
  (CI, android_stub, `.clang-format`) no importan.

### Build de Eden (UWP)
- **Mensajes del compilador en inglés:** `VSLANG=1033` en `build-env.bat`. Si el prefijo de
  `/showIncludes` sale traducido, ninja deja de detectar cambios en los headers sin avisar. Requiere
  el paquete de idioma inglés de VS.
- **Orden de includes:** `<dxcapi.h>` va después de los headers de d3d12/windows. Si no, aparecen
  errores de `REFCLSID`/`IUnknown` sin definir.
- **Configuración:** `ENABLE_D3D12` está ON solo con `CMAKE_SYSTEM_NAME=WindowsStore`.
- **Enums:** `RendererBackend::Direct3D12` se añadió **al final** del enum para no romper los configs
  guardados. `WindowSystemType::CoreWindow` lleva el `IUnknown*` del CoreWindow.

### Rendimiento de referencia
- **Framebuffer del NRO** (1280x720, 600 frames):
  - con escalado por CPU: ~17 s en el PC;
  - con el blit por shader: ~10.2 s en el PC y 10.1 s en la Series, es decir, 60 fps.
- **Memoria en la Series durante el boot:** ~750 MiB de 5120.

### Audio XAudio2 2.9 (29 sep 2026)

- El UWP usa XAudio2 por defecto, con PCM16 estéreo a 48 kHz y tres slots persistentes de 960
  frames: 20 ms por slot y 60 ms en vuelo. `audio=null` conserva la ruta silenciosa temporizada.
- `SinkStream::ProcessAudioOutAndRender` sigue haciendo la mezcla, volumen, downmix y underrun. El
  backend solo mantiene el ring y entrega PCM; no duplica reglas del mezclador de Eden.
- El callback de voice únicamente libera un bit en una máscara atómica y despierta al worker. No
  reserva, registra, mezcla, espera ni consulta el dispositivo. El worker es el único productor y
  el único que llama a `SubmitSourceBuffer`.
- `CreateMasteringVoice` usa device id nulo, por lo que XAudio2 2.9 usa el Virtual Audio Client y
  sigue el endpoint predeterminado. Cualquier HRESULT o `OnCriticalError` retira la voice después
  de terminar sus callbacks y cambia a pacing silencioso sin detener el juego.
- La source voice lleva `XAUDIO2_VOICE_NOSRC | XAUDIO2_VOICE_NOPITCH`. Toda la memoria se reserva
  al construir el stream; steady state no crea objetos ni asigna buffers.
- `audio_profile=1` consulta `GetPerformanceData` cada cinco segundos fuera del callback. Registra
  submits, completados, fallos, latencia, glitches, voices y memoria de XAudio2.
- Build UWP completo correcto. `boot_nro` dio `RunHeadlessBoot returned 0` con XAudio2 por defecto y
  con `audio=null`. El NRO no crea AudioOut, por lo que el sonido real se valida con Game C. El
  AppX 0.2.66.0 está firmado y sus EXE/PDB están archivados en
  `build-uwp/symbols/0.2.66.0/`.
- Gate Game C PC de 92 s: 4244 buffers enviados y 4241 completados; los tres restantes seguían en
  vuelo al cerrar. Cero fallos de submit, starvations y glitches; latencia de 1887–1940 muestras
  (39–40 ms), engine estable en 62 KiB y cierre limpio con `RunHeadlessBoot returned 0`. El audio
  queda funcionalmente validado en PC; faltan la corrida de 15 minutos y Series.

### Ruido conocido en los logs (no es un fallo)
- `Failed to find program id for ROM`: un NRO no tiene program id.
- `BSD: Network isn't initialized` y `Unknown engine name: camera/joycon/tas/...`: el frontend
  headless no tiene esos backends.
- Al salir de la app en la consola aparece `0x80010012`: es el desmontaje de COM y no importa.
- Con un juego:
  - `playtime.bin` no existe la primera vez.
  - `ResolveCallerProgramId: Could not resolve caller process_id=0` también sale en escritorio.
  - `Pin count imbalance` sale al cerrar.

### Ruta ASTC GPU + BC3 (predeterminada desde 0.2.59.0)

- `astc=gpu` selecciona BC3 para texturas 2D de una capa y RGBA8 para arrays. El staging conserva
  los bloques ASTC del guest; no hay decode ni recompression en CPU.
- El decoder D3D12 es una variante del shader ASTC compartido con dos constantes adicionales:
  longitud del SRV raw y primera fila de bloques. Vulkan conserva sin cambios su ABI de siete
  push constants.
- Un dispatch ASTC produce una banda RGBA8, una barrera UAV la hace legible, el segundo dispatch
  genera BC3 en un buffer raw y `CopyTextureRegion` copia su footprint a la textura final.
- Los temporales persistentes tienen presupuestos de 32 MiB RGBA8 y 8 MiB BC3. Al crecer se libera
  el recurso anterior mediante `Scheduler::DeferRelease`; no se espera un fence en el upload.
- Si falta el decoder o el encoder, la imagen no recibe `AcceleratedUpload` y el texture cache usa
  la conversion CPU existente. `astc=bc3` queda como referencia CPU durante los gates.
- Ambos dispatches rellenan la runtime data de compute de `spirv_to_dxil` (grupos y grupo base
  cero). Sin ella, el grupo base heredaba basura de la root signature anterior y las texturas
  salian con bloques rojos o con el contenido de otra imagen (ver `xbox_d3d12_phase4.md`).
- Diagnosticos: `astc_verify=1`, `astc_sync=1`, `astc_fresh=1`.
- Verificado con Game C en PC y Series (0.2.58.0). Es el valor por defecto desde 0.2.59.0;
  `astc=bc3` vuelve a la ruta de CPU.

### Chivato de device removal y coste por draw (0.2.59.0)

- `CheckRemovedAfter` llama a `GetDeviceRemovedReason`, que entra al kernel. Tras cada descriptor
  de cada draw costaba ~40% de la CPU de los draws. Esos sitios usan `CheckRemovedAfterDescriptor`,
  activo solo con `descriptor_checks=1`. El resto de los chivatos (creacion y submit) sigue igual.
- Contadores por ventana (`D3D12 GPU thread:` en el log): coste por draw por fases, clears,
  dispatches, trabajo fuera de draws y esperas del juego a la GPU (`nvhost_ctrl`).

### State tracking y `gpu_profile` (despues de 0.2.60)

- El backend conserva el estado D3D12 dentro de una command list y evita repetir heaps, root
  signature, PSO, attachments, viewports, scissors, blend, stencil y topologia. Cada `Reset`, cambio
  de canal o helper grafico invalida lo necesario.
- El callback de `Reset` no toca directamente las dirty flags: al cerrar puede ejecutarse cuando el
  payload Maxwell ya no existe, incluso antes de `ReleaseChannel`. Solo deja una invalidacion
  pendiente; el siguiente draw/clear/dispatch la aplica con un canal vivo.
- Los vertex/index buffers y el estado fijo usan las tablas dirty de Maxwell; nunca se marcan todos
  los vertex buffers en cada draw.
- Los dos heaps shader-visible son siempre los mismos. Se fijan una vez tras cada reset; Microsoft
  advierte que cambiar heaps puede provocar un flush del pipeline.
- `gpu_profile=1` en `boot.cfg` activa los cronometros finos por draw. Apagado, `LapTimer` y
  `ScopedNsTimer` no consultan `steady_clock` ni actualizan sus contadores.
- Con el perfil activo, `D3D12 GPU thread:` incluye fast-path de pipelines, creacion de CBV, reparto
  streamed/persistente/nulo y copias de vistas. Usar una corrida con cache caliente y sin capa de
  debug para comparar rendimiento.
- Un root CBV no puede ser nulo y no lleva limite de tamaño: GPUVA cero o acceso fuera del recurso
  es comportamiento indefinido. Solo se considerara una ruta hibrida si la medicion de CBV queda
  por encima del 10% y el layout cabe en los 64 DWORD de la root signature.
- Medicion Game C PC (116 s, sin debug layer): 7,6--11,8 us/draw, mas de 99,7% de hits en la
  transicion de pipeline y 1,1--1,3 us/draw grabando estado. `CreateConstantBufferView` consume
  aproximadamente 2% del tiempo activo, no alcanza el gate para root CBV.
- Perfil profundo Game C: todas las esperas `nvhost_ctrl` son del syncpoint 1, reservado por el
  canal grafico GPFIFO. El trabajo fuera de draw se concentra en procesar submits Maxwell
  (~2,0--2,7 ms/frame); `TickWork`/composite cuesta ~0,3--0,6 ms/frame e invalidaciones cero. El
  cronometro por argumento macro es diagnostico e intrusivo: incluye el draw ejecutado por la macro
  y millones de lecturas de reloj. Para optimizar, medir una vez por `MacroEngine::Execute` y
  agrupar por hash/metodo. El JIT x64 estaba activo.
- La traza activada con `T` incluye queue/acquire/release de BufferQueue, slots, frame numbers,
  estado al bloquear el dequeue, submits y fences. Game C solicita `swap_interval=2` durante las
  caidas; Nvnflinger y `Conductor` lo respetan igual con Vulkan y D3D12.
- Game C PC, misma zona: sin fastmem, 39 de 67 frames trazados pidieron intervalo 2 y solo hubo 67
  composites en 120 vsyncs; con `fastmem=1`, uno de 117 pidio intervalo 2 y hubo 117 composites.
  La ruta paginada de memoria guest es el cuello que dispara el fallback a 30 Hz. La ociosidad
  agregada de los cores no descarta que un hilo guest sea el limitante.
- `force_swap_interval=1` existe solo para diagnostico y esta apagado por defecto. No es una
  solucion: desacopla presentacion y simulacion, produce velocidad irregular y eleva las esperas
  de fence. En Series fastmem completo sigue bloqueado por el limite de vistas del AppContainer.
- **Fastmem hibrido UWP (experimental):** `fastmem=hybrid` conserva 4 GiB lineales con memoria privada salvo una
  seccion sparse de 384 MiB en la cola del Application Pool (`0xe8000000..0xffffffff`). Solo esa
  cola se aliasa en el arena; el resto fault/recompila a page table. La vista canonica mas aliases
  no puede superar 896 MiB (384 + 512); un alias que no quepa se omite sin abortar.
- El supuesto de usar el inicio del Application Pool era incorrecto: Game C no mapeo bytes en esa
  franja. El histograma mostro ~1517 MiB de mappings hacia el extremo alto; la cola de 384 MiB
  quedo cubierta completa en PC, sin fallos y con salida 0.
- El gate de Series 0.2.63.0 descarto el hibrido como ruta normal: solo cubrio 388 de 2637 MiB
  solicitados (~15%), se sintio mucho mas lento y termino a los 210 s con `std::bad_alloc`, usando
  5021 de 5120 MiB. No hubo fallos de vistas (`skipped=0`, `failures=0`) ni device removal.
- Desde 0.2.64.0, `fastmem=1` es automatico y elige page-table en Xbox; `fastmem=hybrid` conserva
  el experimento explicito. `fastmem=full` conserva la prueba completa y vuelve al hibrido si falla.
  `fastmem_hot_mib=N` permite 128--448 MiB (384 por defecto). Cada arranque hibrido comprueba
  coherencia backing/alias con 64 KiB antes de entregar el arena a Dynarmic.
- AWE no sirve: `AllocateUserPhysicalPages` requiere `SeLockMemoryPrivilege`, es desktop-only y
  sus paginas no se pueden mapear simultaneamente en dos direcciones. El limite de ~1 GiB de
  vistas sigue siendo una medicion de la consola, no una garantia publicada por Microsoft.

### Page table JIT limpia (0.2.65.0)

- `absolute_offset_page_table` ya estaba activo en AArch64 y AArch32. La penalizacion restante era
  que Dynarmic leia la entrada canonica empaquetada: por cada load/store aplicaba la mascara de
  atributos, comprobaba el bit marcado y, segun la direccion del backing, extendia el signo.
- Cada proceso tiene ahora una segunda tabla dispersa exclusiva del JIT. Sus entradas contienen
  solo el offset absoluto limpio; cero selecciona el callback. El camino normal queda en cargar
  una entrada, probar cero y sumar la direccion guest.
- La tabla canonica conserva tipo, bloque y marcas para Memory, debugger y rasterizer. Al mapear se
  publica primero la metadata y despues el puntero JIT; al desmapear o marcar debug/cache se borra
  primero el puntero JIT. Asi un acceso concurrente cae de forma segura al callback y nunca usa un
  host pointer viejo. Los permisos guest viven en `KPageTable`; `Memory::ProtectRegion` solo cambia
  proteccion del arena cuando existe fastmem y no modifica la traduccion de la ruta page-table.
- Debug pages, rasterizer-cached, MMIO/no mapeadas y accesos que cruzan pagina mantienen los
  callbacks existentes. Al volver a Memory se repone la entrada limpia. Un cambio de proceso usa
  su propio par de tablas, por lo que no hay estado traducido compartido que invalidar.
- No se anadio una micro-TLB software: el hit requeriria tag, comparacion y salto antes de la unica
  carga indexada que ya hace la page table, y ademas reservaria registros en todos los bloques.
  Tampoco se puede reutilizar a ciegas una traduccion entre stores/callbacks que pueden cambiar
  permisos. La tabla limpia realiza el objetivo del fast path sin introducir ese segundo lookup.
- Gate PC: build UWP completo y `boot_nro` con `fastmem=0`, salida 0; 961 MiB comprometidos al final.
  La tabla es `SparseLargeVector`, por lo que reservar el segundo espacio no compromete todas sus
  paginas.
- Game C PC manual, 110 s, `fastmem=0 gpu_profile=1`: termino limpio con salida 0. En gameplay
  estable hubo ventanas de 300 frames en 302 y 318 vsyncs (16,78 y 17,67 ms/frame, ~57--60 fps).
  Zonas con carga oscilaron entre 352 y 413 vsyncs por 300 frames (~44--51 fps). La linea base
  page-table anterior habia producido solo 67 frames en 120 vsyncs (~33,5 fps) en el recorrido
  medido; la comparacion no es A/B exacta de posicion, pero justifica el gate en Series.
- En las ventanas estables el draw D3D12 siguio en 7,6--9,2 us y el renderer clasifico el tiempo
  como `mostly guest CPU`: la mejora no procede de abaratar draws. No hubo device removal ni fallo
  de Render. Durante un hitch de creacion de 648 recursos aparecieron ocho asserts recuperables de
  `BufferQueueProducer` por un slot no libre; son un problema de pacing separado, no de traduccion
  de memoria.
- Series, logs de Descargas del 29 sep 2026, revisados el 30 sep: paquete 0.2.66.0,
  adaptador `SraKmd_arden`, `fastmem off`, shaders asincronos y XAudio2 activos. Por version,
  esta build incluye la tabla limpia; los logs no tienen un marcador dedicado que identifique
  esa tabla. `system.Run()` empieza a los 55,25 s y el log llega a los 179,35 s (~124 s
  de emulacion). No hay `Critical`, device removal, `bad_alloc` ni crash registrado; tampoco
  `RunHeadlessBoot returned 0`, por lo que no se acredita un cierre limpio ni ausencia visual
  de corrupcion. Hay dos PSO rechazados del mismo par VS `d9effdee28edb3b2` / PS
  `e721dbbf095a71c4`, independientes de una prueba de traduccion de memoria.
- Rendimiento Series: las ventanas con ~177000--193000 draws y ~900 dispatches por 300 frames
  (unos 590--640 draws/frame) dan 36,52 / 27,56 / 26,11 / 30,07 / 32,22 / 23,00 ms/frame,
  equivalentes a 27,4 / 36,3 / 38,3 / 33,3 / 31,0 / 43,5 FPS. Son frames nuevos del guest,
  confirmados por `D3D12 frame chain`, sin forzar swap intervals. Las ventanas tempranas de
  18,39--19,06 ms (~52--54 FPS) tienen bastante menos draws y no representan la misma carga.
  Frente a la referencia historica de ~33,3--34 ms en gameplay, hay indicios de mejora y ya
  no todas las ventanas quedan a 30 FPS; no es un A/B del mismo recorrido y XAudio2 y otros
  cambios impiden atribuir un porcentaje exacto exclusivamente al JIT. Todas las ventanas
  siguen clasificadas `mostly guest CPU`; incluso sin stalls de pipeline hay una de 32,22 ms.
  Pico observado en diag: 4541 de 5120 MiB. Funcionamiento observado en Series; quedan la
  comparacion controlada contra 0.2.64.0, el gate prolongado y la confirmacion visual/cierre.

### Pool de placed textures (despues de 0.2.65.0)

- El hitch reproducible de Game C creaba 648 recursos en un frame: 438,7 de sus 610 ms estaban en
  `CreateCommittedResource`, aunque los 647 uploads sumaban solo 8,02 MiB. Cada committed resource
  crea tambien un heap implicito y lo hace residente.
- `TextureResourceAllocator` mantiene bloques DEFAULT de 64 MiB y crea `CreatePlacedResource`
  dentro de ellos. Separa texturas normales de RT/DS para funcionar en Resource Heap Tier 1. Los
  rangos libres se fusionan y solo vuelven al pool despues de que la fence retire el recurso.
- Si `CreateHeap` o `CreatePlacedResource` falla, registra el HRESULT y crea el committed resource
  anterior; el renderer no aborta por el allocator.
- Microsoft documenta que crear heaps puede ser lento, recomienda hacerlo fuera del render thread
  y presenta placed resources como separacion de recurso y memoria:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency y
  https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html. El diseño de pools coincide con
  D3D12MA: https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.
- Gate PC de Game C, mismo evento: 648 creaciones bajaron de 438,7 a 12,4 ms (-97,2%); el frame
  completo paso de 610 a 143 ms (-76,6%). Se crearon 2955 placed resources, cero fallbacks y cero
  fallos; 576 MiB de heaps, 530 MiB vivos de pico y cero vivos al cerrar. `RunHeadlessBoot` devolvio
  0 y no hubo device removal.
- Quedan 125,1 ms de `other work` en ese frame, principalmente preparar/grabar 647 uploads pequenos.
  Es el siguiente bloque a instrumentar y agrupar; ya no conviene mover la creacion a workers antes
  de medir esa ruta.

### Vistas de textura y DRED (despues de 0.2.65.0)

- El perfil fino del siguiente hitch localizo el coste en `FindOrEmplaceImageView`: unas 700 vistas
  consumian 81--108 ms. El repack y la grabacion real de los uploads solo consumian 9--13 ms.
- D3D12 creaba RTV/DSV para toda vista cuya imagen permitiera render, aunque la vista fuese solo de
  shader. Ahora sigue la marca `ImageViewInfo::IsRenderTarget()` que tambien usa Vulkan: una vista
  de shader crea SRV/UAV y una vista attachment crea RTV/DSV. El SRV natural de un attachment queda
  lazy hasta que un shader lo solicite.
- DRED ya no se fuerza en cada arranque. `dred=1` activa breadcrumbs y page-fault tracking al
  diagnosticar un device removal; apagado se mantienen `GetDeviceRemovedReason` tras submits y el
  informe normal. Microsoft cifra los breadcrumbs automaticos en 2--5% tipico y documenta coste
  adicional de creacion/destruccion por el page-fault tracking:
  https://microsoft.github.io/DirectX-Specs/d3d/DeviceRemovedExtendedData.html.
- Los RTV/DSV viven en heaps CPU-only. D3D12 copia su contenido al command list durante
  `OMSetRenderTargets`, por lo que no requieren residencia ni sincronizacion con la GPU:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/non-shader-visible-descriptor-heaps.
- El perfil dentro de `SlotVector::insert` descarto la estructura: buscar un slot, marcar su bit y
  el propio reloj costaban practicamente cero. El `placement-new` concentraba 60,6--89,9 ms por
  lote porque el constructor delegado de `ImageView` materializaba inmediatamente el SRV especial
  que representa una textura 3D como array 2D. Ese SRV de la copia por slices ahora queda vacio y
  `Handle(ColorArray2D)` lo crea solo si un shader llega a pedirlo; el SRV 3D conserva la propiedad
  de su descriptor provisional.
- Gate PC manual de Game C: 724 vistas bajaron de 68,3 a 2,6 ms (-96,2%) y su construccion de 67,7
  a 2,0 ms (-97,0%). Otro lote comparable paso de 61,0 ms para 688 vistas a 3,3 ms para 691
  (-94,6%). En el primer hitch, draws bajaron de 128,3 a 63,0 ms y submit de 177,3 a 114,6 ms. No
  hubo device removal; permanecen el PSO invalido conocido y los asserts recuperables de
  `BufferQueueProducer`.
- El siguiente cuello ya no son las vistas. En ese hitch quedaron 80,8 ms en inserciones de
  imagen: 56,2 ms en `RefreshImage`, 21,3 ms en preparacion/creacion de imagen, 18,6 ms en repack y
  25,9 ms en el backend de uploads (algunas fases estan anidadas). El siguiente perfil debe partir
  `RefreshImage` en busqueda de solapes, conversion de copias, staging/unswizzle, transiciones y
  grabacion; no volver a cambiar descriptores sin datos.

### PSO rechazado y sampler MIN/MAX de Game C (30 sep 2026)

- La capa de debug confirmo `CreateInputLayout` mensaje 61: RGBA8 UNORM en offset 14 requiere
  alineacion a cuatro. Se sustituyo por dos pares RG8 normalizados, offsets 14 y 16, y el
  recompiler recompone las cuatro componentes. Mismo tratamiento para SNORM y lecturas indirectas;
  estas variantes adicionales no tienen todavia un gate especifico. No se repackean buffers.
- El MAX puntual de Game C es exactamente point normal: solo hay un texel en el footprint.
  Se canoniza tambien en PC; desaparece la aproximacion para ese caso. MIN/MAX filtrado o
  anisotropico sin soporte sigue pendiente y conserva un warning distinto.
- La prueba encontro tambien un binding compute residual de ASTC/BC3. Graphics/compute comparten
  PSO en D3D12: se centralizo la cache en `Scheduler::SetPipelineState`, usada por todos los
  helpers y el rasterizador, y se limpia tras `Reset`. Evita bindings redundantes y restaura el
  graphics correcto cuando el helper anterior uso compute.
- Gate PC: Game C 75 s con debug layer, ambos PSO antes rechazados construidos, MIN/MAX exacto
  ejercitado, cero errores Render/capa de debug/PSO rechazados; cierre 0. Persisten ocho asserts
  recuperables de BufferQueueProducer. No se midio mejora A/B de FPS. Evidencia en
  `build-uwp/log-review-2026-09-30/pc-pso-minmax-fixed-debug.txt` y `pc-pso-minmax-fixed-diag.txt`.
- Diseno, comparacion con Vulkan, fuentes Microsoft/Khronos y gate Series pendiente en
  [`xbox_d3d12_phase4.md`](xbox_d3d12_phase4.md#pso-de-game-c-y-minmax-puntual-correccion-del-30-sep-2026).
- Paquete local 0.2.67.0 preparado y firmado en `build-uwp/package/eden-xbox.appx`, EXE/PDB
  archivados en `build-uwp/symbols/0.2.67.0/`. Usa `game=game.nsp` ya presente en LocalState,
  `play=1 fastmem=0 audio_profile=1`, sin debug layer ni entradas automaticas. Instalar por
  Device Portal en modo Game, repetir el recorrido y traer log/diag de Series a Descargas.

### Depth, CPU guest y cargas: correcciones y gates (30 sep 2026, 0.2.68.0)

- Depth: el warning de Game C era un falso positivo (depth test off). Ahora se comprueba la
  escritura efectiva; muestrear un attachment con escritura usa snapshot GPU perezoso y deja
  writable el DSV original. La prueba D32 de 13x7 conserva 0,25 en snapshot y 0,75 en original.
  Game C no ejercita escritura real con feedback: contador 1 corresponde al self-test.
  Pendientes D24/S8, MSAA y semantica entre fragmentos dentro del mismo draw; el snapshot toma
  el contenido anterior al draw. Diseno y fuentes Microsoft/Dolphin/Vulkan en fase 4.
- Cargas: decoder comun comparte la mejora con Vulkan. Copias de 16 bytes para deswizzle
  1/2/4/8 Bpp, colas escalares y sin requisito de alineacion. Gate 5.400 casos exactos.
  RGBA8: 3,07x en primera medicion aislada y 2,66x al repetir durante la prueba PC. No FPS A/B.
- CPU: perfil opt-in cpu_profile=1 por core. Millones de lecturas lentas escalares y muy pocas
  vectoriales: se descarto cambiar Read128. La ruta RasterizerCached reutiliza el puntero
  ya resuelto y evita traducirlo dos veces, conservando la sincronizacion GPU. No hay mejora
  porcentual de CPU demostrada. Tiempo de Run incluye callbacks/traduccion/preemption.
- Trampa detectada: uploads sobre vertices ya ligados pueden dejar COPY_DEST aunque la cache
  generica salte el rebind del stream sin cambios. Se restaura GENERIC_READ si el destino ya
  estaba legible; no se fuerza dirty global ni una barrera nueva en cada draw sin uploads.
- Evidencia y runner local bajo build-uwp/log-review-2026-09-30. El target de tests Catch no
  existe en este preset UWP: el test registrado se ejecuto con un runner local contra el
  objeto de produccion, y el benchmark compara el decoder b3237ab889 compilado con MSVC /O2.
- Pendiente gate Series: instalar 0.2.68.0 en Game, repetir recorrido comparable y revisar
  resultado visual, CPU por core, cargas y errores D3D12; no certificar mejora por un solo FPS.- Gate PC final: Game C 75 s, cpu_profile=1/gpu_profile=1 y debug layer, cierre 0 a 86,109 s
  incluyendo carga/cierre. Cero mensajes de debug D3D12 y cero errores Render; ocho asserts
  conocidos de BufferQueueProducer. Prueba depth pasada, snapshot solo en self-test. Logs:
  pc-depth-loads-final-debug.txt y pc-depth-loads-final-diag.txt. Persisten errores del fichero
  play_time al cerrar, sin fallo del renderer. No se certifica mejora global CPU/FPS.- Paquete 0.2.68.0 creado y firmado; exe/pdb archivados en build-uwp/symbols/0.2.68.0.
  Juego manual sin limite: play=1, fastmem=0, audio_profile=1, gpu_profile=1, cpu_profile=1,
  sin debug layer. Es un paquete de diagnostico: los perfiles activados tienen sobrecoste.
  Pendiente instalar en Series y devolver log/diag de Descargas. No se hizo commit.### Siguiente objetivo: FPS y estabilidad de frame times (30 sep 2026)

- Cambios depth/cargas anteriores comiteados como 111678c38, sin push.
- El usuario requiere jugar sin limite y cerrar con Q cuando haya gameplay/tirones; no usar
  RunSeconds ni entradas programadas para certificar rendimiento. La sesion automatica corta
  anterior queda en pc-fps-short-baseline{,-diag}.txt y no valida gameplay prolongado.
- Primer paso: percentiles nearest-rank p50/p95/p99 por ventana de 300 intervalos presentados,
  ademas de media/FPS y maximo. Array fijo y sort una vez por ventana, sin asignaciones por frame.
  Miden ritmo de presentacion, no frames unicos del guest ni utilizacion CPU. Menu, cargas y
  gameplay deben separarse al interpretar ventanas; p99 se refiere a 300 muestras, no toda la
  sesion ni al promedio de los frames mas lentos.
- Candidato inicial pequeno: SamplerHeap buscaba creando/destruyendo un vector en cada draw,
  incluso en hits. Hash/equality transparentes de C++20 comparan span con la clave almacenada;
  solo un miss crea una clave propietaria. Mismo hash y comparacion completa de tamanos/elementos,
  mismos descriptores y reset tras Finish. Sin cambio de coherencia o lifetime GPU. No atribuir
  una mejora global FPS antes de A/B con recorrido comparable; el cuello principal sigue pendiente.
- Referencias primarias: Microsoft PIX Metrics para detectar outliers y CPU/GPU:
  https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/layouts/pix-metrics-layout
  y unordered_map de MSVC:
  https://learn.microsoft.com/en-us/cpp/standard-library/unordered-map-class.
  Vulkan usa bancos/pools de descriptores; no trasplantar sus reglas de sets al heap D3D12.
- Sesion manual siguiente: play=1, fastmem=0, sin debug, gpu_profile/cpu_profile desactivados
  para medir el comportamiento normal. Q cierra limpiamente; T registra dos segundos de la
  cadena de frames si se quiere localizar un tiron. Revisar log/diag al cierre antes de cambiar
  nuevamente el binario. Candidato y percentiles aun sin commit/gate prolongado.
#### Primera sesion manual de FPS: cierre con Q (30 sep 2026)

- Evidencia preservada en pc-fps-manual-samplers.txt y pc-fps-manual-samplers-diag.txt,
  dentro de build-uwp/log-review-2026-09-30. CPU/GPU profiling detallado y debug desactivados.
  Q a los 90 s de ejecucion del guest; cierre 0 a 99,391 s incluyendo carga/cierre.
- Ventanas finales de 300 presents con unos 600 draws/frame:

| Fin en log | FPS de presents | p50 ms | p95 ms | p99 ms | Max ms |
|---|---:|---:|---:|---:|---:|
| 72,25 s (incluye cargas) | 29,56 | 16,91 | 74,96 | 373,87 | 905,06 |
| 80,95 s | 34,48 | 33,12 | 43,31 | 91,23 | 116,67 |
| 89,09 s | 36,88 | 32,47 | 43,02 | 67,68 | 89,89 |
| 95,02 s | 50,57 | 16,87 | 33,41 | 34,76 | 66,44 |

- No mezclar estas ventanas con menus de 59 FPS ni atribuir mejora frente a la sesion corta:
  el recorrido manual difiere. FPS aqui es ritmo de presents, no una medida independiente del
  tiempo del guest ni del numero de frames unicos. P99 muestra claramente la irregularidad.
- Hitches sin uploads: a 66,55 s un frame de 133 ms incluye 128 ms de espera por comandos del
  guest; a 72,18 s uno de 122 ms incluye 115,9 ms de espera; a 79,65 s uno de 100 ms incluye
  93,1 ms. Sin waits de fences/PSO en esos frames. La ausencia de comandos apunta a guest CPU,
  planificacion o dependencias de hilos; NO demuestra que el JIT por si solo sea responsable.
- Las cargas agravan los picos: a 63,54 s frame de 905 ms, 121 uploads/76,70 MiB, 29,1 ms de
  decode CPU y 713,2 ms de espera por comandos. A 46,52 s pico de 1.144,51 ms con 106 uploads.
  El hilo GPU usa 69,7/105,4 ms de GPU busy respectivamente; son magnitudes parcialmente
  superpuestas y no deben sumarse como fases seriales. En la ventana de 80,95 s, 8,7 s de
  tiempo incluyen 6,46 s idle, 74 ms de fence waits y 3,05 s de GPU busy solapado.
- Estabilidad: cero errores Render, sin device removal y cierre limpio. Debug desactivado:
  esta sesion no valida la capa de debug. Tres asserts recuperables conocidos BufferQueue;
  hay errores de teclado, avatar ausente, cuatro lecturas Device ReadBlock no mapeadas y
  fichero play_time al cierre; el log no esta libre de errores generales.
- Prioridad siguiente: (1) distinguir ejecucion/compilacion JIT, callbacks de memoria y espera
  del guest mediante perfil dirigido; no cambiar flags CPU inseguros, prioridades o VSync a
  ciegas; (2) reducir uploads/decode en las transiciones con picos, preservando coherencia;
  (3) el cambio de sampler elimina una asignacion por hit, pero no aborda el cuello principal.
  Su recorrido manual construye y reutiliza tablas sin errores Render, sin FPS A/B demostrado.
  El siguiente A/B debe usar tramo y recorrido manual comparables, mismo cache y mismos perfiles;
  el usuario decide el cierre con Q. No hay commit del candidato sampler/percentiles todavia.
- Investigacion y diseno del objetivo 60 FPS: [xbox_performance.md](xbox_performance.md). Fetch A64 valida una sola pagina (alineacion contractual), perfil muestreado de reads y tiempo de misses OnCPURead implementados; build incremental correcto. Gate manual dirigido pendiente; 60 FPS aun no demostrado.

#### Perfil dirigido completado y candidato JIT (30 sep 2026)

- Sesion callbacks: Q tras 105 s de guest, retorno 0; lectura muestreada 63--143 ns,
  misses OnCPURead decenas de ms/ventana; no explican solos 30--32 FPS. Render sin
  errores, dos asserts BufferQueue, sin debug; 4907 MiB app al cierre. T: 72/92 frames
  encolados en 120 vsyncs. Intervalo 1/2 pedido coincide con el efectivo; no forzar 1.
- Sesion fases JIT: Q tras 91 s, retorno 0 a 101,062 s; cero Critical/errores Render,
  sin debug, 4884 MiB app. Logs pc-fps-manual-jit-phases{,-diag}.txt preservados en
  build-uwp/log-review-2026-09-30. Agregado de ventanas: 796895 bloques, 54304 ms
  compilando y 23686 ms protegiendo paginas (43,6% del total; fases y cores solapados).
  Ventana de 25,21 FPS: 105795 bloques, 6953,5 ms compilacion, 2975,7 ms proteccion.
- Candidato sin commit: handlers A64/A32 static constexpr (Emit A64 MSVC baja de
  5712 a 352 bytes de pila y elimina reconstruccion de tablas/__chkstk); omitir
  formateo virtual de nombres del perf-map no-op en Windows; Unpatch A64 calcula
  indice FastDispatch con CRC software existente y evita ejecutar lookup JIT con
  dos cambios RX/RW por invalidacion. Conserva W^X y coherencia de invalidaciones.
  No elimina las transiciones requeridas para emitir bloques nuevos. Perfil exacto
  opcional añade duracion de invalidaciones para medir esta parte.
- Regresion hash software frente a instrucciones Xbyak: 262144 casos PASS, ramas
  con/sin SSE4.2. Runner en tools/xbox/tests/jit-fast-dispatch.cpp. Usa entorno
  vcvarsall x64 escritorio para el runner: build-env selecciona CRT Store y faltan
  DLLs APP al ejecutar un .exe suelto. App UWP sigue compilada con build-env.
- Build incremental y diff-check correctos; gameplay del candidato, A/B comparable
  sin perfiles y gate Series pendientes. Datos, fuentes y limites en xbox_performance.md.

#### Revision manual del candidato JIT

- pc-fps-manual-jit-optimized{,-diag}.txt preservados en log-review-2026-09-30.
  Q tras 97 s de guest y retorno 0 a 107,140 s. Sin debug; cero errores Render y
  dos asserts recuperables BufferQueue. App 4915 MiB al cierre; no certificar limite Series.
- Compile agregado normalizado: 68,14 -> 67,02 us/bloque (1,6% menos observado).
  Primera ventana: 65,25 -> 59,77; ultimas cuatro: 70,48 -> 66,42. Recorrido manual,
  composicion de bloques y scheduling distintos; sin repeticion A/B ni significancia.
  No presentar 8,4% favorable inicial como mejora global. Las tablas estaticas reducen
  trabajo en ensamblado, pero no se demuestra mejora sostenida de FPS.
- Cero invalidaciones JIT en las ventanas: Unpatch no se ejercito. Protecciones por
  bloque 2,324 -> 2,325; sigue el coste RX/RW de emitir bloques nuevos.
- Ultimas cuatro ventanas: 54,21/50,99/51,14/51,37 FPS, p99 34,22/41,56/39,59/33,57 ms.
  Persisten 31--35 FPS, p99 hasta 453,17 ms con cargas y un hitch final de 150 ms
  con idle GPU 143,3 ms, sin uploads/fences/PSO. Las trazas T capturan 110/114 frames
  nuevos por 120 vsyncs (~55/57 FPS); no hay vsync perdido, faltan frames nuevos.
- Gate funcional manual PC correcto con asserts conocidos. A/B sin perfiles y Series
  pendientes. Siguiente cuello: emision/proteccion de bloques nuevos y dependencias
  framebuffer/guest; no invalidaciones ni reads escalares. Detalle en xbox_performance.md.

#### Comparacion fastmem Full con el mismo candidato (PC)

- Corrida pedida por el usuario: play=1, fastmem=full, cpu_profile=1. Arena Full de
  512 GiB/seccion 4096 MiB confirmada en HostMemory. Q tras 84 s, retorno 0 a 94,656 s.
  pc-fps-manual-fastmem-full{,-diag}.txt preservados en log-review-2026-09-30.
- Ultimas 900 presents: FPS agregado 51,17 sin fastmem -> 51,82 Full (+1,3% observado);
  p99 por ventana 41,56/39,59/33,57 -> 39,03/33,43/49,63 ms. Recorridos y duracion
  distintos, no A/B controlado. No demuestra mejor estabilidad ni 60 FPS sostenidos.
- Callback reads escalares/present 16673,4 -> 719,2 (~95,7% menos); Compile medio
  66,63 -> 62,87 us/bloque; Run elapsed/core agregado por present 20,43 -> 14,94 ms,
  no utilizacion CPU. Full funciona y elimina trabajo, pero queda otro limite de ritmo.
- Render sin errores, cierre limpio, ocho asserts BufferQueue frente a dos sin fastmem;
  no causalidad demostrada. First-chance AV registradas: 32, app continua; no tasa total
  de faults. App memory reportada 4915 -> 3265 MiB al cierre, pero Full usa file-backed
  DRAM y no es prueba de que la RAM fisica total baje igual ni de viabilidad en Series.
- T: 51/81/91 frames nuevos encolados en 120 vsyncs, con intervalos 2 frecuentes y
  ComposeWaitEnd practicamente cero. Detalle y comparacion completa en xbox_performance.md.
  Mantener fastmem Full como diagnostico PC; default Xbox no cambia, gate Series pendiente.

#### BufferQueue: hipotesis acotada y siguiente captura

- Correlacion ReleaseBuffer->fin dequeue en T: p50 40--48 us, max 112 us entre las
  trazas sin fastmem/Full. Se excluyen bordes y waits sin release observado. Despertar
  host rapido en estas muestras: no culpar notify ni scheduler host de esos waits a ciegas.
  Falta distinguir IPC/HLE y la vuelta del hilo guest a ejecucion.
- T ahora registra begin/end de SVC 0x18/0x21/0x22 con ID guest original, transicion
  raw Runnable/prioridad y signal VSync. IDs guest permiten emparejar aun con migracion
  de fiber. Tiempos bloqueados no son CPU ni se suman entre hilos. Desactivado sin T.
- Asserts BufferQueue incluyen slot/estado/preallocation/max/override/default/cola;
  no se suprime la condicion ni se altera el conteo. Hace falta contexto para corregirla.
- Volcado de trace lleno se difiere a VSync para no ejecutar logging masivo bajo
  scheduler lock. Capacidad 16384; una captura llena se trunca hasta el siguiente VSync.
- Build incremental correcto; gate manual fastmem=0, cpu_profile=1 abierto, T en
  gameplay y Q del usuario. Todavia diagnostico, sin mejora de FPS certificada.

#### Captura guest waits y ventana T ampliada (2026-09-30)

- Evidencia: build-uwp/log-review-2026-09-30/pc-fps-manual-guest-waits{,-diag}.txt.
  Q tras 109 s, RunHeadlessBoot returned 0; cero Critical y errores Render.
- Las dos capturas T llenaron 16384 entradas: solo cubren 1,55/1,44 s y 93/87
  vsyncs. No tratarlas como ventanas completas de dos segundos.
- IPC del guest 83: 528 llamadas emparejadas, espera maxima 37,994 ms; desde
  el ultimo Runnable hasta fin de SVC, p95 0,06 ms y max 0,20 ms. Las esperas
  largas ocurren antes de Runnable; no prueban retraso general del scheduler.
  Guest 123 tiene un outlier de 24,39 ms desde Runnable a fin de IPC; pendiente
  identificar su dependencia. Ese tramo incluye terminar HLE, no solo scheduling.
- A peticion del usuario, T pasa a 240 vsyncs (~4 s a 60 Hz). Capacidad 65536
  eventos (~2 MiB) para el ritmo observado de ~11k eventos/s, con margen.
  Sigue siendo limitada: mayor actividad puede truncarla. Gate de cuatro segundos
  pendiente de la proxima corrida manual, sin cierre automatico.

- Gate PC posterior: pc-fps-manual-guest-waits-4s{,-diag}.txt, Q a 93 s, retorno 0.
  T completo tres veces: 240 vsyncs, ~4 s y 37362/51958/55766 entradas. Sin
  saturacion. Frames nuevos 112/174/195; intervalos pedidos/efectivos coinciden.
  Release->fin dequeue max 82 us; IPC guest 83 Runnable->fin p95 ~60 us.
  Ocho asserts ahora identifican slot 2 Queued/Acquired fuera del max 2, buffer
  preallocated, override/default 2. Revisar limite por conteo vs indice y slots
  activos antes de corregir. No demuestra causa universal de FPS bajos.
  Dump tarda 0,51--0,79 s: perturba rendimiento despues de T. Detalle en
  xbox_performance.md; Series y A/B normal siguen pendientes.

#### HUD de rendimiento compartido con shaders

- Panel superior derecho reutiliza AppendText/ClearRects de shaders; fuente 3x5
  ampliada a letras/signos y rectangulos unidos por fila. Cache de texto/rects 500 ms,
  dos clears por frame, sin mas PSO/fences/readbacks. Ruta blit y copia CPU cubiertas.
- FPS y FRAME/MAX son presents/intervalos, no coste exclusivo de generacion. CPU
  GetProcessTimes: kernel+user de todos los hilos, 100% por core; MS/F agregado.
  GPUQ usa timestamps completados de la cola D3D12, con retardo; no porcentaje
  global del hardware. Datos no disponibles se muestran '--'. Funciona sin cpu_profile.
- Build UWP incremental correcto; gate visual PC/Series pendiente. Detalles y
  fuentes Microsoft en xbox_performance.md. Sin commit.

- Feedback HUD: 230% confunde; CPU cambiado a 2,30 CORES equivalentes. Usuario
  observa ~60 FPS al volver a zonas preparadas y tirones en zonas nuevas.
- Aviso de T bajo HUD: ID, CAPTURANDO/cuenta atras, GUARDANDO, GUARDADA verde
  persistente; TRUNCADA si llena capacidad antes del final. ID tambien en log.
  No permite iniciar otra T durante Dump. Si VSync esta volcando, GUARDANDO
  puede no presentarse; fin visible en el siguiente frame. Gate T 1 nueva / T 2
  vieja pendiente; sin commit.

- Gate nueva/vieja recibido en pc-fps-new-old-zones{,-diag}.txt: T 1/2 completas
  (~4 s), 137/210 frames nuevos (34,25/52,5 FPS), max gap 153,73/48,59 ms.
  Ultima ventana 59,79 FPS/p99 17,86 ms. JIT de ventanas cercanas 34949->4394
  bloques y Compile 2299,9->299,2 ms agregados; no totales exactos dentro de T.
  Uploads 81->0 y GPU busy similar. Refuerza preparacion/JIT frente a saturacion
  GPU o reanudacion lenta. Ocho asserts previos a T; cero errores Render.
  Usuario cerro con X: proceso terminado, sin retorno/shutdown en diag; no crash
  demostrado ni cierre ordenado certificado. Detalles en xbox_performance.md.

- Candidato JIT siguiente: Patch no inserta listas vacias si no hay incoming links;
  emisores/RSB conservan registro de referencias y lookup de destinos compilados.
  DisableWriting agrupa paginas contiguas ya RW y append para restaurar RX, sin
  tocar huecos. Sin nuevas asignaciones, code cache ni W^X cambiados.
- Build incremental correcto; 24573 casos de rangos y gate real Windows de
  protecciones RX/huecos intactos PASS (tools/xbox/tests/jit-writable-ranges.cpp).
  Evidencia runner en log-review-2026-09-30/run-jit-ranges.bat; CRT escritorio.
  Manual PC y Series pendientes; no afirmar mejora de FPS ni hacer commit.

- Gate candidato recibido pc-fps-jit-ranges{,-diag}.txt: Q 340 s, shutdown/retorno 0,
  cero errores Render, ocho asserts previos a T. T completas: nueva 113 frames
  (~28,25 FPS), max gap147,02 ms; vieja215 (~53,75 FPS), max34,98 ms. Antes137/210
  frames, max153,73/48,59 ms: no mejora clara, instantes/intervalos distintos.
  Ventana nueva con ~35k bloques: Compile65,807->64,316 us/bloque (-2,27%), Protect
  calls2,473->2,388/bloque (-3,44%). Ahorro pequeno observado; tirones sin resolver.
  Gate funcional correcto, eficacia y Series pendientes. Sin commit.

- Perfil A64 ampliado: apertura RW, setup, instrucciones, terminal, deferred, rangos,
  registro (patch+insercion), cierre RX y cleanup. Timers por fase/bloque, no por opcode.
  Protect/patch son anidados; no sumar. cpu_profile=0 no lee reloj en estos timers.
- Totales JIT monotonicos: Read no consume; Take de unico renderer calcula delta
  desde snapshot propio. T toma bordes y registra deltas antes del volcado, con ID
  Frame trace JIT capture N. Llamadas cruzando bordes se cuentan al completar.
  Build correcto; gate nueva/vieja pendiente, diagnostico y sin commit.

- Gate desglose pc-fps-jit-emission{,-diag}.txt: Q108s, retorno0, cero errores
  Render, ocho asserts. NuevaT1:23874 bloques/1528,925ms Compile, Protect645,931ms
  (42,25% Compile agregado), instrucciones456,030ms; cierreRX568,066ms, rangos62,203
  y registro33,149ms. T2/3 solo334/363 bloques, ~24--25ms Compile y ritmo~60FPS.
  Prioridad proteccion e instrucciones; registro/rangos no son el coste principal.
- T2/3 truncadas a~3,9s:65536 eventos, gameplay estable llega~17k eventos/s.
  El aviso era correcto; subir capacidad a131072 (~4MiB fijo) evita ese limite
  observado con margen. Build correcto; nueva capacidad y Series pendientes.
  No repetir el recorrido solo por esas truncadas: datos ya identifican las fases.

- Candidato CFG: restaurar RX con PAGE_TARGETS_NO_UPDATE solo debajo del high-water
  RX previo; paginas nuevas RX normal para inicializar targets. Mantiene W^X/CFG;
  fallback RX normal si FromApp rechaza flag, cache atomic de compatibilidad.
  Perfil registra cfg-preserve-rx/initialize-rx/fallback para verificar uso real.
- Gate desktop FromApp con /guard:cf activo:80000 pares+flush y llamadas indirectas
  al entrypoint inicial/nuevo en misma pagina PASS. Benchmark inicialmente~7% menor,
  repeticion ahorro menor/variable; no FPS demostrados. Build UWP correcto, gate
  AppContainer/Series pendiente. Fuente/test en xbox_performance.md. Sin commit.

- Arranque PC AppContainer del candidato CFG: guest funcionando, cfg-preserve-rx
  registrado y cfg-fallback0 en primera ventana revisada; cero errores Render.
  API acepta flag en esta muestra. Recorrido manual/FPS/cierre y Series pendientes.

- Gate CFG pc-fps-jit-cfg{,-diag}.txt: Q315s, retorno0, cero errores Render,
  tres asserts BufferQueue previos aT. Dos T completas, nueva124frames(~31FPS)
  max232ms, vieja235(~58,75)max38ms. cfg-preserve30394calls, fallback0: API/ruta
  funcionan, sin mejora demostrada; antes nueva126frames max143ms. Compile/bloque
  64,041->68,386us, cierreRX23,794->26,347us, instructions19,102->21,958us:
  muestras distintas y timestamps extra; no atribucion causal. No promover aSeries
  como mejora validada. Capacidad131072 corrige truncacion observada. Sin commit.

- Investigacion externa30sep: prioridad compilacion CPU al primer encuentro
  (T1 CFG26267bloques/1796ms, T2 937/62ms), seguida de transiciones RW/RX.
  Ryujinx PPTC ofrece precedente de perfiles/prewarm; Mozilla batching amortiza
  protecciones. Duplicacion entre JIT por core posible, aun no medida. Siguiente
  gate correlacion por hilo/frames + conteo descriptor/core y ETW CPU/context
  switches; no mas cambios de flags basados solo en microbenchmarks.
- Caso Mozilla/Defender VirtualProtect corregido en2023; PC actual motor1.1.26080.3
  con proteccion activa. Hipotesis de coste externo por medir, no causa afirmada
  para Series. WPR y New-MpPerformanceRecording presentes; no se inicio captura
  ni se altero seguridad. Truco Win32 VirtualAlloc para RX no trasladable:
  VirtualAllocFromApp rechaza protecciones ejecutables. Fuentes y prioridades
  documentadas en xbox_performance.md; investigacion sin cambios de codigo/commit.

- Consulta de migracion a Ryujinx antes de implementar prewarm: port nuevo C#/UWP
  y GAL, no copia directa del renderer C++ de Eden. Reutilizables componentes
  D3D12/Mesa y conocimiento Xbox; caches/bindings/frontend requieren adaptacion.
  Primer gate seria runtime administrado + bloque ARMeilleure en Series; .NET
  Native/AOT no certifican por si solos esa integracion. JIT .NET y JIT del guest
  son distintos. No existe A/B local de rendimiento Ryujinx contra Eden.
  Detalle/fuentes en xbox_performance.md; aun sin cambios de implementacion prewarm.

- Candidato posterior implementado: perfil A64 persistente por title/BuildId/core,
  descriptor relativo+hash+longitud, `jit_prewarm=record/1/0` (default0). Prewarm
  antes de Run con guest parado, solo rangos RX iniciales,64MiB emitidos/core;
 262144 observaciones/core preasignadas, hash durante Translate, fallback normal
  ante diferencia y persistencia por temporal+rename al cierre limpio. UI CPU JIT
  reutiliza progreso. Modulos dinamicos posteriores quedan fuera. No bytes host
  persistidos ni sharing entre cores; detalle y limites en xbox_performance.md.
- Gate automatizado jit-prewarm.cpp PASS con biblioteca Dynarmic UWP real desde
  harness desktop: no ejecucion/cambio de estado al precalentar, reuse sin traducir,
  hash/longitud/ASLR, corrupcion/truncacion/identidad/merge/rangos y reemplazo real
  de archivo. Build UWP incremental correcto. Perfil guardado y FPS manual pendientes.
  No RTTI UWP y include Dynarmic privado: usar entry point ligero jit_prewarm.h.
- Primera corrida record invalidada: filtro comparaba permisos UserMask contra
  UserReadExecute con bits KernelRead incluidos, seleccionando cero rangos RX.
  Se corrige en ambos operandos y log incluye RX ranges. Usuario Q65s, retorno0;
  sin perfil guardado, no evidencia FPS del candidato. Archivos pc-prewarm-record-
  invalid-rx{,-diag}.txt. Build corregido correcto; relanzar aprendizaje.
- Aprendizaje corregido pc-prewarm-record: Q89s/retorno0, Render0, dos asserts
  previos aT. Perfiles guardados0/1/2:262144/222383/211400, core3 sin observaciones;
  core0 descarta84504 al limite. Checksum/identidad/orden/longitud de los tres PASS,
  backup ignorado antes de corrida warm. T nuevas110frames/33755bloques(~27,5FPS),
  recorrida225/4042(~56,25FPS); ambas completas240vsync. Warm lanzado, comparacion
  y cobertura real pendientes. No interpretar limite como resultado PPTC.
- Warm arranque confirmado:464635 bloques aceptados,0 rechazados,231292 fuera
  del presupuesto;192MiB emitidos/3cores,~26,1s antes deRun. Gameplay/T/Q pendientes.
- Gate warm cerrado Q64s gameplay/retorno0, Render0, cinco asserts previos aT.
  T1 record->warm110->154frames(~27,5->38,5FPS),33755->19545Compile(-42,1%),
  2241->1353ms(-39,6%),p99122,77->84,36ms. T2 empeora225->189frames
  (~56,25->47,25FPS),4042->5229Compile. Beneficio parcial observado, muestras
  manuales no deterministas; no60 sostenidos ni mejora general/Series certificada.
  Prioridad siguiente cobertura/prewarm por demanda y misses clasificados:
  limite omite231292 y seleccion por descriptor favorece PC/FPCR, no gameplay.
  Core0 merge reemplaza60448 entradas por cap; no serializar bytes host a ciegas.
  Evidencia pc-prewarm-warm{,-diag}.txt y tablas en xbox_performance.md. Sin commit.


Actualizacion candidato FPS (30 sep 2026): prewarm prioriza compilaciones observadas
durante T, conserva perfiles v1 y guarda v2; cupo de observaciones reservado para T,
mismo presupuesto64MiB/core. Contadores T clasifican misses por cobertura, presupuesto,
core/FPCR, codigo distinto o recompilacion. Harness y build incremental UWP correctos;
primera corrida aprende prioridad, segunda valida seleccion/FPS. Gate manual y Series
pendientes; sin commit. Detalle en `docs/xbox_performance.md`.


Gate PC prioridad JIT (30 sep 2026): Q71s, retorno0, dos T completas. Nueva43FPS
con16414 compilaciones:69,75% presupuesto y28,80% perfil solo en otro core;
recorrida59,25FPS/559 compilaciones.16973 registros T guardados en v2 y checksum
verificado. Esta corrida aun cargo v1 sin prioridad; siguiente compara prewarm
priorizado,64MiB/core. Cinco asserts BufferQueue antes de T, Render sin errores.
Arranque tuvo pausa larga compatible con suspension host, causa sin confirmar.
Detalle/evidencia en `docs/xbox_performance.md`; Series pendiente, sin commit.


Gate PC prewarm priorizado (30 sep 2026):16973 prioritarios aceptados sin rechazos,
64MiB/core; Q67s/retorno0, dos T completas. Nueva43->51,5FPS, Compile16414->5672,
p99gap71,290->34,780ms y max200,573->50,043ms; recorrida59,25->58FPS con mas
compilacion y peor p99. Mejora parcial observada, no60 sostenidos ni estabilidad
general certificada.25157 registros prioritarios persistidos/checksum correcto;
Render sin errores, dos asserts BufferQueue antes de T. Cobertura por presupuesto
y perfil en otro core sigue pendiente; siguiente candidato prioridad entre cores
con validacion de codigo. Series pendiente, sin commit; detalle en rendimiento.


Direccion FPS ampliada por usuario: presupuesto configurable/adaptativo, aprendizaje
persistente sin truncar por residencia en RAM, velocidad de precarga (cache host
relocalizable/PPTC) e investigacion de reutilizacion por modulo/contenido entre juegos.
64MiB y262144 registros son limites distintos; operaciones ARM64 ya estan implementadas,
se aprende codigo concreto. Diseno y fuentes en xbox_performance.md; sin cambio de
codigo/presupuesto ni lanzamiento en esta revision, sin commit.


Decision usuario memoria: caches por juego, pruebas PC con limite5120MiB (Series
medido,5GiB). local-run.ps1 aplica por defecto Job process-commit cap, verifica
API y conserva limite tras cerrar launcher; MemoryLimitMiB0 opt-out. Frontend
memory_limit_mib refleja presupuesto de caches/diag sin falsear limite OS.
No aumenta prewarm64MiB/core ni cambia memoria guest: medir gameplay primero.
Probe asignacion real rechazado al limite y build UWP correctos; app PC limitada
lanzada, gate manual pendiente. GPU dedicada PC no reproduce RAM unificada Xbox.
Detalles/fuentes/trampa de permisos en xbox_performance.md; sin commit.


Gate PC5120MiB: Q84s/retorno0, T completas38FPS nueva/53 recorrida; maximo
muestreado4653MiB commit con466MiB margen, sin errores Render/asignacion, dos
asserts BufferQueue previos.25157 prioritarios precalentados,64MiB/core. Ventanas
muestran mas recreacion/decodificacion de texturas y uso GPU; posible presion GC
con politica5120, sin thrashing probado ni A/B determinista. Antes de ampliar JIT,
medir evictions/hits y proteger conjunto de trabajo de gameplay. Job PC no equivale
a RAM unificadaSeries. Detalle en xbox_performance.md, sin commit.


Candidato autorizado usuario: prewarm100MiB por core emulado (antes64), hasta
400MiB en cuatro instancias; con core3 sin perfil hasta300MiB. JIT capacidad512MiB
se mantiene, PC cap5120MiB/caches por juego. Build incremental pasa; prueba manual
T/Q, pico memoria, FPS y Series pendientes. Sin commit; detalle en rendimiento.


Gate PC100MiB/core con limite5120: Q64s/retorno0, T completas; nueva38->57,25FPS
yCompile6725->598, recorrida53->59,25FPS/637->251. Maximo muestreado4685MiB,
434MiB margen; Render/asignacion sin errores, cinco asserts BufferQueue antes deT.
Precarga40,4s frente27,9 con64MiB. Presupuesto ya solo45/1 misses; otro core533/249
domina restantes. Mejora parcial manual, no60 sostenidos ni Series certificados.
Mantener100, siguiente foco cobertura entre cores/precarga/GC; sin commit.


Usuario autoriza candidato150MiB de prewarm por core (antes100), PC cap5120MiB
y perfiles por juego. Build incremental correcto; gate manual T/Q y Series
pendientes. Sin commit; evidencia/diseno en xbox_performance.md.


Gate150 PC: Q67s/retorno0, T completas56/57,5FPS frente57,25/59,25 con100.
Techo+50% pero codigo real+5,97% (317,37MiB), bloques+6,11%; p99 empeora22,38/25,75%.
Maximo commit muestreado4814MiB/margen305, precarga43,8s. Perfil completo sin
omitidos por presupuesto, misses otro core/nunca aprendido.100 mejor balance
observado, A/B causal/Series no certificados. Codigo sigue150 autorizado, sin
reversion automatica ni commit; detalle en xbox_performance.md.


Candidato autorizado115MiB/core: comparte perfiles prioritarios entre cores del
mismo juego/BuildId, valida codigo y emite por JIT; perfiles contradictorios no se
comparten. Precarga paralela con un worker por JIT detenido y progreso solo en
coordinador, join antes de Run/fallback. PC cap5120MiB conservado. Harness Dynarmic
real pasa concurrencia/estado/hash y unwind, build incremental correcto. Gate
manual tiempo/RAM/T/Q ySeries pendiente; sin commit, detalle en rendimiento.


Arranque115 compartido/paralelo PC confirmado:4344 bloques de otros cores
aceptados,777750 preparados total, cero rechazados/omitidos, codigo321,14MiB
total(113,64/105,58/101,92). Precarga completa21,9s frente43,8 serial observados
con perfiles distintos; workers21,39s wall. Commit2251MiB tras carga, cap5120
verificado. Gameplay/T/Q, margen/FPS ySeries pendientes, sin commit.


Gate115 compartido/paralelo PC: Q58s/retorno0, T completas56,25/57FPS;
Compile334/20 frente797/140 con150(-58,09%/-85,71%), pero FPS sin mejora general.
Precarga21,9s vs43,8;4344 compartidos aceptados y cero omitidos/rechazados.
Commit maximo muestreado4811MiB/margen308; Render/asignacion sin errores,
tres asserts BufferQueue previos aT. T1miss181otro-core/153nuevos, T2 10/10;
solo compartimos prioritarios. CPU ejecucion/sync/pacing/caches siguiente diagnostico,
no mas presupuesto ni60FPS certificados. Error playtime al cierre registrado,
retorno0; Series pendiente, sin commit. Evidencia en xbox_performance.md.


Commit7eeb775e7 guarda perfilesJIT/prewarm115 compartido/paralelo y diagnosticos
previos. Siguiente candidatoT: RunThread>=200us porguest/core, dispatchscheduler,
GCpresion/evictions/creacion y cargastextura>=200us. SoloT, misma politicaLRU/JIT.
Analizador de intervalos largos recorta/une solapes; no confundir elapsed con CPUbusy
ni correlacion con causalidad. Fixtures y compatibilidad logsprevios correctos;
build/gateTcapacidad/manual/Series pendientes; detalle en xbox_performance.md.

Gate build del diagnosticoT: incremental UWP pasa (incluye objetos D3D12/Vulkan
por header comun). Analizador pasa fixtures y dosTprevias. Prueba manual lanzada
con115/core,play1,fastmem0,cpu_profile1,jit_prewarm1; Job5120 verificadoPID3436.
PendienteT completas/capacidad, overhead y lectura del cuello restante; Series
pendiente. Instrumentacion posterior al commit7eeb775e7 queda sin commit.


Usuario ampliaT a8s: frontend arma480vsyncs. Nuevos eventos llenaron131072 entradas
 a3,337/3,094s (~39-42k/s); ambas T truncadas, no comparacion FPS. Evidencia archivada
pc-frame-stalls-truncated{,-diag}.txt. Proceso ausente, diag sinQ/retorno confirmado.
Capacidad pasa524288 (~16MiB,+12MiB respecto131072), almacenamiento fijo y sin
alloc duranteT; mantiene dump fuera de medicion. HUD calcula tiempo porvsync restante
sin constantes4s. Analizador usa480 por defecto, --vsyncs240 para logsanteriores;
FPS solo cuando ventana completa, rechaza interpretar truncada como8s completa.
Incremental/manual por registrar; cambios posteriores7eeb775e7 sin commit.

Gate8s: build incremental UWP correcto(5operaciones); analizador pasa captura480
completa, compatibilidad240, unionrecortada y rechaza FPS de ambasTtruncadas reales.
Relanzado manual conplay1/115MiBcore/fastmem0/prewarm1, Job5120 verificadoPID18888.
Capturas480/HUD/capacidad ySeries pendientes; cierre usuarioQ, sin timeout gameplay.


GateT8PC: Q66s/retorno0,480vsyncs completas303166/331495 eventos. FPS52,625/58,375,
p99gap41,476/32,684,max144,634/43,036ms. GC315/311evictions,206/235recreaciones
mismaaddr; proxy3004--3238MiB siempre sobrecritical2758. Gaps41/34ms coinciden
GC30/20ms; peor145ms incluyeRun105,5ms guest125/core0 yGPUidle119ms (causaCPU
no separada). JIT4433/1501Compile,418,6/107ms; no missesbudget. Commitmax4873,
margen246; RenderError/Critical0,8unmappedDeviceReadBlock antesT. Dump provoca
pausa3,79/4,14s fueraT: corregir guardado antesde evaluar estabilidadvisual;
GC/presion yRun siguiente foco. Sin60sostenidos/Series, sin nuevo commit.


CandidatoGC autorizado: guardado debug fueraalcance. PoolD3D12 trimheapstotalmente
vacios trasfence, conserva1warm/clase normal yliberatodosbajopresion, tombstones
indicesestables. TextureGC usaheadroomapp/DXGIactualconhisteresis, age120normal/
60critico/10emergencia, trabajoincremental1ms salvoemergency; downloadindividual
puedeexceder1ms. BufferGC/Vulkanfallbackoriginal. T8desglosaRunlargoCompile yCPU
flushthreadlocal; noCPUbusy. TestsMSVCpolicy/harnessDynarmicrealTLS/parser pasan,
buildUWPcorrecto; manualchurn/RAM/FPS/Seriespendientes, sincommit. Detallefuentes
encuaderno rendimiento; 115/core y5120proceso mantenidos.

PruebaPC candidataGC/Runlargo lanzadaPID19476, Job5120MiB verificado,play1,
fastmem0,cpu_profile1,jit_prewarm1,115/core,T480. Sinlimitegameplay; usuarioQ.
Arranque/precargaenprogreso, validarTchurn/maxGC/trimreal/headroomyCPUdetalle;
no concluirmejoraFPSporbuild. Sincommit.

Nota arranque: LruCache.ForEachItemBelow incluye tickigual al corte; clamp0 admitia
imagestick0 antesdeedad minima. Fuente ahora omitepasada si frame_tick<age (sin
unsignedwrap ni expulsarfirstframe). TrialPID19476 ya lanzado con clamp0 previo;
susTgameplay despues120frames ejercenmisma politica steady-state. Correccion de
arranque pendiente siguiente link/lanzamiento, no certificar gateearlyaging con
ese proceso. No interrumpir corrida manual para substituirbinario.


GatecandidatoGC: Q94s/retorno0,T8completas53,875/50,25FPS. No mejora validada:
evictions395/429,recreaciones258/250; levelEmergency todaT,margen120,66--162,37/
134,55--146,40MiB. Trim0,pool512MiB peak416(fragmentacion/ocupacionporinvestigar).
Run34,431ms contieneFlush34,402 ycoincideGC35,796; Run33,929 contieneCompile33,223:
CPUflush yJIT son ambosfocos. Parserempatestimestamp corregido ordenestable yfixture
pasa. RenderError0,4BQassertantesT. Guardagingarranque buildincremental pasa trasQ;
no relanzado,sincommit. Guardadodebug fueraalcance; siguienteGCpacking/descargas
/margenreal,115/core y5120 intactos,Series y60sostenidos pendientes.

Investigación de texturas y contraste Vulkan: documentada en
[`xbox_texture_memory.md`](xbox_texture_memory.md). Vulkan usa VMA y presupuesto
real, pero comparte GC síncrono GPU-dirty; sus flushes async no cubren esa ruta.
No activa desfragmentación VMA. Distinguir fragmentación, retirada pendiente y
presión de commit app frente a VRAM PC. Prioridad: readback GC diferido con
validación de versiones, packing/estadísticas por heap y conjunto de trabajo.
Fuentes Microsoft/GPUOpen, discusión MJP y experiencias de fragmentación,
Dolphin/Xenia revisados; investigación no constituye mejora validada.

Usuario autoriza experimento ring upload 256 MiB (antes 128). Cutoff por upload
32 MiB conservado para aislar capacidad, 16 regiones ahora de 16 MiB. JIT115/core,
Job5120 y T8 se mantienen. Build incremental correcto; margen real, fallbacks,
esperas, p99/FPS y Series pendientes. No cambio de packing/readback aún, sin commit.

Gate staging256 PC: Q74s/retorno0, dos T480 completas,58/59FPS frente53,875/50,25;
p99 33,615/29,294ms frente46,860/35,976. Compile1261/258 frente3171/5447:
aprendizaje/escenas impiden atribuir mejora a staging. Cero ringwaits en ventanas;
margen T mínimo71,945/107,301MiB, diag pico5009MiB, RenderError0. Pool384MiB,
trim2heaps/128MiB confirmado; no packing nuevo. GC máximo18,161/23,544ms,
T2 peor gap40,081 coincideGC23,544/22evictions. Cinco BQasserts antesT; datos y
límites en xbox_performance.md. Mantener256 pedido,115/core y5120; readbackGC/
packing siguiente,60sostenidos/Series no certificados, sin commit.

CandidatoGC diferido ybest-fit implementado; detalle/coherencia/fuentes en
xbox_texture_memory.md. Pinnedreadback8MiB, versiónGPU/modificación/CPUvalidada,
esperaFence diferida confallbacksíncrono siappfree<64MiB/copiamayor/noapta.
Poolbase64MiB conservado, mejorhuecocompatible, tamañoheapgrandealineado sin
potenciadedos. NuevosTstats distinguen reservas/libres/fencepending/GCpinned.
Harness1.248.000 casos pasa; buildincremental pasa. GateGPU realAppContainerdebug
pasa moves, bytes, escriturasGPU/CPU obsoletas, cap8MiB/descarte/emergency;
queued5/ready2/stale2/sync1,peak8MiB/pending0,RenderError0,retorno0.
No mejoraFPS certificada; gameplaymanual ySeries pendientes, sin commit.

GateGCdiferido/bestfit: Q67s/retorno0,T480 completas58,25/57FPS frente58/59,
sin mejoraFPSgeneral. GCmax18,161/23,544->9,312/6,812ms; Tqueued/ready11/11 y6/6,
stale/sync0. Countersboot5/2/2/1 incluidosentotal452/449/2/1; gameplayporresta
447queued/447ready,stale/sync0,pending0shutdown. PicoGCpinnedT3,75/2,754MiB.
Heap384MiB,libre118–170/mayorhueco50,875,trim1heap64; packingventajacausalno
demostrada. Margenmin98,617/126,891,RenderError0,8BQasserts antesT.
Peoresgaps dominanGPUidle27–30ms yalgúnupload/Run sinCompile/Flush; siguiente
desglosarguest/esperas/uploads.115/core,256staging,5120 intactos,sin commit;
60sostenidos/Series pendientes,evidencia en rendimiento.

DiagnósticoCPU siguiente implementado: T añade PCfinal/HaltReason/SVC porRunlargo,
callbacksclock/icache elapsed ycontadoresmemory; SVCSleep/locks/condition/address
amplíantrazaIPC. CPUhostGetThreadTimes enventanas>=100ms, no deltaRun: harnessPC
observa~15,625ms cuantización, query551ns; UWP soportado segúnMicrosoft. No API
QueryThreadCycleTime(desktoponly/ciclos no tiempo). ConsultasOS soloenT, check
ventana cada16Runs. API unavailable explícita; PCendpoint nohotspot, callbacks
anidados noCPUbusy. Fixturesmigración/SVC/PC/windows/oldlog pasan; detallesfuentes
enxbox_performance.md. Staging256/JIT115/Job5120/T8 intactos; gateTmanual/Series
pendientes,sin commit.


Corrección capacidad T CPU/SVC (30 sep 2026): Q131s/retorno0; primera T llena
524288 a5,382s/323vsyncs, segunda llena a~5,2s y volcado solo261vsyncs por
límite100MiB del logger (archivo105830342bytes). No comparar FPS de estasT.
Evidencia pc-cpu-run-detail-truncated{,-diag}.txt. Sleep/locks/address dominan
nuevos eventos (126492 comienzos enT1). Se conservan edgesIPC/WaitSynchronization;
SleepThread/ArbitrateLock/WaitProcessWideKeyAtomic/WaitForAddress se emiten como
un único guest-svc-long al terminar>=200us, mismo captureID yguestoriginal aunque
migre dehost. FueraT/sinSVCseleccionado no reloj ni eventos. Spans menores200us o
cruzando bordes omitidos explícitamente; solape no prueba causalidad. Memoriafija
524288(~16MiB),T480,staging256,JIT115/core,Job5120 intactos. Replay aproximadoT1:
282195eventos frente524288,proyección8s419496 (no garantía de carga futura).
Parser fixtures migratedIPC/longlock/border pasan; incrementalUWP22operaciones y
gitdiffcheck pasan. Gate manual completitud deambasT ySeries pendientes,sincommit.


Gate PC T CPU compacta (30 sep 2026): cierreQ72s/retorno0, proceso ausente.
Capturas480vsyncs con ambos end presentes,415931/420795eventos (<524288).
Evidencia pc-cpu-svc-compact{,-diag}.txt y analysis.jsonl. FPS58,625/56,375,
p99gap33,377/33,540ms,max40,213/36,969,gaps>=25ms16/29. Frente al gate
GCpacking58,25/57 no mejora general demostrada; diagnostico no optimizacion y
escenas/perfiles distintos. Compile124calls/10,954ms y322/34,245ms: no dominante
en ventanas agregadas, aunque Run7,949ms incluye4,966ms Compile enT2.
API GetThreadTimes disponible0unavailable; ventanas100ms OSuser+kernel completas
235/236. Utilizacion media hiloshost core0/1/2:46,08/41,22/45,53% T1 y
50,76/48,39/51,93% T2. Incluye trabajo entreRuns, cuantizacion y sobrecosteT;
no es porcentaje guest puro ni excluye cuello en hilo crítico/preemption/locks.
Ready->dispatchp99subset0,140/0,145ms; no captura todos los episodios Runnable.

Señales restantes: guest123/core1 terminaBreakLoop enPC0x81239648; Run11,004ms
T1 y17,797ms T2 conCompile/Flush/Clock/Icache0. Coinciden con dos refreshcontents
texturas (10,800/16,731ms solape) en OTRO hilo host15064, no tiempos anidados de
ese Run. Guest124/core2 mismoPC12,317ms también coincideconuploads. Endpoint
no prueba hotloop ni que upload causeespera; PC/cadena de señales por investigar.
PeorgapT2 36,969ms incluye17,441ms refresh/upload, GC0,236. GapT1 36,761ms
incluye19,330ms uploads. Prioridad1 separar refresh por staging/read/deswizzle/
convert/repack/backend y tamaño/formato para escoger optimizacion medida.
Otras gaps37,214/34,715ms tienenuploads0/GC0 yel hiloGPU esperando trabajo
28,281/22,981ms (no medidor de ocupacionGPU). Guest83 SendSyncRequest espera
35,839/31,612ms enesas gaps; tiene2344/2254 llamadas y7155/7249ms elapsedT.
Prioridad2 identificar servicio/comando IPC ydependencia/wakeup que entrega
trabajo al hilo gráfico; waits coincidentes de background como guest118 condvar
1s no atribuirlos a tirones. Muchos WaitForAddress workers123--127 (~5--6sT)
puedenser espera legítima; no recortar waits ni alterar guest scheduling sincausa.
Prioridad3 CPUflush puntual: Run10,514ms guest124 contiene10,494ms Flush T1,
aunque peoresRuns restantes carecenCompile/Flush. Desglosar lock/check/download
si se confirma recurrente, no llamar restoCPUbusy.

GCmax10,004/7,635ms,sum35,127/44,948,readbackqueued/ready9/9 y7/7,
sync/stale0 dentroT, pinned3,750/2,754MiB. Evictions435/437,creates481/409:
churn persiste. Headroommínimo84,156/119,133MiB. RenderError/Critical0;
5BufferQueueasserts65,14--65,16s antesT77,06s. Erroresplaytime/abandoned al
cierre ya conocidos. Mantener staging256/JIT115/core/Job5120/T8; no60sostenidos
niSeriescertificados. Sincommit ni relanzamiento durante revisión.


Candidato diagnóstico upload/IPC (30 sep2026): fases T>=200us separan staging,
read/mapguest, CPUdeswizzle, convert, backend yCPUrepackD3D12; bytesguest/formato
acompañan refresh. ReadUnsafe map no equivale a copia: fase puede ser solo acceso
al span; deswizzle incluye cargas de ese span. AcceleratedUpload separaReadBlock
ybackend. Repack está contenido enbackend, no sumar tiempos anidados; backend
mide grabación host, no ejecuciónGPU. Ruta genérica instrumentada paraVulkan y
D3D12, sin modificar selección/corrección/contenidos ni política de cachés.
ServiceFramework HandleSyncRequest marca antesdelmutex el guestoriginal,
command/type y24bytes delnombre (longitud original ytruncamiento explícitos).
Nombre payload fijo sinalloc/format/logdirecto, parsereensambla porguest; spans
mutex/handler>=200us. Handlerincluye respuesta/setupdeferred, no completa por sí
soloelSVC; parserasocia despachos dentrodeSVC33/34 porguest aunque cambiehost.
Resumenporguest/service/command solosiun despacho; variossonposiblesretries.
Requests iniciados/finalizados fueradeT noatribuir completos. IPCidentity fueraT
no leectx/nombres. FueraT timerssinlectura de reloj, memoriacapturaigual16MiB.

Para conservarcapacidad seomiten RunCompile/Flush/Clock/Icache redondeados0us;
evento finalRunMemory certifica timingsfaltantes0 enparser, antesdelmarkerfinal
permanecenunknown. ReplaymanualTprevias omite37805/40702entradas; IPC6987/6838
requests, incluso6entradas/request proyectan420048/421121 (sincontarfasesupload
nuevas, nopromesa paraotroescenario). No cambiarcap/job/staging/prewarm/T8.
Parser fixtures migratedSvc+namechunks+mutex/handler, uploadfases anidadas,
Runcompacto/bordesunknown yservicelongtruncatedpasan; logs480previoscompatibles.
Buildincremental23ops +final4ops correctos, gitdiffcheck limpio. PendienteTmanual
completitud/overhead/identidad83/faseuploaddominante ySeries. Sincommit.


Gate PC fases upload/IPC (30sep2026): Q76s/retorno0, procesoausente. AmbasT480
completas397947/409314eventos yendlogpresentes. Evidencia pc-upload-ipc-phases
{,-diag}.txt/analysis.jsonl. FPS55,25/58,375,p99gap33,789/32,102ms,max41,439/
61,397,gaps>=25ms44/15. FrenteCPUcompacta58,625/56,375 variacionescenas/perfiles
no admiteatribuirmejora/regresion ni certificaroverheadinstrumentacion. JIT300/
148Compile32,649/12,450ms. CPUhostcore0/1/2media51,48/50,33/51,11% y52,33/
47,05/51,74% OSuser+kernel, no guestbusy ni saturacionindividualdemostrada.

Prioridad texturas ya concretada: enums28/29 son BC5_UNORM/BC5_SNORM,
noASTC. Arrays786432bytesguest reaparecen enambasT; topT1 uploads5,787/5,890ms,
convert3,533/3,600 ybackend2,088/2,129; T2 parejamax5,412/8,420ms,
convert3,261/3,342 backend2,003/4,929. CoincidenRun123/124 terminandoBreakLoop
comoantes, peroOTROhost: no sumarni tratarcomotiminganidado delRun. Conversion
13,156/21,633ms en4/7spans>=200us; backend22,965/29,182ms en18/15spans;
repack8,493/8,169ms en11/9spans contenidosenbackend,max1,683/1,991ms.
Unswizzle10,951/1,492ms capturados;read3,484/2,826; staging0spans>=200us,
no afirmarcostecero. Siguienteoptimizable BC4/BC5decodeCPU yuploadplaintexels,
mantenerfallbackBCarraysqueevitacorrupcionSeries; rutaCPUConvertImage->
DecompressBCn porbloques. Alternativas a investigar SIMD/batchedCPU ocomputeGPU
reutilizandopatronASTC, con bytes/capas/SNORM validados. Topuploadsnoexplican
por sísolos todosgaps. No cambiarRAM ni desactivarfallbackvisual paraFPS.

IPCguest83 resuelto a IHOSBinderDriver comando3/type6 TransactParcelAuto,
llamaTransactParcel contransaction_id queaunnoestácapturado. Ningúnnombretruncado;
6794/6986dispatches; servicioMutexp0spans
>=200us. Guest83binder883/933requests,sum6977,481/7017,055ms deSVCwait,
max32,418/36,318ms. Nvdrv comandos1/11 cortos<=0,175ms. Tophandler32,332ms
contiene32,319msunionDequeueWait; T2handler36,197 contiene36,185msdequeue
(múltiples esperaspuedenunirse). Estoidentifica esperaBufferQueue porframebuffer
libre, no30msdecomputo ni bloqueoMutexdelservicio. Normalbackpressure existe;
no atribuircausa aesaespera por sísola. Siguiente cadena verificarAcquire/Release,
VSync/composicion/fence ycantidadbuffers/readyframe; transactionBinder para
vincularDequeue/Queue directamente si hacefalta. No reducir sleeps/lockswaits ni
relajarsincronizacion guest/GPU paraforzarFPS.
PeorgapT2 61,397ms solo0,718upload/GC0/11,738GPUthreadidle,guest83WaitForAddress
52,107ms, no binderwaitlargo eneseintervalo. Distinto delgap37,417ms con
BinderSVC36,318/dequeue36,185/GPUthreadidle30,151 yGC0,284. Dosrutasporinvestigar,
no presentarBindercomocausaunica. RenderCPUflush puntual14,757msT1/12,519T2;
Run79enT1 solapa14,656ms enotrocore,noCPUbusy exclusivo demostrado.

Margenmin54,855/89,590MiB; GCsyncfallback1T1 alcruzar64MiBprotegecoherencia;
queuedready5/5,6/6,stale0. GCmax5,570/4,647ms. RenderError/Critical0,
BQassert0;unmappedDeviceReadBlockantesT(~60,64s),abandoned/playtimealQconocidos.
Mantener256staging/115core/5120cap/T8. Series/60sostenidos/optimizaciónpendientes,
revisión sin nuevo commit ni lanzamiento.


## Candidato BC4/BC5: bloques completos y escritura por filas (30 sep 2026)

La captura identifica BC5 UNORM/SNORM convertido por CPU. Se conserva la decisión
D3D12 `decode_bc_arrays`: la textura final continúa en R8/RG8 para evitar la
corrupción de arrays observada en Series. Vulkan mantiene BCn nativo si
`IsOptimalBcnSupported`; cuando no, usa los mismos formatos R8/RG8 y el decoder
genérico. El cambio acelera ese decoder compartido, no elimina el workaround.

Fuentes consultadas:
- Microsoft describe bloques4x4, dos canales BC5 y padding de mipmaps:
  https://learn.microsoft.com/en-us/windows/uwp/graphics-concepts/block-compression
- Referencia SwiftShader de la que deriva `externals/bc_decoder`:
  https://github.com/google/swiftshader/blob/d070309f7d154d6764cbd514b1a5c8bfcef61d06/src/Device/BC_Decoder.cpp
- bcdec documenta soporte signed/unsigned y prioriza tamaño, no velocidad;
  no se incorpora otra dependencia ni se copia su código:
  https://github.com/iOrange/bcdec
- Modelo local `renderer_vulkan/maxwell_to_vk.cpp`: BCn nativo o conversión a R8/RG8.

`bc45_decode.h` prepara la paleta una vez por canal/bloque, especializa signed y
canales por plantilla, interleave RG antes de escribir y almacena una fila completa
con memcpy de4/8bytes. No asigna RAM ni requiere instrucciones específicas de CPU;
memcpy permite buffers desalineados y evita aliasing. Conserva byte por byte la
interpolación entera y semántica SNORM del decoder existente, incluidos endpoints
-128; no presenta esta compatibilidad como nueva certificación de precisión hardware.
Solo aplica si ancho/alto por capa son múltiplos4 y source row texels >=width y
múltiplo4. El resto de mipmaps conserva el decoder anterior. Capas y slices usan
los mismos strides que el camino previo. No cambia transferencias/fences/eviction,
ni cantidad/formatos de recursos; backend/repack siguen pendientes de ahorro propio.

Gate MSVC desktop /O2:262144casos (65536endpointpairs xBC4/5 xUNORM/SNORM), cada
uno con los8índices, y108 imágenes full-block con pitches extra, capas aplanadas,
punteros input/output desalineados y guard bytes. Bytes idénticos al decoder
`bcn::DecodeBc4/5`; asserts activos. Prueba aislada BC5 128x128x48, mediana9:
UNORM4,281->1,7415ms (2,458x), SNORM4,3052->1,7997ms (2,392x).
No es medición FPS ni Series ni mismo trabajo/cache/clock de lasT reales.
Incremental UWP4operaciones (decoderobj,lib,exe) pasa ygitdiffcheck limpio.

Trampa de harness: build-env usa StoreCRT; link standalone necesita windowsapp.lib
pero ejecutarlo fueraAppContainer devolvió0xc0000135 (runtime UWP ausente).
Se compila el test standalone con vcvarsall x64 desktop; el binario real de Eden
se sigue compilando con build-env/UWP. No sustituir runtime del emulador para tests.
Scripts/evidencia del harness quedan en build-uwp/log-review-2026-09-30 ignorado,
fuente reproducible tools/xbox/tests/bc45-decode.cpp enrepo. Gate gameplay2T/Q,
mejoraFPS/memoria/visual ySeries pendientes. 256staging/115MiBcore/5120cap/T8
conservados. Sincommit.


## Gate PC del decoder BC4/BC5 por filas (30 sep 2026)

Prueba manual cerrada con Q tras75s de gameplay, retorno0 y proceso ausente.
Evidencia: `build-uwp/log-review-2026-09-30/pc-bc45-packed{,-diag}.txt` y
`pc-bc45-packed-analysis.jsonl`. Ambas capturas contienen480vsyncs y marcador
end:399390/414750eventos. No truncamiento ni aumento de capacidad/RAM.

Comparación manual con `pc-upload-ipc-phases` (no A/B determinista):
- FPS55,25->58,875 y58,375->59,25 (+6,56%/+1,50%).
- p99gap33,789->30,883ms y32,102->25,898ms (-8,60%/-19,32%).
- gaps>=25ms44->13 y15->8. Maxgap41,439->41,840ms (sin mejoraT1),
  61,397->33,014msT2. No60sostenidos ni eliminación de todos los tirones.

BC5 UNORM/SNORM con el mismo guest size786432bytes: antesUNORM T1
3,030/3,533ms,T2 3,110/3,141/3,261; candidatoT1 1,597ms,T2 1,475/2,220.
SNORM antesT1 2,993/3,600,T2 3,182/3,339/3,342; candidatoT1 1,646,
T2 1,543/2,415. Menor coste por conversión observado y coherente con benchmark
aislado; pocas muestras/elapsed incluye preemption y condiciones no idénticas.
TotalConvertT1 3,243ms/2spans vs13,156/4; T2 7,653/4 vs21,633/7: totales
no comparables directamente por distinta cantidad de cargas. El backend permanece
~2,0--2,5ms para estas BC5 (no se optimizó); repackT8,193/8,962ms anidado enbackend.

JIT22/30Compile con2,357/3,087ms frente300/148 y32,649/12,450ms antes.
Evictions331/342 ycreates379/341 vs435/437 y481/409. Trabajo/perfiles/escenas
cambian: no atribuir todo el aumento FPS ni RAM al decoder. Mantener el candidato:
bytes se validaron exhaustivamente, coste de conversión baja sin memoria extra;
visual de gameplay y Series pendientes de confirmación específica.

Readbacksqueued/ready4/4 y6/6, sync/stale0 enT. Headroommin90,039/116,656MiB
frente54,855/89,590; máximo commitdiag5009MiB, conDRAMguest1704MiB alQ frente
1733 antes (confunde comparación memoria). GCmax4,598/6,368ms,total30,143/45,628.
RenderError/Critical0 yBufferQueueassert0. UnmappedDeviceReadBlock antesT(~66,65s)
yabandoned/playtime alQ ya conocidos, no errores de renderer/asignación.

Cuello siguiente: gapT1 38,023ms conupload0/GC0,hiloGPUesperando28,601ms y
Binderguest83SVC36,841; T2gap31,526 conupload0/GC0,idle24,543 yBinder30,196.
PeorT1 41,840 solo1,328upload/0,039GC,idle31,339. No decoder costoso suficiente
para explicar estos casos. Investigar BufferQueue/VSync/composición/Acquire/Release
ymomento de frame listo antesde cambiar pacing/sincronización. Backend BC5 todavía
medible, pero cadena de presentación es siguiente prioridad para los gaps restantes.
CPUflushT1 puntual9,971ms, no desaparece. No recortar sleeps/esperas a ciegas.

Staging256MiB,prewarm115MiB/core,Job5120MiB,T8 intactos. Sin nuevo commit ni
relanzamiento durante revisión. Series/60FPSsostenidos/A/B pendientes.


## BufferQueue/VSync después del commit b7aa63d14 (30 sep2026)

Commit solicitado por usuario b7aa63d14 guarda GC diferido/bestfit, staging256,
T8/CPU/IPC/upload ydecoderBC45 validado. No push. Siguiente trabajo separado,
no incluido enesecommit.

Lectura dirigida de los logs existentes cambia la hipótesis: QueueBuffer registra
requested/applied low/high32. Gate antesBC45: T1 408interval1+34interval2,
T2 460+7; despuésBC45 T1 466+5,T2 471+3. Esto es el guest solicitando presentar
cada dos vsyncs, no un incrementoinventado enelcompositor. Todavía no prueba por
qué elguest pide2 ni convierteFPS de presentación en simulación60Hz.

Ejemplo T1 postBC45: buffer0 se encola3894,567ms con2->2, adquiere3910,245,
VSync3926,782 no lo libera, libera3943,876 después de2vsyncs; WaitForComposite
0us en ambos ticks. Productor espera ycontinúa3943,913(37us despuésrelease).
Su siguienteQueueBuffer3945,120 cierraelgap38,023ms. T2: adquiere intervalo2
909,376, tick925,773 retiene, libera942,678, dequeuefin942,721(43us), siguiente
queue944,082. Vsync no faltó enesos ejemplos: retención correspondeal contrato2.
No cambiar intervalo2 ni soltar guestbuffers antesde sus fences por aparentar60;
el ensayo force_swap_interval1 anterior no arregló simulación (cabecera documento).

Investigación fuentes:
- AOSP define acquire/release fences y cuándo el productor puede reutilizar buffers:
  https://source.android.com/docs/core/graphics/sync
- Microsoft explica sincronización/flight limiting de la swap chain D3D12:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains
- UWP describe el objeto waitable de frame latency:
  https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
No introducir flags/SetMaximumFrameLatency/present0 a ciegas sin probar cuello y
capacidad Series. Cadena HardwareComposer/Conductor/GPU RequestComposite es común
conVulkan; D3D12 terminaenDXGI Present(1,0), medible por separado. No atribuir
hold2 a backend D3D12 sin evidencias.

Candidato diagnóstico dirigido, preserva toda sincronización:
- CoreTiming callback marca vsync-tick lateness antes Set; GetNextTicks se mantiene
  despuésSet comoantes, sin cambiarorden temporal delcallback.
- HardwareComposer registra framelease porconsumerID+guestframenumber: adquisición
  con intervalo normalizado, hold conperiodosrestantes, decisiónrelease con edad
  en ticks. Overlaysnormalizados1; no truncarframe number. LeaseRelease es decisión
  previa aReleaseBuffer, no marcaGPU completado ni garantiza statusrelease.
- Spans>=200us para mutexContainerCompose,ComposeLocked y DXGI Present.
  Presentelapsed incluye bloqueohost; noGPUbusy. Nada de cambios de buffers/fences.
- Analizador cuenta requested/applied, leaseintervals/retencionticks/residency y
  extraheldticks, callbacklate/maxgap, solapePresent en gap. Wallresidency no es
  declaración deVSync perdido: velocidad guest/host yborde deT importan.
- Spans adquiridos/released fueraT no se emparejan; consumidor incluidoevita colisión
  deguestframenumbers entrecapas. Holdevents no equivalen espera CPU.

Fixtures dehold2legal, mismo frame en2consumers, retenciónextra1tick, Present/tick
pasan; dos480anteriores compatibles, fields nuevos ausentes no inventan muestras.
IncrementalUWP26ops yfinal26ops (header común) pasan; gitdiffchecklimpio. Gate
manual T8/Q/capacidad/interval1delay/Presents/Series pendientes. SinRAMextra,
almacenamiento524288 fijo; eventosnuevos~unosmilesT, no perdraw/perGuestRunextra.
Staging256/prewarm115core/Job5120 mantenidos. Este cambio mide el siguiente cuello,
no se presenta como optimizaciónFPS certificada. Sincommitnuevo.


## Gate PC BufferQueue/VSync y frame lease (30 sep 2026)

Evidencia: `build-uwp/log-review-2026-09-30/pc-frame-lease-chain{,-diag}.txt`
y `pc-frame-lease-chain-analysis.jsonl`. Q tras89s de gameplay, retorno0.
T480 completas394720/395684 eventos, FPS59,125/59,375; p99gap28,684/28,590ms,
max39,181/33,702ms. Frente58,875/59,25 previo, la diferencia pequeña no demuestra
mejora causal: esta versión añade diagnóstico, no cambia sincronización.

- Los480 callbacks CoreTiming aparecen en ambasT; lateness máximo0,509/0,539ms,
  separación máxima17,074/17,135ms. No retraso largo del tick en estas muestras.
- Leases completas472/474, una incompleta por bordeT cada captura; cero leases
  retenidas más ticks que su intervalo. Intervalo1 máximo17,075/17,145ms;
  intervalo2 aproximadamente33,3--33,5ms. Requested/applied1->1:470/472,
  2->2:3/3. No se justifica liberar temprano o forzarinterval1.
- Present host>=200us:5/7 muestras, máximo0,224/0,252ms; cero spans de mutex
  Compose o ComposeLocked>=200us. No explican los gaps largos observados.
- T1 gap39,181ms (4382,965--4422,146): release4398,144 y DequeueWaitEnd4398,185
  (~41us desde ReleaseBuffer, ~37us desde adquisición siguiente); QueueBuffer
  siguiente4422,146, unos23,961ms tras finDequeue. Guest83 WaitForAddress(SVC52)
  termina4421,756 tras23,359ms, por tanto comienza4398,397. Solapa casi todo el
  tramo posterior aDequeue; GPUthreadwait23,153ms termina4421,944. Esto apunta
  a disponibilidad/sincronización del productor guest, no alease retenida.
  El evento SVC no identifica todavía dirección ni quién despierta: correlación,
  no prueba de origen ni CPUbusy. Uploads largos previos suman1,152ms en el gap.
- T1 gap34,968ms: finDequeue4315,020, Queue4335,198 (+20,178ms), WaitForAddress
  guest83 de19,534ms termina4334,745. Uploads previos7,376ms; la espera posterior
  requiere seguir la cadena de wakeups, sin sumar fases solapadas.
- T2 max33,702ms coincide conlease3659 intervalo2 adquirida211,268 y liberada
 244,766 (33,498ms/2ticks); Binder guest83 espera32,345ms. La política solicitada
 explica esta retención; no es un bug demostrado del compositor.

RenderError0; ocho asserts BufferQueue slot2 fuera del límite2 entre93,265--93,293s,
antes deT1(108,944s), permanecen pendientes y no explican por sí solos estos gaps.
Headroom mínimo83,426/111,543MiB; readbacks4/4 y8/8 queued/ready, sinfallback/stale
T. Mantener staging256MiB/prewarm115MiB porcore/Job5120MiB/T8. Series pendiente,
no60 sostenidos certificados. Siguiente foco: WaitForAddress del productor guest83,
dirección/valor esperado y señalizador, junto al tramo Dequeue terminado->QueueBuffer;
conservar semántica de fences y swapinterval. Diagnóstico posterior a b7aa63d14 sincommit.


## Traza concentrada en WaitForAddress, limpieza de diagnóstico (1 oct 2026)

Usuario autoriza desactivar diagnóstico ya innecesario. Política central constexpr
FrameTrace::Enabled/TargetGuest83: no registrar scheduler/Run/SVC/IPC de otros guests;
ScopedSpan desactivado no lee reloj. Scheduler descarta otros guests antes deMark.
Se desactivan timestamps CPU host, contadores Run/callbacks, detalle de uploads por
fase/formato, churn/packing/readbacks por evento, GPU submit, ticklateness, señales
VSync duplicadas, mutex/Compose/Present. Se conservan HUD FPS/CPU/GPUQ, estado T8,
Queue/Acquire/Release/Dequeue, framelease/intervalos, fences, GPU idle/composite,
Run PC/stop guest83, IPC guest83, elapsed largo upload/GC y headroom. La información
que se elimina deja de estar disponible; ausencia no implica coste0.
Autotests históricos buffer/texture del arranque quedan tras constexprfalse, código
retenido para gates de desarrollo. Errores/fallbacks/asserts siguen activos.
Nueva corrida sin cpu_profile=1 (defaultfalse): CPU callback/JIT timing detallado
apagado; prewarm115MiB/core sigue funcional, no confundir aprender perfiles con
contadores de diagnóstico. gpu_profile/audio_profile/debug_layer/GBV ya opt-infalse.

KAddressArbiter registra solo target83 y duranteT: enqueue real tras validar condición
(usando user_value ya leído, sin lectura guest adicional), address/value observed/
expected/type, timeout absoluto en ticks; signal identifica sourceguest ytarget
justo antes EndWait en las tres rutas. CancelWait identifica Result de timeout/cancel.
Al volver del wait se registra Result solo en la misma captureID. Señal no equivale
CPU inmediata: parser diferencia wake->dispatch y wake->resume, conservando guest
ID aunque cambie host/core. No se cambia el árbitro, condición, orden de EndWait,
colas, exclusivas ni timer. No se rastrean escrituras arbitrarias de memoria guest:
una dirección permite localizar el objeto en la siguiente captura, no nombrarlo
sin evidencia. Esperas cruzando bordeT quedan desconocidas/incompletas explícitas.
IDs signal/target empaquetados32bits (IDs observados83 ymenores); dirección64bits.
Valores signed32 ytimeout signed64 decodificados. MemoriaT fija524288 sin ampliar.

Referencia ABI primaria libnx: https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
WaitForAddress0x34 ySignalToAddress0x35 aceptan address/type/value ytimeout/count;
implementación común Kernel sirve D3D12 yVulkan. Por eso el seguimiento se sitúa en
el árbitro, no en el renderer ni modifica semántica para forzarFPS.
Fixtures tools/xbox/tests/address-wait-trace.py pasan: dirección errónea no atribuye
wake, signal/cancel/migración, valoresnegativos, bordesT. Analizador previo480 conserva
FPS59,125/59,375; fields nuevos ausentes no inventan resultados. IncrementalUWP27ops
+4ops y scheduler final por registrar. Tmanual para identificar dirección/señalizador,
overhead real/FPS ySeries pendientes. Limpieza no certifica mejora deFPS. Sincommit.

Gatebuild final scheduler4ops pasa; gitdiffchecklimpio. Trialmanual concentrado
lanzadoPID11468, Job5120MiB verificado, play1/fastmem0/jit_prewarm1; cpu_profile
omitido(defaultfalse), sin timeout de gameplay ni entradas programadas. Arranque
shadercache yprewarm enprogreso. T8 completas/overhead/dirección/waker/Series
pendientes; cierre usuarioQ. Sincommit.


## Gate PC diagnóstico concentrado: dirección y señalizador identificados (1 oct 2026)

Evidencia `build-uwp/log-review-2026-09-30/pc-focused-address-wait{,-diag}.txt`
y `pc-focused-address-wait-analysis.jsonl`. Q71s gameplay, retorno0; proceso ausente.
T480 completas27831/27911 eventos, FPS58,75/58,75, p99gap32,881/31,637ms,
max39,055/38,737ms, gaps>=25ms11/12. No mejoraFPS validada frente59,125/59,375;
escenas distintas yprofiling distinto impiden atribución causal.

Limpieza reduce eventos~92,95% frente394720/395684 previos. Dump fueraT tarda
0,346/0,388s frente~5,09/5,25s; consecuencia del menor volumen, no nuevo trabajo
sobre guardado. Sin truncamiento ni ampliaciónRAM/capacidadT.

Las470/470 esperas reales del guest83 se producen exclusivamente en dirección
0x210a010120, observed1/expected1, ArbitrationType2 WaitIfEqual, timeout-1 indefinido.
Todas despiertan por señal efectiva del ID79 enhost16012, retornan ResultSuccess0;
no cancelaciones, cero esperas incompletas. HostGPU productor1920 (idle/uploads)
es distinto delhost16012. Callgraph local SignalAddressArbiter solo entra desde
SVCSignalToAddress; no confundir79 conhiloGPU ni inventar su rol funcional.

Wake->resume median4/5us, p9918/10us, max33/70us. Mayor espera27,426/26,789ms:
T1begin5462,432,wake5489,852,resume5489,858 (+6us);
T2begin6694,930,wake6721,712,resume6721,719 (+7us).
Por tanto estas esperas largas ocurren ANTES delSignal del79; el árbitro/scheduler
no añade decenas dems después delwake en estas muestras. El valor1 es condición
observada antes dormir; no indica por sí solo qué objeto/semaforo representa ni
si la señal modifica la palabra (tipoSignal del79 todavía no capturado).

Mayor espera deambasT no contiene upload/GC>=200us: GPUthreadidle solapa21,974/
20,689ms. SegundaT tiene otras correlaciones: Wait22,951ms contiene11,182ms upload;
gap34,211ms incluye17,859ms GC. No hayuna causaGPU única ni prueba causal por
solape; parte delretraso puede venir detrabajo/sincronización del79 con terceros.
T1maxgap39,055ms yT2gap33,218ms sí incluyen leasesinterval2 legales. Cero retención
extra de ticks enambasT. Métricas apagadas (CPU otrosguests, Present, phasespacking,
readbackstates) son desconocidas, no deben interpretarse susceros como ausencia.

RenderError0;4assertsBQ a66,205--66,223s antesT1(79,562s). Headroom mínimo57,887/
73,738MiB (T1 cruza umbral64MiB de recovery GC; sintrace estados no certificar
fallback0). Diag pico muestreado5051MiB/margen68MiB. Mantener staging256/prewarm115
porcore/Job5120/T8. Próximo paso dirigido: seguirguest79 (PC/stop, Run largos,
IPC yesperas propias) ysourcePC/args deSignalToAddress paraesa dirección; conservar
83 yframechain. Determinar qué retrasa la señal antes de cambiarsemántica dewait,
prioridades o liberarframebuffers temprano. Sin nuevo cambio ni commit en estarevisión;
Series y60sostenidos pendientes.


## Candidato seguimiento del señalizador79 (1 oct 2026)

Tras gatefocus83, ambasT identifican mismo source79. TracksGuest central permite
solo79/83 en scheduler ready/dispatch, Run>=200us PC/stop, IPC/longSVC. Árbitro
registra también esperas reales del79 yquién las despierta (tercerhilo IDsolo en
wake efectivo, no se habilita profiler global). Permite dependencia83<-79<-otro.
CPUprofile/JIT detallado siguenfalse yautotestsbootfalse; HUD yT8 permanecen.

PC/LR deentradaSVC registrados desdeContextguardado porExitContext, sinleer código
ni memoria guest adicional. ParaSignalToAddress validado se registran address,
signaltype/expected/count signed32, sourceguest yResult final enmisma captureID.
Request/End no equivalen wake: se enlazan solo aEndWait efectivo capturado de
misma dirección/source. Args pueden señalar varios waiters; metadata objeto
compartido hastaResult final. Si wake externo alSVC orequestantesT, argsdesconocidos.
Dirección elegida enruntime, sinhardcode0x210a010120 porqueASLR puede cambiarla.
No se modifica prioridad, Signal, exclusivas, condiciones/timers ni colaBufferQueue.

Analizador enlaza PC/LR sourceSVC yargs/result alwait, yrecorta/unifica spans Run
>=200us yesperasSVC delseñalizador dentro begin->wake deltarget. Incluye4mayores
Run/esperas conPC/IPC porwait, sinconfundir solape con causalidad nielapsedconCPUbusy.
Cuantización timestamp1us: asociaciónPCalspan admite2us para redondeo; sólo usa
puntos deSVCcapturados, no decodifica función ni adjudica trabajo deguest porPCfinal.
El resto delwait sinspans conocidos no significaCPUpuro (runs<200us, JIT/flush,
preemption ycalls enborde no clasificados). Capacidad fija524288; gatevolumenTreal
pendiente, no inferir overhead0. IDs79/83 actuales deestegate; otrascorridas pueden
necesitar actualizarTracksGuest si el orden de creación cambia.

ReferenciaABI libnx https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
Signaltype0 no modifica valor;1 incrementa sicoincide;2 modifica porwaitercount.
Por eso elobserved1 delwait83 no basta para afirmar qué variable representa sin
capturar tipo/PC delsignal. RutaKernelcomún Vulkan/D3D12, referencia local
k_address_arbiter/k_process/svc_address_arbiter yphysical_core::ExitContext.

Fixtures tools/xbox/tests/address-wait-trace.py pasan wake/cancel/migración/bordes,
83<-79<-77, PCs/args signed/count-1/Result0 ysolapesrecortados independientes.
AmbasTprevias conservan58,75FPS y470waits/source79; no inventan args ausentes.
IncrementalUWP28ops pasa, gitdiffchecklimpio. Gate manualT8/dirección/sourcePC/
cuello79/capacidad/overhead ySeries pendientes; cambiosdiagnóstico sincommit.
Staging256MiB/prewarm115porcore/Job5120/T8 mantenidos. Ninguna mejoraFPS certificada.

Trial79chain lanzadoPID10568, Job5120MiB verificado;play1/fastmem0/prewarm1,
CPUprofile omitido0. Shadercache/prewarm enprogreso; usuarioT8 yQ sinlimitetime
ni entradasprogramadas. Gate79sourcePC/args/Run/esperas/volumenpendiente. Sincommit.


## Gate79chain: espera de syncpoint1 y trabajadores, corrección PC (1 oct 2026)

Evidencia `build-uwp/log-review-2026-09-30/pc-signalling-79-chain{,-diag}.txt`
y análisisJSONL regenerado descartando PCs inválidos. Q123s/retorno0, procesoausente;
T480 completas253950/245393events,FPS55,375/52,75, p99gap39,859/35,769ms,
max53,350/58,441ms. No mejoraFPS, no atribuir regresión causal a79trace: escenas,
presiónRAM ymetadata mucho más frecuente. PC/LR generó134774/131784events (>50%
volumenT); volver acompactar metadata corta si no hace falta en siguiente diseño.
Sin truncar, capacidad524288 ysinRAMextra. Dump3,198/3,162s fueraT.

Guest83443/422 waits reales, address0x210a610120, expected/observed1, WaitIfEqual,
todaswake79/Result0. Signal validado tipo1 SignalAndIncrementIfEqual,value1,count-1
(allwaiters), incrementa palabra condicionada; no eliminar esaespera ni cambiar
valor paraforzarFPS. Wake->resume median5us ambasT,p9931/23us,max71/74us.
Guest79 registra5431/5220 waits, cuatroaddresses; despiertan123/124/125.

Ejemplo determinanteT1gap53,350ms (5039,387--5092,737):83wait5055,262--5092,317
37,055ms. Guest79WaitSynchronization5055,553--5091,67236,119ms; previamente dos
nvdrvcommand1/ioctl. game-fence-wait a5055,502:syncpoint1,value14669;
game-fence-signal5091,612 tras36,110ms.79retornaWaitSynchronization60usdespués,
luegoSignalAddress al83 en5092,311;83resume6usdespués. Esto identifica una cadena
coherente deeventGPU/syncpoint1->79->83. Falta IDobjeto/handle deWaitSynchronization
para probar que es exactamentelamismakevent; no equiparar hostelapsed aGPUbusy.
T2tambiénsyncpoint1,value16908 espera31,411ms (5999,937--6031,351),79svc24termina
6031,435 y83wake6031,973. Consulta de código: nvhost_ctrlRegisterHostAction marca
GpuFenceSignal antesKEvent::Signal. FenceManagercommon ya usaGPUFencingThread en
Vulkan yD3D12(HAS_ASYNC_CHECKtrue), WaitFence->PopAsyncFlushes->IncrementHost.
Siguiente foco separar backendFencecompletion/asyncflush/callback/eventwakeup;
no asumir falta de polling porque ya haythread asíncrono.

T2mayorwait83 38,930ms (4968,100--5007,030) contienewaits79:
address0x210a6590dc espera21,368ms,wake123 en4998,890; otrospanmismaaddr5,406ms
wake124; address0x2153b8b9ac6,341ms,wake125 en5006,220. EstasdependenciasCPUworker
sonsegundafamilia, no justificar todoporGPU. MayorgapT2 58,441ms tieneleaseinterval2
legal,GC25,372ms yRun79elapsed25,267ms, tambiénArbitrateLock23,347ms. No sumar
solapes ni llamarRunCPUbusy (perfilglobalJIT/flushapagado). T2interval2sube5->43
respectoT1; cero lease extraticks. Headroommin50,414/74,363MiB, cruza64recoveryT1;
RenderError0,6BQasserts103,131--103,152 antesT1(115,963). Series/60 pendientes.

ERROR DE DIAGNÓSTICO CORREGIDO: sourcePC/LR tomaba thread->GetContext suponiendo
ExitContext lo guardaba siempre. Enrealidad soloactualiza siDebuggerEnabled; copia
puede serstale desdeúltimocontextswitch. Todos guest-svc-pc/lr deestegate ylas
atribucionesPC derivadas quedan descartados. Duraciones/IDs/args/señalesválidos;
GuestRunPc usabaGetContext vivo ysiguesiendopuntofinal válido, no hotPC.
physical_core ahora lee interface->GetContext acontextlocal ANTES deSvc::Call,
antesposiblefiber migration. Nuevo nombre guest-svc-live-pc/lr evita confundircon
logsantiguos. Parser ignora explícitamentePCs antiguos ytestsverifican conservar
args/timing sinadjuntarPCviejo. Fixtures pasan, buildincremental6ops ydiffcheck
pasan. No nueva corrida conPCcorregido, no afirmar gatehardware porcompilación.
Corrección necesaria realizadadurante revisión; sincommit. Próximo gate necesita
identidadWaitSynchronization/event/syncpoint yworkerdependency, mantenertraza reducida.
Staging256/prewarm115porcore/Job5120/T8 conservados.


## Candidato cadena fence->evento->79 y trabajadores (1 oct 2026)

Usuarioautoriza atacarambasfamilias. FenceManager compartidoVulkan/D3D12 añade
identidaddefence porvida(.get), queued/dequeued, elapsedWaitFence>=200us,
PopAsyncFlushes total, mutexcache, texture/buffer/query porfase, callbacksbegin/
elapsed/done. D3D12 InnerFence::Queue relacionaidcontick. Scheduler::Wait separa
espera de recording submission deSetEventOnCompletion+WaitForSingleObjectEx.
No nuevasqueriesGPU, fences ni submits, no polling ni prioridadnueva: se conserva
WaitFence->PopAsyncFlushes->operations. Spans nested no sumables yelapsedhost no
GPUbusy. Identidadfence reciclable; parser crea nueva vida alqueued sin mutar la
referencia que ya apunta avida anterior. BordeT oqueuedfueraT dejan camposunknown.

KSynchronizationObject::Wait marca sólo esperas reales79/83 trasvalidación, objeto(s)
yaresueltos/referenciados ydespuésresumptionResult mismacapture. NotifyAvailable
marcaobjeto exactoque termina espera. Nolecturaguestextra/handlelookup ni referencia
adicional. NVhostctrl relaciona readableevent consyncpoint+value alarmar; marca
callbackevento sólo en transiciónWaiting->Signalling antesSignal, ytiempolargoSignal.
Parser exige mismaidentidadobjeto+syncpoint+target para asociarwakeactual aNVevent;
ademásrelacionaNVSignal albatchfenceencallbacken mismo host. No tratar request de
señal como wake efectivo, ni lasesperasgenéricas comoGPU sineseenlace. Punteros
son sólo IDs opacos durantevidaobjetos, no sedesreferencian analizando ni exponenUI.

TracksRunGuest123/124/125 añade Run>=200us PC/stop yesperaslargas>=200us (incluye
IPC/WaitSynchronization compactados paraworkers); no scheduleredges/IPCnames globales
niperCPU/JITprofiling. Árbitro conserva target79/83 ysourceID dequienlosdespierta.
Elimina metadataPC/LR deSleep/lock/addresscortos; quedan enIPC79/83 ySignalToAddress
con contextoARMvivo. PC deARM aentradaSVC no necesariamente dirección delopcode;
no inferir nombre/CPUbusy porPC. Workers danPCendpointRun ySVCID, sinPCporcada yield.
CPUprofile0,prewarm115/staging256/Job5120/T8/524288 conservados. Gatevolumenreal
pendiente: noafirmar coste0 ni proyectar garantía de capacidad.

Microsoft: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
levantaevento alalcanzarvalor ypermitevarios threads;elapsed deAPI/eventwaitincluye
planificaciónhost. https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
UINT64_MAX indicadeviceremoved, semánticaoriginal/fallbackconservados. ModeloVulkan
comparteFencingThread, PopAsyncFlushes yHostsyncpoint; D3D12Waittrazadoesbackendlocal.

Fixtures address-wait-trace.py pasan identidadobject/syncpoint/fence, tick, fases
nested, lifetime reutilizado yobjectincorrecto noheredaenlace; fixturesanteriores
migración/signed/cancel/PCstale pasan. DosTpreviascompatibles55,375/52,75 conlinks
nuevosausentes desconocidos, no inventados. IncrementalUWP38ops pasa(incluyeVulkan
porheadercomún), gitdiffchecklimpio. ManualT/Q, chaincompleta/volumen/overhead ySeries
pendientes; sólodiagnóstico, ningunamejoraFPScertificada. Sincommitnuevo.

Trialcompletionchain lanzadoPID19100, Job5120MiB verificado;play1/fastmem0/
prewarm1 yCPUprofile0. Shadercache/prewarm enprogreso. UsuarioT8manual yQ, sin
timeoutgameplay/entradasprogramadas. Chain/volumen/overhead/Series pendientes.


Gate PC completion-object (1 oct): Q139s/retorno0; T1/T2 completas181785/186445 eventos, FPS56,25/57,875, p99gap39,721/32,416,max96,803/51,917ms; T3 accidental excluida. Objeto kernel confirma316/316 y263/263 waits79 ligados a NV syncpoint1. T1wait83 93,193ms incluye wait79 84,658:69,548ms antesenqueuefence+14,918ms esperaD3D12host; pipelinebuilt termina7,841ms antesenqueue y ventana perf registra3stalls/101,5ms, candidatoWaitBuilt sincausalidadindividualconfirmada. T2wait79address33,381ms wake125; Run123~35ms elapsed, noCPUbusy. Wake83mediana5us; flushmax0,901/1,049ms, nocuello largo. Margen83/103MiB,commitmuestreado5031MiB; RenderError0,5BQassertantesT, leasesextraticks0. SiguienteWaitBuilt/DXIL/PSO ycadena workers.256/115/5120/T8, sincommit; Series/60pendientes. Detalleenxbox_performance.md.


Candidato1oct pipelines: DXIL firmado enlazado reutilizado por inputSPIRV completo+opciones/stages/longitudes, por juego, LRU4MiB contabilizados/max128; concurrentes compartencompilacion sinmutex duranteMesa/firma, failedretry/bypassoversize preservados. PSOs siguenindependientes, noGetCachedBlob/noskipdrawspequenos. PublicacionPSO IsBuilt release/acquire. Tfrontend/worker/DXIL/Mesa/validatorlock/sign/PSO/WaitBuilt+cacheoutcome porpipeline, noCPUglobal/norelojfueraT; fasesanidadas nosumar. Harness16concurrentes/collision/content/options/retry/1000evictions/LRU yparserlifetime/borders/nesting pasan, build44ops UWP correcto. Gameplayhits/WaitBuilt/FPS/Series pendientes, sincommit;256/115/5120/T8 conservados. InvestigacionMicrosoft/Dolphin/Vulkan ylimites en rendimiento.


Gate final candidato shaders: DXIL immutable compartido por shared_ptr entre PSOs, sin copias en hits; consumidores sobreviven a eviction. Harness actualizado valida eviction real y bypass al llenarse cupo de compilaciones en vuelo. Build10ops+4ops final correcto; parser pipeline/address y regresionT1/T2 previa correctos, gitdiffcheck limpio. Correccion include frame_trace omitido al ordenar includes: link final recompilado, no afecta corrida anterior. Trial manual lanzadoPID14184, Job5120MiB verificado;play1/fastmem0/prewarm1/CPUprofile0,115core/staging256/T480. T1/T2 hits/esperas/FPS/memoria ySeries pendientes; usuarioQ, sinlimitegameplay, sincommit.


GatePC DXILcompartido: Q76s/retorno0; T480completas182760/175587events,FPS56,5/55,p9939,721->41,761 y32,416->40,624,max56,828/48,932ms. Sinmejorageneral.1718PSOsprecargados; cero creacion/WaitBuilt/cacheoutcome enT, ahorroDXIL/hits no medidos. Margen25,918/61,996MiB,GCmax19,251/23,958ms; gapT2GC18,653 conappfree62,070<64MiB compatible recoverysincrono (contadorreadbackoff, no conteo confirmado). OtragapT1WaitD3D12async43,212ms. RenderError0,5BQassert/8unmappedantesT. Parser corrigeforegroundwaitmismotick: host/dequeue necesarios parafaseanidada, fixturepasa. Siguiente margen/GCsync, despuesD3D12/CPUworkers.256/115/5120/T8 intactos, sincommit/Series/60pendientes; detalle enrendimiento.


Guard memoria D3D12 (1 oct): prevencion128/64MiB y emergencia10MiB app-free; staging256->128->64->0, retiro por fence sin rings solapados, Finish solo ultima emergencia. DXIL opcional y staging libre se recortan; readbacks pinned/dirty guest conservados, heaps vacios siguen GC existente. Muestra memoria compartida con GC, sin hilo nuevo; swap/pop evita desplazamientos masivos. JIT115/core no se decommitea. Politica/limites/fuentes/gates en docs/xbox_performance.md. Sin commit; gameplay/Series pendientes.


GatePC memoryguard: Q231s/retorno0,T480 completas181034/186029. Guardactua127,969MiBfree antesT: ring256 retiradoy128 repuesto, capacidad-128MiB real. MargenT93,344/136,520 vs25,918/61,996; FPS56,5/58,25 vs56,5/55,p9938,598/30,355, max63,871/40,592. T1GC43,977ms empeora peortiron, noestabilidadgeneral ni causalidadFPS. Commitmuestreado4986MiBmax,RenderError0,1BQassert+4unmappedantesT. Rama10MiB/Finish/recovery64/overhead/Series pendientes. SiguienteGC largo yD3D12waits;115core/5120/T8conservados,sincommit. Evidencia enrendimiento.


Cierre autorizado usuario1oct: pendientes porprioridad documentados en xbox_performance.md: GC/readback43,977ms; cadena D3D12/CPUworkers fueraGC; overhead/ramas64/10/recovery delguard; gateSeries/admissioncontrol/eventoslimite/JITdecommit real. No iniciar nuevo candidato hasta siguiente indicacion.


ComparacionVulkanPC autorizada1oct: UWPsoloCoreWindow, VulkanWindows requiereHWND; no exe desktop existente. Preparados build-vulkan-pc.bat (buildcompleto usuario yautolaunch) yvulkan-run.ps1 (Job5120/T8/Q/fastmem0/prewarm115/mismosdatos, logsseparados). FrontendSDLcorrigeestado/AppResult/entrada lifetime/callback dangling, agregaManualContentProvider ystatusT/FPS/commit. /ZsMSVC yparserPowerShell pasan; configure/link/gameplay pendientes. LoaderPCpresente; diferenciasAppContainer/GC/audio/VRAM documentadas, noA/Bcausalcertificado. Sincommit;detalle rendimiento.

Correccion build Vulkan PC (1 oct 2026): primer configure fallo antes de compilar
el proyecto porque PATH resolvio CMake de devkitPro/MSYS2 (/opt, /home, /c) junto
con MSVC y Ninja nativos. build-vulkan-pc.bat ahora fija ejecutables CMake/Ninja
incluidos en Visual Studio, antepone sus directorios para subprocesses y pasa
CMAKE_MAKE_PROGRAM explicito. Si la cache existente contiene rutas MSYS, usa
--fresh para regenerar CMakeCache/CMakeFiles; no borra objetos ni build UWP.
Gate aislado con el mismo entorno: ABI C/C++ MSVC correcto, configure/generate,
compilacion/link y ejecucion de probe nativo retorno 0 (CMake 4.3.1-msvc1).
Build completo Vulkan y gameplay siguen pendientes; usuario repite el mismo
comando build-vulkan-pc.bat. No certificar el emulador con este probe.
Segundo bloqueo configure Vulkan PC (1 oct 2026): CPMUtil.cpm_find_program
seleccionaba GIT_EXECUTABLE de devkitPro; alli no existe patch.exe y los hints
no alcanzaban Git for Windows. Script fija GIT_EXECUTABLE y PATCH_EXE al Git
for Windows instalado (Program Files, fallback LocalAppData), comprueba ambos
antes del configure y antepone Git/cmd. Perl Strawberry, NASM y glslang se
seleccionan como en build-env; CMake/Ninja VS conservan precedencia.
Gate ligero con el prefijo real del script: Git 2.55.0.windows.5 y GNU patch
aplican un diff y contenido verificado; where confirma Perl/NASM/CMake correctos.
No ejecutado build completo ni descarga pesada; usuario repite mismo comando.
Bloqueo compilacion desktop host_memory (1 oct 2026): g_fastmem_* y constantes
histograma estaban bajo HOST_MEMORY_USE_FROM_APP, pero Map/MapView/UnmapView se
compilan en todo Windows y las referencian incluso si hybrid nunca se activa.
Contadores movidos a namespace anonimo Windows comun; demanda de commit, VEH e
inicializacion hybrid conservan guard UWP. Sin cambio de asignacion/presupuestos.
Gate: objeto real host_memory.cpp compila con Ninja/MSVC desktop Vulkan y tambien
con Store CRT UWP; git diff --check limpio. Build completo/link/gameplay pendientes.
El log adjunto paraba por C2065 (simbolos no declarados); C2672 min/max era derivado.
Gate enlace y arranque Vulkan PC (1 oct 2026): link fallaba LNK2019 main porque
SDL3 con UNICODE genera wmain/wWinMain y el CMake heredado forzaba
/ENTRY:mainCRTStartup. WIN32_EXECUTABLE TRUE deja al CRT/linker elegir arranque
Windows adecuado. Modelo SDL: https://wiki.libsdl.org/SDL3/README-main-functions
Launcher corregido a bin/eden-cli.exe (OUTPUT_NAME de src/CMakeLists.txt), incluye
eden-cli al detectar otra corrida. Batch detiene cualquier retorno distinto de0,
incluso -1/4294967295 del linker (if errorlevel1 no atrapaba ese retorno negativo).
Incremental desktop pasa (regenerate, objetos frontend/scm y link, 7 operaciones),
parserPS y gitdiffcheck correctos. LanzadoPID18060, Job5120MiB verificado; log
Render.Vulkan y carga perfiles JIT cores0/1/2 confirmados, proceso vivo al revisar.
Usuario T8/manual/Q, sin timeout; precarga/gameplay/FPS/margen pendientes.
Warnings conversion/PDB sirit no eran el bloqueo. No cambio rendererXbox, sincommit.

GC44ms candidato1oct: Emergency preventivo128/histeresis192 antes permitia40downloads sin limite; ahora1ms/1 intento salvoappfree<64MiB oGPUbudget agotado. T fasesprepare/staging/record/wait/swizzle/release yreasonfallback, pinned8MiB/115JIT/guardconservados. Harnesspolicy/parserpasan; fullgatemanual pendiente, sincommit. Detalle yfuentes xbox_performance.md.

Gate final candidatoGC1oct: incremental UWP completo correcto51 pasos planeados
(incluye runtimeD3D12 y cache comunVulkan), harnesspolicy/fixtureGC ycompatibilidad
logmemoryguard antiguo pasan, gitdiffcheck limpio. Relanzado D3D12 PID2920,
Job5120MiB verificado, play1/fastmem0/prewarm1/CPUprofile0 (default),115/core,
T480. Loadstatus0/shadercache y carga perfiles JIT confirmados; precarga/gameplay
manual enprogreso. UsuarioT nueva/recorrida yQ, sin timeout de gameplay. Mejora
GC/FPS/margen/coherencia/capacidadT ySeries siguen pendientes; sincommit.
Trampa launcher: Appx no carga desdePowerShell7 (0x80131539); local-run invocado
con WindowsPowerShell5.1 de System32 como en gatesprevios. No hubo app lanzada por
el primer intento7; no matar procesos manuales para reintentar.
## Gate PC GC acotado (1 oct 2026)

Evidencia archivada pc-gc-bounded.txt, pc-gc-bounded-diag.txt y
pc-gc-bounded-analysis.jsonl (build-uwp/log-review-2026-09-30). Proceso cerrado,
Q tras632s segun diag y retorno0; logwall1261s no equivale a CPUbusy. Precarga
20,8s (7,234->28,031s). DosT480 completas188588/183709eventos, sin truncar.

|Metrica|memoryguard previo T1/T2|GC acotado T1/T2|
|---|---|---|
|FPS|56,5 /58,25|58,25 /57|
|p99 gap ms|38,598 /30,355|34,332 /35,337|
|max gap ms|63,871 /40,592|51,005 /41,884|
|GC max ms|43,977 /24,235|5,588 /17,390|
|min app-free MiB|93,344 /136,520|91,797 /126,406|

GC max cae87,29% y28,24% observado. NoA/Bcausal: recorridos/perfiles y duracion
son distintos; T1 mejoraFPS/p99, T2 empeora. No60 sostenidos ni estabilidadgeneral.
Guard funciona antesT:1227,274s ring256->128 conheadroom127,922MiB, retire1227,298
restore1227,351. Diag maxmuestreado5028MiB/margen91, no certificapicoabsoluto.

Desglose: T1 GC5,588ms tiene copyrecord5,580ms para256bytes formato21
(B10G11R11_FLOAT). T2 GC17,390ms tienecopyrecord16,703ms+staging0,677ms para
130944bytes mismoformato. OtroGC6,647ms=copy5,171+staging1,328; otro5,936ms
incluyestaging5,912ms para4096bytes R16G16B16A16_FLOAT. Sinspans dewait/sync
reason; T1readbackqueued197/ready198 (uno venia de antesT), T2queued9/ready9.
Cierrequeued391/ready390/stale0/sync0/pending0/peak8MiB: la ultima copia pudo
serdescartada alshutdown, no dataconsumida sinfence. No fallbacksincrono en toda
corrida. Evictions365/463; TextureCreate sigueoff, recreacion0delparser NO es
medida valida dechurn ni prueba de ausencia derecreaciones.

Lectura: costos restantes estan en hostPrepare/DownloadMemory y staging, no en
espera explicita de GC alGPU. Copyrecord puede incluirtransicion, writeback de
vista reinterpretada, CreateCommittedResource temporal DEFAULT y comandos porfila;
no prueba16,703ms de CPUbusy ni identifica una llamada individual. Inspeccion
DownloadMemory confirma tempnuevo pormip/layer cuando offset/pitch no cumplen
footprint yCopyBufferRegion porfila para obtenerlayouttight. Siguiente candidato:
eliminar creaciones temporales reiteradas/reutilizar buffers protegidos porfence,
o readback footprints alineados+repack trascompletion, manteniendo capreal8MiB,
versiones y coherencia. Medir separados CreateTransfer/transition/record antes
atribuircoste aallocator/driver; no subirRAM para ocultarlo.

PeorframeT1 gap51,005ms soloGC0,207ms, upload10,349 yGPUthreadidle33,175; fence
completado enmismo tramoesperaGPU32,574ms. T2max41,884ms soloGC0,231 yidle34,149.
Gaps restantes no los explica GCsolo; cadenaD3D12/fence sigueprioridadposterior.
RenderError0, cincoBQassert y8unmapped antesT; BufferQueueabandoned alQ esperado.
Visual ySeries pendientes. Fuente/codigo sin cambios adicionales, sincommit.
Gate GPU footprints1oct: AppContainer+debug homebrew termina16,3s/retorno0; logs
GC direct footprint yGC deferred gate pasan, RenderError/Critical0. Bytes reales
B10 127x63/5mips/3capas, RGBA8volume13x7x3/3mips, BC1native64x32/3mips,
idempotenciaCompact; gatesprevios move/GPUstale/CPUstale/cap8MiB/discard/emergency
siguenpasando. Evidencia gc-footprint-gate{,-diag}.txt. Bootselftest activado solo
paraeste gate y restauradofalse antesbinario de gameplay. Primer gate fallo por
fixtureBCarray crudo con decode_bc_arrays activo (creo formatoRGBA8 pero elfixture
pasabaBCraw): UploadMemory esperaba bytes ya convertidos. Fixture corregido a
BC1native1capa y rechazo explicitodeplanBCarray convertido; no tocarfallbackSeries.
Se archivaprimerfallo gc-footprint-gate-first.txt. No afirmar fallo del fastpath ni
corregir descompresion fueraalcance. Buildnormal final/manual/Series pendientes.
Gate final candidatefootprints1oct: buildnormal4ops/link correcto trasrestaurar
bootselftestsfalse. CPU20000cases/parser fixtures pasan; gateGPUdebug completo
retorno0/RenderError0. TrialD3D12lanzadoPID24052,Job5120MiB verificado,
play1/fastmem0/jitprewarm1/CPUprofile0,115/core,T480,stagingnormal256+guard.
Sinlimitegameplay, usuarioTnueva/recorrida yQ. Precargaenprogreso; confirmar
fastpathcounts/compact/copyrecord/GCmax/FPS/margen ySeries queda pendiente.
Sincommit, no cambios gameplayVulkan/FSR. Detalle yfuentes en rendimiento.

Gate PC footprints1oct: Q82s/retorno0, ambasT480 completas180353/189541eventos.
Fastpath210/5, GCmax3,041/1,111ms vs5,588/17,390; copyrecord sin eventos>=200us,
compactmax0,947ms. FPS55,5/59 vs58,25/57: no mejora general certificada. Margen
116,121/155,723MiB, commit muestreado4978MiB, guard staging256→128. RenderError0,
5BQassert y31unmapped antesT; sync/stale0, pending0 al cierre. Peores gaps48,509/
39,626ms contienen GC0,572/0ms; siguiente fence/flush/worker y cadena frames.
Evidencia pc-gc-footprint{,-diag}.txt/analysis.jsonl, Series/60 pendientes,sincommit.
Pendientes priorizados1oct: P0 reproducir/corregir BQassert ylecturas no mapeadas;
P1 cadena submit/fence/flush/callback/wake/QueueBuffer; P2 churn/fragmentación y
swizzle GC; P3 CPU residual/coberturaJIT/precarga. Gates transversales: escenas
comparables, prueba prolongadaSeriesUMA, fallbacks/guard y60sostenidos. Vulkan
compila/arranca pero comparación manual pendiente. Plan y criterios completos en
`xbox_performance.md`, sección Prioridades pendientes después del GC directo.

Frontend1oct: biblioteca opt-in `library=1` / scripts `-Library`, raízLocalState/games,
NSP/XCI/NRO ysubcarpetas, scanner cancelable/cap10000/depth5 sin abrir contenedores.
UI Direct2D/DWrite+D3D11 temporal/CoreWindow, liberada antesD3D12. Mando/teclado
navegan; ajustes básicos letra/posición ydeadzone8/12/18% guardadosLocalSettings.
BuildUWP yscanner desktop pasan; harnessStoreCRT fueraAppContainer falla0xc0000135:
compilarharness con vcvarsall desktop. PCPID8952/Job5120 biblioteca visible confirmado,
commitreposo36MiB; usuario sinmando. Transición/gameplay/persistencia/manual ySeries
pendientes. Diseño, comandos, fuentes yreciclajeEden en docs/xbox_frontend.md.

Rediseño biblioteca tras rechazo visual: carátula/título destacados, tarjetas,
foco menta, márgenesTV, panelMando F1/View yratón. Loaders deqt_common ReadTitle/
ReadIcon/ReadControlData, metadata solo página5 enworker sinInitialize/Load/Run,
iconosretenidos<=1MiB/entrada yWIC256x256/cacheGPU12. Buildincremental4ops/harness
pasan. PCPID20692 Job5120: títuloWonder real+icon93583bytes, commit44MiBreposo;
selecciónmanual→D3D12→Load0 confirmado. Proceso ausente durantecache sinQ/retorno:
cierre/gameplay no certificados. Evidencia pc-library-cards{,-diag}.txt;
visualrediseño/panel/mando/Series pendientes. Fuente/diseño docs/xbox_frontend.md.

Usuario confirma carátula/título ymejora visual. Ajuste posterior: logo original
EdenPNG deltema Qt copiado Assets/EdenLogo.png al empaquetar, cover rounded12 real
con máscara D2D antialias/cachegeom. Build3ops/diffcheck/scriptparser pasan;
gatevisual delajuste/panel/Series pendientes,sincommit.

ImportaciónGame GPC: Move-Item desdeDownloads conservaACL si mismo volumen;
enumeraciónlista funciona pero ReadIcon/ReadTitle falla permission denied. Corregido
archivo con icacls/grant:R alSIDespecíficoEden obtenidoACLgames. No conceder a todas
lasapps. ValidarpermisosAppContainer además del tamaño al importar dumps; refrescar
biblioteca. Detalle docs/xbox_frontend.md.

UIprompts porplataforma: DeviceFamilyWindows.Xbox muestra soloA/B/cruceta/View/Menu;
Desktop Enter/Esc/F1/flechas/R/Q, incluso header/CTA/empty/panel. Subset11PNG Kenney
InputPrompts1.5A CC0(4695bytes), license/README enAssets/InputPrompts, cachelocal
porcanvas sinred. Build3ops/diffcheck pasan, visualPC/Seriespendiente,sincommit.
Fuentes ycriterios docs/xbox_frontend.md.

SelectorPC jugador1: automático/teclado/mando porID persistido; catálogo Windows
raw+Gamepad estándar compartido con gameplay, hotplug500ms. Series conservaauto.
Raw sinmapeo aparece deshabilitado; no soporte arbitrarioBluetooth ni multijugador
certificado. Frontend separado navegación/canvas/entrada, metadata porpágina sin
recorrerbiblioteca cada16ms, assetshelper/caches. Harnessselección pasa ybuild
incremental pasa; últimoajuste/visual/hardware/Series pendientes. Sincommit;
detalle yfuentes en docs/xbox_frontend.md.

Gate selectorPC: build finalincremental3ops/harness selección ybiblioteca PASS,
diffcheck limpio. Biblioteca abiertaPID15052 Job5120 verificado, sin cierre
observado. Panelvisual/mandofísico/Series pendientes. First-chance0x6d9 a8s,
procesoactivo; causasinconfirmar. C++/WinRT Path requiereincludeStorageexplícito.
Sincommit; fuentes/diseño/evidencia en xbox_frontend.md.

TecladoPC implementado: editor click/captura4s/Esc/borrar/restaurar, guardado
ParamPackage v1 poracción. Reutiliza Keyboard/InputFactory/GenerateKeyboardParam;
callbacks->máscaraatómica->virtualpad, no polling28bindings. Ambossticks/cruceta/
triggers/clicks/home/captura, foco reset. Q/T defaultlibres, developer_hotkeys1opt-in;
local-run directo conservaopt-in/Qmanual, Library/release no. GateAppContainer
selftestreal PASS PID13988 ydesktopbinding PASS/build7ops. Usuario rechazódibujo
inicial; rediseño usa contornos/posicionesoriginales QtPro, pathsD2Dcached,
3grupos/2columnas/keycaps/centrado. Build7ops/harness3pagescorrectos; linkfinal,
visual/persistenciareinicio/gameplay/Series pendientes. Sincommit; xbox_frontend.md.

Gate rediseño final: linkincremental3ops correcto trascorregir cachealign/font.
RelanzadoPID12596 Job5120verificado ykeyboardselftestAppContainer PASS; no inputs
programados nigameplaypor tiempo. Previewvisual final ypersistencia/manual
pendientes delusuario, appse dejaabierta. Sincommit.

Usuario confirma que cerró manualmentePID12596; no atribuir ausencia de proceso
acrash. Reporta aviso de guardadoMando encimaJugar: bugestado compartido notice.
Corregido separando avisos biblioteca/controller/keyboard; confirmaciones controles
solo en panelrespectivo, caducan3s; errorespersisten mientraspanelabierto, limpieza
al cerrar/cambiarcaptura. Sin recorrido/alloc adicional porframe salvoclearalexpirar.
No crear tests que reflejen solo etiquetas; buildincremental yvisual son gate.
RediseñoPro permanece, sincommit.

Gateavisos: incremental7ops correcto, diffchecklimpio. Relanzado biblioteca
PID23500, Job5120verificado ykeyboardselftestPASS. Avisos fuera delpanel ycaducidad
visual pendientes; se dejaappabierta, cierremanualusuario. Sincommit.


## Pro Controller original por USB (1 oct 2026)

El mando del usuario es Nintendo `057e:2009`, conectado por USB. Windows lo
publica como RawGameController (18 botones, 4 ejes, 1 hat), pero
Gamepad.FromGameController devuelve null. Sus lecturas normalizadas interpretan
mal el paquete completo: botones variables en reposo y ejes incoherentes. No
habilitar esa fila mediante índices arbitrarios.

La prueba real en AppContainer establece `DeviceAccessStatus::Allowed`, pero
HidDevice.FromIdAsync(ReadWrite) devuelve null. **Read sí funciona**: 200 informes
0x30 de 64 bytes en dos segundos. El permiso `humanInterfaceDevice` se declara
en el manifiesto; cambiar el manifiesto de un paquete registrado exigió subir
versión (0x80073CFB). Versión final de la prueba PC: 0.2.70.0. No borrar LocalState
ni los juegos para resolver una reinstalación.

`uwp_pro_controller.cpp` usa eventos HID solo en PC, enumera asincrónicamente
cada 2 segundos en el hilo UI y conserva objetos por ID de interfaz. Biblioteca
y gameplay comparten la misma lectura. Un informe debe ser 0x30, contener el ID
al principio y tener al menos sizeof(InputReportActive), máximo 64 bytes; el
poller original hace memcpy y requiere esta validación previa. Se reutiliza
JoyconPoller (botones/sticks), con calibración nominal de Eden (centro 0x800,
rango 0x6cc), zona muerta existente y estado protegido por mutex. El buffer se
lee directamente mediante IBuffer.data(), sin DataReader/alloc propio por
informe. Los callbacks capturan weak_ptr; desuscripción/Close al retirar el
objeto. Sin datos durante 250 ms, la lectura vuelve a neutro. El catálogo guarda
availability como snapshot para detectar cambios en UI; reconexión reabre objetos
sin informes válidos. Nintendo tiene disposición/letras originales de Switch:
el intercambio letra/posición Xbox solo se aplica a dispositivos Gamepad.

No hay escrituras USB, calibración SPI real, vibración ni motion en esta ruta;
Bluetooth/otros clones no están certificados. La lectura 0x30 está demostrada en
esta conexión; si tras reconectar solo llega otro modo, no se declara compatible
hasta implementar y validar el cambio de modo. No se inicializa SDL desktop en
UWP; SDL3 solo aporta headers requeridos por los tipos del poller existente.
Xbox conserva su entrada Windows.Gaming.Input.Gamepad y no enumera HID Nintendo.

Gates: build incremental y selección auto/manual/reordenar/reconectar/teclado
PASS. Self-test **dentro del AppContainer**, con JoyconPoller real: 14 botones
normalizados individualmente, ZL/ZR/Home/Captura, release a neutro, sticks centrados,
paquetes truncados y modo incorrecto PASS. Hardware PC: decoder ready=1,
botones=0 y los cuatro ejes≈0.001724; zona muerta elimina ese pequeño offset.
Biblioteca lanzada PID17396 con Job5120MiB; selección/navegación manual,
transición a gameplay, reconexión real y Series pendientes. Diagnóstico de
transporte/self-test únicamente con pro_hid_probe=1; no logging de informes en
release. Sin commit.

Fuentes: [Microsoft HID y capacidades](https://learn.microsoft.com/en-us/uwp/api/windows.devices.humaninterfacedevice),
[Microsoft RawGameController](https://learn.microsoft.com/en-us/windows/uwp/gaming/raw-game-controller),
[protocolo Nintendo en SDL](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch.c).
El rechazo ReadWrite se midió localmente; no se atribuye a bloqueo exclusivo,
filtros o permisos sin evidencia. PnP no muestra upper/lower filters ni device
internal en el mando conectado.


Gate manual Pro USB: usuario confirma que puede seleccionarlo y navegar con
cruceta/A/B. Reporta ayudas todavía de teclado: el canvas elegía iconos por
plataforma PC/Series. Corregido para usar el mismo SelectController que entrada:
Teclado o sin mando→teclado; Gamepad→Xbox; ProUSB→Nintendo A/B monocromos, -/+
y cruceta. En Series se conserva Xbox. Cambia al seleccionar y al actualizar
catálogo por hotplug. Los glyphs Nintendo son D2D propios (círculos/letras y
líneas), formatos reutilizados, sin descarga/assets ni paths porframe; cruceta
usa el glyph neutral de navegación existente. Editor de teclado mantiene
instrucciones de teclado, porque esa captura requiere teclas.

Build incremental final PASS, git diff --check limpio. Ajuste de swap por
posición restringido al Gamepad Xbox (el Pro ya tiene disposición Switch).
Relanzada biblioteca PID16884, Job5120 confirmado, sin pro_hid_probe ni captura
de informes debug. Validación visual de ayudas, gameplay/replug y Series
pendientes; selección/navegación Pro USB sí confirmadas. Sin commit.


Corrección editor1oct: lectura de mando continúa en modal teclado, actualizando
edges; solo B produce Volver mientras está abierto, incluso durante captura.
La misma rama de cierre cancela captura y consume Quit, evitando cerrar biblioteca
con ese mismo edge. A/cruceta/otros botones no se traducen a teclas ni acciones
de biblioteca dentro del editor. Ayuda B/Volver visible cuando hay mando activo.
Cruceta roja era el PNG Kenney de color, no indicador de error: reemplazada por
glyph D2D blanco compartido para Nintendo/Xbox, sin assets/alloc nuevos.
Buildincremental pasa; comprobación manual B/abierto/captura y visual pendientes.
Sincommit.


Stick navegación1oct: biblioteca y menús aceptan stick izquierdo del mando
seleccionado (Pro USB/Gamepad PC/Series). Helper puro StickNavigation, sinalloc:
entrada>=0.55/salida<0.35, eje dominante con histéresis diagonal, primer paso
inmediato, repetición tras350ms ycada120ms; no catch-up trasstall. Cruceta tiene
prioridad. Cambio de dispositivo/contexto/errores requieren recentrar antes
de nuevo movimiento; editor teclado mantiene solo B/Volver y no captura mando.
AyudaExplorar/Elegir muestra cruceta ystick L monocromos. Gameplay no cambiado.
Harnessdesktop drift/histéresis/diagonal/repetición/inversión/modal/stall/NaN PASS,
buildincremental4ops/diffcheckcorrectos. Usuario cerró anterior, relanzado
PID14084 biblioteca/Job5120; gate navegaciónmanual ySeries pendientes,sincommit.


ASTC sampler original2oct: prototipo opt-in `astc_sampling_probe=1`, aislado antes
de Core::System, sin integración guest. SPIR-V->DXIL SM6.4, buffers raw agrupados,
texel solicitado/point/bilinear/mips, arrays/cube y3D mediante slices ASTC2D.
PC AppContainer AMD5300M:60717 consultas CPU/direct/raw compute+fragment y22369
nativeRGBA pasan, errores máximos0,000000298/0,005099639 (precisión filtros native),
14footprints/particiones/dual-plane/sRGB/invalidos/mips/costuras, retorno0 yRender0.
Payload717328bytes=×1, buffersorigen+metadata786432=×1,096335; esto NO incluye
PSO/driver/scratch ni certifica sampler total≤×2. Timestamps4096queries muestran
16,547–369,313x coste frenteRGBA nativo en corpus sintético: no integrable default,
noFPS gameplay. Pico proceso normal732,875MiB; arranque más lento1797,4MiB.
Buildincremental ySPIRV normal idéntico sinrecordsdebug pasan. Consola0.2.71.0
empaquetada en build-uwp/astc-probe-series, símbolos archivados; Series pendiente.
JIT115/core,Job5120 ypolíticasguest intactas. Sincommit. Diseño/fuentes/resultados
en [`xbox_astc_sampling.md`](xbox_astc_sampling.md). GBV/smoke autónomos
se registran al cerrar el gate; no certificarlos sin PASS.

Trampas del prototipo: GLSL `%` con operandos negativos es indefinido; wrap usa
restos positivos, contrastado con CPU y samplers nativos. Oracle CPU histórico
redondea UNORM8 y carece de expansión ASTC sRGB; nueva especialización de prueba
aplica el perfil Khronos sin cambiar Decompress normal. Cubemap corner devuelve
media de tres caras, oracle hardware admite alternativas legales; point con ejes
empatados no tiene cara hardware única. GBV frontend se activa con `debug_layer=gbv`,
NO `gpu_validation=1`; -DebugLayer sobreescribe esa opción con debug_layer=1.
Un primer ensayo etiquetado gbv sin activación real se invalidó explícitamente.
GBV real excedió240s y se archivó FAIL/timeout, no prueba de corrupción. Reintento
900s preserva5120MiB; registrar resultado y no usar tiemposGBV como rendimiento.


Cierre GBV ASTC2oct: real900s agotado, ambas validaciones60717/22369 pasan sin
mismatches/RenderError, pero solo20/32 benchmarks ysin cierre/retorno0/PASS: gate
incompleto, NO certificadoGBV. Pico5116,48MiB casiJob5120 sin cambiarpresupuesto.
Ensayo240s también timeout. Datos archivados194240-gbv/193600-gbv; tiempos
instrumentados no son evidenciaFPS. El gate completo debug sinGBV sí pasa.


Smoke normal ASTC2oct: opción ausente, homebrew/PID23340/Job5120/debug, sentinel,
framebuffer y cierreautomático13,203s/retorno0. Renderer/bootPASS, RenderError y
Critical0; logglobal tiene1084Input+3Core metadatosNRO+3BSD, ruidoheadless conocido
yarchivado sin ocultar. No gameplay/FPS ni gatevisualmanual. Appx no importa en
pwsh7 de esta shell; repetir scripts con WindowsPowerShell5/powershell.exe.
DiseñoASTC actualizado, paqueteSeries0.2.71.0 separado ysincommit. Sinapp viva.


ASTC guest experimental2oct: usuario autoriza integración y Game D con Job5120.
Gate PC integrado debug pasa60717+22369 consultas anteriores y7168 por etapa
compute/fragment,7mips/8capas/GOB0--3/SRV RGBA nulo,0discrepancias. astc_guest_sampling=1
opt-in;float2D/arrays PS/CS, fallback nativo para operaciones incompatibles. Conserva
padding guest y buffers committed: no hereda x1,096 del gate agrupado. Cuenta
raw/metadatos/compañeras ybaseline contrafactual, no ahorro total ni Series.
Smoke default retorna0/Render0. Importador omite BuiltInWorkgroupSize dummy que
sustituía LocalSize. Primer Game D abortado antesRun:13256pipelines antiguas alcanzan
cap5120 al recompilar; experimento ahora omite solo precarga shader y compila a
demanda. RelanzadoPID19896/play1,Job5120 verificado,prewarm115/staging256/T480 intactos,
monitor RAM activo. Calidadguest general/LOD implícito/gameplay/Q/Series pendientes.
Sincommit; fuentes,diseño,evidencia y límites en docs/xbox_astc_sampling.md.
Cierre ASTC guest2oct: última Game D PID9520/Job5120/play1 duró348,782s, salida menú
volver biblioteca, retorno10 (noQ/0), RenderError/Critical0. Array de carga raw40,6875
frente650,375MiB nativo contrafactual evitó609,6875MiB; tras su GC predominó fallback
con raw+compañera:1--1,4375MiB EXTRA en recursos vivos. No ahorro RAM total demostrado,
sin A/B/Series/FPS/visual certificados. Diag muestra hasta5063MiB commit; monitor
externo incompleto tras1muestra, no pico global. Perfil shader experimental acotado,
<=4samples/fetch y margen1GiB al preparar; fallback preservado. Prewarm115/staging256/
Job5120/T480 intactos. Corrige propiedad descriptor/compañera al mover Image y
File.Replace PowerShell NullString; build incremental pasa, smoke normal debug
retorno0/Render0, monitor22muestras/182lecturas concurrentes y running=false final.
Último cambio ownership aún sin nuevo Game D; sinapp viva/sincommit. Estas notas
sustituyen estado previo PID19896 pendiente. Informe completo y evidencia en
`docs/xbox_astc_sampling.md`, `build-uwp/astc-game-d-results/20261002-222748-bounded/`.
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
Comparación ASTC solicitada2oct sin cap: predeterminada peakcommit7341,371MiB vs
primer prototipo7658,629 (+317,258/+4,32% observado), no ahorroRAMtotal. Últimas
ventanas27,86/29,99 vs12,68/13,81FPS; prototipo258,761s pipeline stalls/386,323s
ventanas, duración/caches/compilación impiden atribuir causalidad a ASTC puro.
Último conjuntoASTC raw+compañera49MiB extra vsbaseline, ahorro609,6875 transitorio
no sostenido. DefaultQ/retorno0; prototipo sinQ/0confirmado. RenderError0 en ambos,
un DebugCriticalFSA escrituraactual0 en cada uno. Repeticiónprecarga128pares/256PSOs,
computeademanda, falla gatecaliente por stalls persistentes; registrado como fallo
porusuario. No appviva ni nuevos límites/cambiosrenderer/commit. Evidencia
astc-ab/comparison-observed.json y detalle en docs/xbox_astc_sampling.md.
Velocidad ASTC Directo3oct: pipelines sin decoder importado recuperan4workers si
AppFree>=4GiB; decoder importado/bajo margen siguen serializados por memoria.
Gate referencia384,633->135,798ms(2,832x), DXIL idéntico; no es mejora decode/FPS.
Cache/pesos parciales/quad2x2/mip innecesario/constantes dieron mejoras puntuales
pero regresiones GPU o compilación1,16->3,8s: rechazados, decoder original retenido.
Probe final-safe/20261003-001213-debug pasa CS/PS23552queries extra por etapa,
60717aisladas/22369nativas/7168guest,32benches+4constantes,Render0/retorno0/Job5120.
Corregido lifetime de recursos al cambiar corpus (anteserror921/devicehung en
constant-before); copias terminan antes de reemplazar. Timings separan translate/
sign de PSO; labelrgba_native eshelper importado enGuestIntegration. Shaderrestore
requieremtimeactualizado paraNinja; after8stale no certifica variante. Sin ahorroRAM
nuevo ni mejora decode general/Series certificada; sincommit. Detalle y resultados
en docs/xbox_astc_sampling.md, conservaJIT115/staging256 yA/B sinlímite anterior.
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
ROMs externas3oct: usuario rechaza APPX22GB con Game D/Game G. Investigación
primariaDolphinUWP(pickers/ruta),PPSSPP(pickers/FutureAccessList/StorageManager),
RetroArch(VFSFromApp) yMicrosoftdocumentada en docs/xbox_rom_storage.md.
Distribuciónpropuesta appseparada, internaLocalState/games yaenumerada yUSBNTFS
seleccionado/recordado sincopiar. ActualSeedUserData duplica20,132GiBjuegos en
primera instalación; no confundirSSD/RAM. Coreusa_wfopen: picker/tokenno certifica
acceso porruta; requeriráadapterFromApp/streamVFS ygateoffsets>4GiB/Series/permisos.
MicrosoftbroadFileSystemAccessnoXbox; no dependerdesktop-onlyhandleinterop.
SinimplementaciónUSB, sinpaquetenuevo/commit/push; artefacto0.2.72conservado.

ROMs externas implementadas3oct,0.2.73.0: FolderPicker asíncrono enCoreWindow,
registro/scanWinRT enworkerMTA,FutureAccessList persistente16fuentes. Catálogo
interno+externo conidentidad eden-usb:/token/relative,metadatos/boot porVFSUWP.
FromAppread-onlyHANDLE/64bits yfallbackIRandomAccessStream conchunk/scratch1MiB,
mutexporarchivo yMTAporTLSparaworkersdelcore. No cambiarIOFile/globalcore:
adaptadorVfsFilefrontend evitaCRT/FILE* y mantieneRealVfs paraLocalState.
Preflight IO previoalguest yLoadfailed recupera biblioteca conaviso.
Buildincremental ytestsidentidad/scannerPASS;gateAppContainer record/restore
en2procesosPASScatalog72/NROloader/bytesreferencia/cruce4GiB/EOF/256readsconcurrentes/
readonly/traversal/tokennulo/reauthorización. Fixturesparse4297064705B(~2MiBescritos),
retorno0/~1s/commit25MiB,Job5120 aplicado. Evidencia build-uwp/rom-storage-gate.
Trampas: paginación requiereStorage.Search+DefaultQuery;AccessListEntry campos
Token/Metadata;testC++ suelto desktopx64/MD (UWP no tiene libcpmt paraMT).
APPXfirmado13848490B sinROMs/keys/firmware/fixtures enpackage-series-usb-0.2.73.0,
configASTCDirecto/115core/5120/libraryplay1intacta,símbolos0.2.73.0/hash/layout
verificados. Paquete0.2.72gigante conservado. No instalaciónSeries ni gateUSB
físico/pickerexterno/ACL/disconnect/gameplay certificados; detalles ypasos en
docs/xbox_rom_storage.md. Sincommit/push;renderer sin cambios por esta tarea.
Biblioteca normalPC relanzadaPID22564/Job5120/~46MiBworking-set, sininputs ni
limitetimegameplay;visual/picker manual pendientes. ParaAdd-AppxPackage usar
System32\WindowsPowerShell\v1.0\powershell.exe:hostPS7rechazaAppx0x80131539.
Series Game D0.2.73/USB3oct: Load0/Run sí pasan; abort+144,390s porruntime_error
Create committed texture fallback0x8007000E trasCreateHeapOOM. ASTCshader5/13calls
superacap4; a142,494s crea compañera650,4375MiB paraarray1024/121capas/raw40,6875MiB,
luego guardmemoria conserva shadersnativos. Últimoheartbeat3994/5120MiB antes
ese salto, no pico fatal. Siguientearray512/121 raw10,44MiB/baseline166,4375MiB
precedeOOM; no se confirmó creación de esa segunda compañera. Excepción sale de
hilo1648 yabort: gateSeriesfallido, no fallo inicialROM/USB ni×1general demostrado.
Evidencia astc-series-results/20261003-0.2.73-game-d-oom; diagnóstico detallado en
xbox_astc_sampling.md. Pendiente cobertura/fallbackmemoria/manejoOOM sincrash;
este turno solo diagnóstico, sin cambiosruntime/paquete/commit.
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