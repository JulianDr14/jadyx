# Packaging and deploying the headless boot appx to an Xbox Series X|S

This is the step that turns the UWP build from [`docs/xbox/uwp_build.md`](uwp_build.md) into something
that actually runs on a console. It covers **GATE 2**: proving Eden executed guest code through the
dynarmic JIT inside the Xbox AppContainer.

> **Current state:** the renderer executes guest D3D12 workloads and UWP uses native XAudio2 2.9
> output. The `boot_nro` still proves JIT and graphics only; it does not open AudioOut. Real audio
> is therefore gated with a game after the NRO returns 0. See [`xbox_audio.md`](xbox_audio.md).

## What you need

**On the console**
- An Xbox Series X|S with **Dev Mode** activated (the *Xbox Dev Mode Activation* app, which requires
  a Partner Center developer account).
- Device Portal reachable at `https://<xbox-ip>:11443`.

**On the PC**
- Everything in [`docs/xbox/uwp_build.md`](uwp_build.md) (VS 2022 + the **C++ (v143) UWP tools**
  component, a Windows 10/11 SDK, CMake, Ninja, native perl + nasm, vswhere on PATH).
- The Windows SDK also supplies `MakeAppx.exe` and `SignTool.exe`, which the packaging script finds
  on its own.

**The payload**
- **devkitPro** with the `switch-dev` package, to build the `boot.nro` in
  [`tools/xbox/boot_nro/`](../../tools/xbox/boot_nro). That is ordinary homebrew, built from source
  here: no keys, no firmware, no commercial ROMs — house rule, and nothing it calls needs them.

Build the payload from that directory, in **PowerShell or cmd** (not a Git Bash shell — msys2's
make resolves paths differently there and the compile fails):

```
C:\devkitPro\msys2\usr\bin\make.exe
```

It runs a loop, checks the result, and emits `JADYX_JIT_ALIVE` only if the arithmetic came out
right — a miscompiling JIT reports `JADYX_JIT_MISCOMPILE` instead of silently passing the
gate. Observing the sentinel is what makes GATE 2 positive proof rather than "it didn't crash".

## Build, package, sign

From a **`vcvarsall.bat x64 uwp`** shell with CMake + Ninja on PATH (see the build doc — all three
env steps matter, and `echo %LIB% | findstr x64\store` must match):

```bat
cmake --preset uwp-x64
cmake --build --preset uwp-x64 --target jadyx
```

Then, from PowerShell at the repo root:

```powershell
.\tools\xbox\package-appx.ps1 -BootNro C:\path\to\boot.nro
```

The script stages the layout, packs it, mints (or reuses) a self-signed code-signing certificate
whose subject matches the manifest's `Publisher`, signs the package, and exports the `.cer`:

```
build-uwp\package\jadyx.appx
build-uwp\package\jadyx.cer
build-uwp\package\Microsoft.VCLibs.x64.14.00.appx
```

That third file is the **Store CRT framework package**. The exe hard-imports `VCRUNTIME140_APP` /
`MSVCP140_APP`, which live there rather than in our package, so the manifest declares it as a
`PackageDependency` and the console needs it installed. Without it the app fails to *activate*, with
a generic launch error and no crash dump. The script copies it out of the VS Extension SDK, which
means the **"C++ (v143) Universal Windows Platform tools"** component has to be installed — the
base C++ toolset alone links the app fine but does not ship this package.

## Deploy

1. Open `https://<xbox-ip>:11443` and accept the self-signed certificate warning.
2. **Add** → upload `jadyx.appx` and add `Microsoft.VCLibs.x64.14.00.appx` as a
   **dependency package** if a compatible version is not already installed.
   For published Jadyx releases from 0.3.9.0 onward, no separate `.cer` upload is required.
3. Set the app to **Game mode**, not App mode. App mode caps a UWP app at roughly a gigabyte of
   memory; Game mode is what makes the emulated DRAM reservation viable — especially on Series S.
4. Launch it.

### Certificate upload

The user confirms that published releases from 0.3.9.0 do not require uploading a separate
`.cer` in Device Portal. The package remains signed: do not remove signing from the build or
change its identity. The browser warning for Device Portal’s HTTPS certificate is unrelated.
This installation-guide update does not claim new fresh-console test results.

## Audio gate

The default is `audio=xaudio2`: PCM16 stereo at 48 kHz through the system default endpoint. No
extra DLL, microphone capability or redistributable is needed. Optional `boot.cfg` entries are:

```ini
audio=xaudio2
audio_profile=1
```

Use `audio=null` for a timed silent comparison. An unknown `audio=` value writes a warning and
falls back to XAudio2. With profiling enabled, inspect `jadyx_log.txt` for the periodic `XAudio2
profile` line and for any `GlitchesSinceEngineStarted` increase. The console acceptance gate is at
least 15 minutes of continuous game audio over HDMI (and controller headphones when available),
with no repeated pops, distortion, `Critical`, deadlock or frame-pacing regression. If the endpoint
is lost, the game must continue at normal speed using the silent paced fallback.

## Reading the result

The boot writes two things into the app's **LocalFolder** under `jadyx\log`, pullable through the
Device Portal's file explorer:

- **`jadyx_diag.txt`** — coarse startup breadcrumbs written before Eden's logging is up
  (`BootView::Run entered`, the resolved NRO path, `RunHeadlessBoot returned <rc>`, or the exception
  that killed it). This is the file to read when the app flashes and closes.
- **`jadyx_log.txt`** — Eden's own log, once logging initialises. GATE 2 passes on:
  `Headless boot: JIT liveness CONFIRMED ('JADYX_JIT_ALIVE' observed).`

Return codes from `RunHeadlessBoot`: `0` liveness confirmed · `2` the NRO failed to load ·
`3` the app ran but the sentinel never appeared within the 30 s backstop.

## Manifest notes

[`dist/uwp/AppxManifest.xml`](../../dist/uwp/AppxManifest.xml) is deliberately minimal, but two things
in it are load-bearing:

- **`<Capability Name="codeGeneration" />`** — this is what unlocks `VirtualProtectFromApp`,
  `CreateFileMappingFromApp` and `MapViewOfFileFromApp` for the AppContainer. Without it dynarmic
  cannot make JIT pages executable and `common/host_memory.cpp` cannot map the emulated DRAM. Drop
  this line and nothing about the port works.
- **`Identity/@Publisher` must equal the signing certificate's subject** character for character.
  A mismatch is the most common sideload rejection; `package-appx.ps1` checks it before packing.

`EntryPoint="Jadyx.App"` is the conventional moniker for a plain C++/WinRT `CoreApplication` app
(`wWinMain` + `IFrameworkView`, see [`src/jadyx/app/uwp_boot.cpp`](../../src/jadyx/app/uwp_boot.cpp));
there is no activatable WinRT class to name.

## When it does not activate

A UWP app that fails **activation** dies before any of your code runs — no log, no dump, no diag
file. The usual causes, in the order worth checking:

- **A hard import on a DLL the Xbox sandbox does not provide.** `src/jadyx/CMakeLists.txt`
  already delay-loads the known set (WLAN API set, setupapi, hid, winmm, imm32, user32…). A new
  dependency that pulls a desktop-only DLL reintroduces this. Inspect with
  `dumpbin /imports jadyx.exe`.
- **Missing `codeGeneration`**, which fails later, at the first JIT page protect.
- **App mode instead of Game mode** — shows up as an out-of-memory death during DRAM reservation
  rather than at activation.

Para diagnosticar CPU guest en 0.2.68.0 se puede agregar `cpu_profile=1` a BootCfg.
El log informa Run y callbacks por core; Run es tiempo transcurrido e incluye traduccion,
callbacks y preemption, no utilizacion CPU. El perfil esta desactivado por defecto y agrega
trabajo si se activa: comparar rendimiento con la misma configuracion en ambas builds.
Para estudiar las cargas, combinarlo con `gpu_profile=1` y repetir el mismo recorrido.

### Perfilador por muestreo (desde 0.2.75.2)

Es el perfilador de CPU para la consola, donde no hay xperf ni PIX para UWP. Funciona así:

- Pausa cada hilo con nombre 250 veces por segundo, copia su pila y lo reanuda. Cada pausa dura
  unos 20 us, así que a un hilo que trabaja le cuesta menos del 1%.
- La pila se desenrolla después, sobre la copia, con el hilo ya corriendo.
- Los hilos bloqueados se muestrean 32 veces menos.

Va apagado por defecto. Se activa al empaquetar un build de diagnóstico; un release se empaqueta
sin la opción:

```
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library -Profile on
```

- `-Profile on` escribe `profile=1` en `boot.cfg`: solo el muestreo.
- `-Profile full` escribe `profile=full`: además activa `gpu_profile` y `cpu_profile`. Esos
  cronómetros sí frenan el hilo de GPU.
- Con `-BootCfg` se ajustan la frecuencia, `profile_hz=` (10-1000, 250 por defecto), y la
  duración de cada ventana, `profile_window=` (segundos, 20 por defecto).

Cada ventana se añade a `LocalState\jadyx\log\jadyx_profile.txt`. Incluye el tiempo de CPU de cada
hilo y cada pila distinta, con cuántas veces se vio corriendo (R) o esperando (W). El archivo se
vacía al abrir la app; cada juego que se abre desde la biblioteca añade sus ventanas. En el PC,
con el PDB del mismo build:

```
python tools/xbox/profile-report.py jadyx_profile.txt --out informe.md --folded perfil.folded
```

El informe incluye:

- Qué hilos están saturados y en qué subsistema se les va el tiempo.
- Por hilo:
  - las funciones más caras, solas y con lo que llaman;
  - quién llama al sistema y al driver;
  - dónde espera.

`perfil.folded` se abre en https://www.speedscope.app como flame graph. `--window N` o `--window
N-M` limitan el informe a unas ventanas, por ejemplo las de la zona lenta.

### Captura de rendimiento: por qué la GPU está ociosa

Para saber por qué bajan los FPS en una zona pesada con la GPU sin llenar. Se empaqueta con
`package-appx.ps1 -Library -Hunt` (`hunt=1` en `boot.cfg`; no cuesta nada hasta capturar) y sin
`profile=1`: si ese perfilador ya corre, la captura no tiene el suyo.

En la zona lenta: View+Menu 1 s → **Capturar rendimiento y continuar** → A, y seguir jugando sin
moverse unos 7 s. Se puede repetir en varias zonas: cada captura lleva su número. Tras 30 cuadros de
calentamiento, la captura guarda:

- `jadyx_hunt.txt`: 8 cuadros con el tiempo de CPU y de GPU de cada draw, y la cadena de cuadros
  durante 5 s: lo que pidió el juego (swap interval), vsyncs perdidos y las esperas de cada hilo.
- `jadyx_hunt_profile_<N>.txt`: la CPU y las pilas de todos los hilos durante esos mismos 5 s.

Los archivos se bajan con `tools/xbox/fetch-logs.ps1`. En el PC, con el PDB del mismo build:

```
python tools/xbox/hunt-report.py jadyx_hunt.txt --capture N --out informe.md
```

El informe empieza con «Why the GPU sits idle»: una conclusión, el trabajo por cuadro de cada eslabón
(GPU, hilo de GPU, grabador, núcleos guest) y la tabla de esperas. La conclusión indica correlación
en esa ventana, no una causa probada. El hilo de GPU se mide en tiempo real sin sus esperas; los
demás, en tiempo de CPU, así que un hilo expulsado de su núcleo parece más ligero y un núcleo guest
que gira esperando parece ocupado: sus pilas lo aclaran.

Después viene «Guest code»: los bloques del juego que más ejecuta el núcleo guest más ocupado, con su
dirección, cuánto del tiempo se fue en llamadas al emulador y sus instrucciones (desensambladas con
`pip install capstone`). Marca los que parecen una espera: WFE/YIELD, o un bucle corto y cerrado que no
escribe en memoria. Un bucle de sondeo más largo no se detecta solo, así que conviene leer los
primeros bloques. Solo cubre código A64. Con `hunt=1` el JIT anota cada bloque compilado (unos 32 bytes
por bloque, de ahí que no se active sin esa clave). Lo verifican `tools/xbox/tests/hunt-report-chain.py`
y `tools/xbox/tests/sampling-profiler.cpp`.

La misma sección reparte el tiempo en JIT entre los bloques del juego, los thunks de acceso a memoria y
el resto del JIT. Una muestra dentro de un thunk se atribuye al bloque que lo llamó, por la dirección de
retorno en la pila. Ese tiempo incluye esperar los datos del juego, que costaría igual sin thunks.

`jit_thunks=0` en BootCfg emite los accesos a memoria dentro de cada bloque, sin llamada. Ocupan mucho
más: cada acceso son ~85 bytes en vez de 5, en la misma caché de código. Es una clave para comparar; por
defecto siguen los thunks. El diagnóstico indica el modo en `guest memory accesses`.

La presentación usa un hilo propio por defecto. Con `async_present=0` en BootCfg se presenta en
el hilo de GPU, como antes de ese cambio. El log dice qué modo se usa:
`D3D12: presentation on its own thread` o `on the GPU thread`.

Las llamadas a la command list de D3D12 se hacen por defecto en un hilo propio (`D3D12Recorder`):
el hilo de GPU solo las graba, en bloques que ese hilo ejecuta en orden y envía a la cola. Con
`async_record=0` en BootCfg el hilo de GPU las hace él mismo, como antes. También se cambia en
Ajustes > Gráficos y en los gráficos de cada juego, con efecto al iniciar el juego; BootCfg manda
sobre los dos. Apagado, `descriptor_defer` tampoco se usa. El log dice qué modo se usa:
`D3D12: command list calls made on a recording thread` o `on the GPU thread`.

La reutilización del pipeline entre draws está encendida por defecto (`pipeline_key_reuse=0` la
apaga). Un draw reutiliza el pipeline del anterior sin recalcular su clave cuando no cambió ningún
registro que la clave lea, ni el engine, la topología, el hint de macro HLE o los shaders. La lista
de registros que no la afectan está en `d3d12_pipeline_key_tracking.cpp` y la verifica
`tools/xbox/tests/run-pipeline-key-tracking.ps1` contra el refresco real de la clave. Con
`pipeline_key_reuse=check` se recalcula siempre y el log registra cada draw en el que la reutilización
habría dado otro pipeline (`D3D12: pipeline key changed with no tracked register write`); cada perf
report añade `D3D12 pipeline key check: N reusable draws verified, M mismatched`. Con `gpu_profile=1`
el detalle del hilo de GPU cuenta además los `key reuses`.

En consola, el modo check verificó unos 7,3 M draws reutilizables (~74% de los draws) sin ninguna
discrepancia, en una sola zona de un juego. No hay registrada una comparación A/B en el mismo sitio.

El plegado de claves de pipeline está apagado por defecto. Con `pso_fold=on` en BootCfg, las claves
que solo difieren en estado que su pipeline no puede mostrar comparten un pipeline, y al arrancar las
guardadas se pliegan antes de traducirlas. Solo corre cuando una clave falla en las búsquedas rápidas.
Con `pso_fold=check` los pipelines no cambian: el log resume lo que ahorraría (`D3D12: pipeline
cache: N stored keys, M distinct once folded`) y compara los pipelines que uniría
(`D3D12: folding would merge pipelines`). `pso_fold_rules=<máscara hex>` elige las reglas, en el orden
de `PipelineKeyFold::Rule`. Lo verifica `tools/xbox/tests/run-pipeline-key-fold.ps1`.

Sigue apagado porque rinde poco y falta una duda por resolver. Una simulación sobre las cachés de 26
juegos da una mediana de ~6 % menos de compilaciones del driver, porque la caché de PSOs ya deduplica
casi todo. En consola, `pso_fold=check` en un juego dio 78 de 879 pares con shaders distintos, todos
con el shader en otra dirección de memoria del juego, y no se sabe por qué difieren. `pso_fold=on` no
se ha probado en consola.

El hilo `HostTiming` duerme hasta poco antes de cada evento con un timer de alta resolución y
solo espera en la CPU el retraso que ese timer suele tener (decenas de microsegundos). Con
`timer_sleep=0` en BootCfg vuelve a dormir por ticks del timer del sistema y a esperar en la CPU
el último tick entero, como antes: más preciso, pero ocupa casi un núcleo. El log dice qué modo
se usa: `Host clock: ..., sleeps: high-resolution timer` u `OS timer ticks`.

En la Series ese timer llega hasta ~0,7 ms tarde, así que el margen aprendido se queda cerca de
1 ms y, como casi todas las esperas duran menos, `HostTiming` no duerme nunca: ocupa un núcleo
entero en MWAITX. `timer_spin=<us>` limita la espera en la CPU a esos microsegundos; si el timer
llega más tarde, el evento se atrasa. Sin la opción no cambia nada. El arranque escribe en
jadyx_diag.txt `step: CPU ...` con los procesadores lógicos, los CPU sets y cuáles tiene la app.

En la Series la app ve 8 procesadores lógicos en 4 núcleos con SMT y tiene 6: los dos hilos de
los núcleos 2, 4 y 6. El núcleo 0 queda para el sistema. Con `timer_spin=100` (0.3.8.70, mismo
sitio que 0.3.8.68 con `cs_split=16`):

| | 0.3.8.68 | 0.3.8.70 `timer_spin=100` |
|---|---|---|
| CPU de `HostTiming` | 102% de un núcleo | ~0% |
| FPS tras la carga | 22,0 · 21,7 · 25,0 · 24,6 | 25,5 · 26,9 · 26,7 · 26,0 |
| p99 tras la carga | 134 · 70 · 97 · 60 ms | 84 · 64 · 68 · 48 ms |

El precio: las esperas terminan en promedio unos 0,2–0,4 ms después de lo pedido, con picos de
3,8 ms. La primera ventana, la de la carga, va lenta en todas las versiones.
