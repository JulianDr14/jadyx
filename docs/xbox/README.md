# Xbox Series X|S: documentación del fork

- [Integración de insignias de compatibilidad](nxbox-integration-notes.md): conflictos,
  pruebas host y gate de consola pendiente para la integración con FTP/USB/resolución/shaders.

Port UWP de Eden con backend Direct3D 12, compartido con las pruebas en PC.
La rama de trabajo es `xbox`; las reglas del proyecto están en [AGENTS.md](../../AGENTS.md).

## Arranque y despliegue

El instalador vigente se entrega siempre en `build-uwp/package/jadyx.appx`.
No crear una carpeta de paquetes por compilación. Las rutas `package-*` citadas
en documentación histórica ya no son carpetas de entrega vigentes.

- [Cuaderno del proyecto](xbox_internal.md): estado, decisiones, trampas y resultados de pruebas.
- [Compilación UWP](uwp_build.md): toolchain y configuración inicial; conserva el contexto histórico de la fase 2.
- [Despliegue en Series](xbox_deploy.md): empaquetado, instalación y diagnóstico de activación.

- [Investigación de activación 0x80070002](nxbox-launch-80070002.md): comparación 0.3.8.0/0.3.8.5 y comprobación de dependencias al empaquetar; causa Series pendiente.

## Diseño y funciones

- [Curated compatibility freshness](nxbox-jx-19-compat-fresh.md): background refresh, timestamp selection and manual Bun curation without a new release.

- [Compatibility badges](nxbox-compat-badges.md): report metadata, offline fallback, opt-in refresh and console test.

- [Actualizador de Jadyx](xbox_updater.md): versiones firmadas, descarga reanudable, pantalla de actualizaciones y asistencia web; instalación interna Xbox pendiente de gate.

- [Pulse: sesiones persistentes](xbox_pulse.md): guarda y reanuda sesiones desde la biblioteca; juegos comerciales de 64 y 32 bits probados en la Series y en PC.

- [Direct3D 12, fase 3](xbox_d3d12_phase3.md): scheduler, recursos, cachés y fences.
- [Direct3D 12, fase 4](xbox_d3d12_phase4.md): pipelines y shaders del guest.
- [Audio XAudio2](xbox_audio.md): salida de audio para PC y Series.
- [Actualizaciones y DLC](xbox_content.md): almacenamiento, selección y recorrido de importación.
- [Game information in Menu options](nxbox-jx-1a-game-info-menu.md): read-only metadata, community report and existing removal confirmation; host evidence and pending console gate.
- [Biblioteca e interfaz](xbox_frontend.md): carátulas, navegación, mandos y teclado.
- [Contratos compartidos del frontend](xbox_frontend_refactor.md): catálogo de mandos, acciones, captura modal y listas; migración y gates.
- [Descubrimiento USB y juego sin copia](nxbox-usb-play-in-place.md): detección automática opt-in, VFS existente y gate de consola pendiente.
- [Carpetas y almacenamiento USB](xbox_rom_storage.md): acceso UWP persistente y apertura de juegos sin copiarlos.
- [Portal local de importación](xbox_network_portal.md): exportación completa de logs/capturas en tar.zst con descarga reanudable (.16, AppContainer PC correcto); NSP real validado por HTTP en AppContainer; claves/firmware ZIP y sueltos con revisión de versiones en 0.3.4.3, gates PC correctos y prueba Series pendiente.

- [FTP server](nxbox-ftp-server.md): large local transfers from the library, folder mapping, host checks and pending console gate.
- [Themes on integration](nxbox-jx-18-themes-on-integration.md): merge resolutions, host results and pending console gate.
- [Jadyx and Old NXbox themes](nxbox-themes.md): tokens, live preference, host gates and console comparison.

## Rendimiento y evidencia

- [Parallel shader experiment (jx-70)](nxbox-parallel-shaders.md): worker/signing audit, opt-in three-worker policy and pending Series A/B gate.

- [Operaciones Fermi2D](xbox_fermi2d.md): diagnóstico 0.3.4.9 para identificar operación, ROP, superficies y ruta antes de corregir efectos.

- [Rendimiento](xbox_performance.md): perfiles, optimizaciones y prioridades pendientes.
- [Preparación y reutilización de PSO](xbox_pipeline_preparation.md): identidad host, trabajo único, residencia por fence y gates Game A.
- [Preparación CPU de draws](xbox_draw_preparation.md): baseline Game D, metadata, samplers y subasignación de uniform buffers.
- [Memoria de texturas](xbox_texture_memory.md): packing, readbacks, staging y comparación con Vulkan.
- [Copias de bloques BC](xbox_bc_block_copy.md): preservación exacta de arrays decodificados y reinterpretación BC3/UINT128.
- [Dimensiones de SRV](xbox_srv_dimensions.md): compatibilidad de cubemaps, fallback nulo y propiedad de descriptores.
- [Líneas en el terreno](xbox_terrain_seams.md): dimensiones virtuales BC corregidas; desaparición de líneas confirmada por el usuario en Game A PC y Series; diagnóstico F7.
- [Iluminación y tubos de Game F](xbox_deferred_lighting.md): cerrado en Series; iluminación confirmada en .44 y ambos tipos de tubo en .51, con comparación exacta PC/Series. Release 0.2.88.0.
- [Revisión de logs del 30 de septiembre de 2026](xbox_log_review_2026-09-30.md).

Los resultados históricos conservan sus límites y pendientes. Un build correcto o
un arranque en PC no certifica gameplay, rendimiento ni funcionamiento en Series.

- [RTV duplicados e inactivos](xbox_rtv_bindings.md): binding por máscara, PSO, MSAA y alpha-to-coverage.

- [Geometry shader passthrough](xbox_geometry_passthrough.md): Game B, lowering, interfaces SPIR-V/DXIL y gates de componentes.

- [Magnesis PC/Series](xbox_magnesis.md): antecedentes resueltos y plan de captura diferencial del fallo de enganche exclusivo de Series.

- [Experimental resolution options from NXbox](nxbox-resolution-options.md): global/per-game
  scale, D3D12 coverage limits, host checks and 3D World/BotW console protocol.
- [Opt-in controller preference and synthetic motion](nxbox-37-controllers-motion.md): NXbox input port, host evidence and pending Series acceptance.

- [Read-only on-console Files browser](nxbox-jx-20-files-browser.md): LocalState and authorized USB folders, bounded pages and host evidence; Series gate pending.
- [Optional startup warmup controls](nxbox-jx-22-warmup-skip.md): safe shader/JIT skip, return to library, host gates and pending Series acceptance.
