# Xbox: como aprovechar la GPU de la Series

Guia practica para sacar rendimiento de la GPU de la Series X|S desde el backend D3D12. Parte de
un caso real (el compute shader `8ad2228ecae48414`) y termina con reglas generales y la forma
de medir cada cambio en consola.

## La maquina

Series X: GPU RDNA 2 personalizada, 52 CU a 1,825 GHz fijos (sin boost), unos 12 TFLOPS.
Memoria unificada de 16 GB: 10 GB a 560 GB/s y 6 GB a 336 GB/s. La Series S tiene menos CU y
menos ancho de banda; todo lo de abajo aplica igual, con numeros mas chicos.

Lo que importa de RDNA 2 para nosotros:

- Cada CU tiene dos SIMD32; dos CU forman un WGP. En la Series X son 104 SIMD.
- Cada SIMD admite hasta 16 waves residentes. La ocupacion es waves asignadas / 16 por SIMD.
- El driver elige wave32 o wave64 por shader. Un grupo de 64 hilos es una wave64 o dos wave32.
  Wave64 ocupa el doble de registros por wave.
- Las latencias (memoria, atomicos que devuelven valor, texturas) solo se esconden cambiando a
  otra wave lista. Con una o dos waves por SIMD no hay a quien cambiar y la SIMD espera.
- Los registros (VGPR) limitan cuantas waves caben: 1024 registros fisicos por SIMD32, hasta 256
  por wave, y wave64 cuenta doble.

En corto: la GPU es ancha. Rinde cuando tiene cientos o miles de waves en vuelo; un dispatch
chico y largo la deja casi vacia, por rapido que sea cada hilo.

## Caso de estudio: CS 8ad2228ecae48414

### Forma del shader

Leido del IR (`dump_shader=8ad2228ecae48414`) y del DXIL desensamblado:

- Grupos de 64x1x1, sin memoria compartida ni barreras.
- Cada grupo procesa un registro de trabajo. Un bucle de stride arranca en el local id, avanza de
  64 en 64 y termina en `count` (palabra +108 del registro). Lo protege `if (!(lid >= count))`.
- Todas las escrituras (stores y atomicos) estan dentro de ese bucle, salvo una cola:
  `if (!(lid == 0 && count == 0)) return;` y luego trabajo de un solo hilo (atomicos en el
  registro +0 y un bucle serie sobre las entradas de +12). La cola solo corre en grupos vacios.
- Cada iteracion hace unos 13 atomicos que devuelven valor, encadenados: cada uno espera el
  resultado del anterior.

### Que medimos

Con `hunt=1` el dispatch dura 77--84 ms (lineas `r 5 ... 8ad2228ecae48414 26 1 0 ... <ns>`). Se
lanzan 26 grupos y `cs_inputs=` mostro que el grupo mas largo hace cientos de iteraciones por
hilo. Sale a unos 5,5 us por iteracion, unos 770 ciclos por atomico: es la ida y vuelta completa
de un atomico a L2, sin nada que la tape.

26 grupos son entre 26 y 52 waves para 104 SIMD: menos de una wave por SIMD, 1--3 % de
ocupacion. La GPU no esta calculando, esta esperando atomicos uno detras de otro. Optimizar las
instrucciones del cuerpo no ayuda; hace falta mas trabajo independiente en vuelo.

### La solucion: dividir el grupo (`cs_split=`)

`WorkgroupSplitPass` (`src/shader_recompiler/ir_opt/workgroup_split_pass.cpp`) convierte cada
grupo de ancho W en `f` grupos reales que actuan como un grupo virtual de ancho W*f:

- workgroup id x pasa a ser `gid >> log2(f)`;
- local id x pasa a ser `lid + (gid & (f - 1)) * W`;
- el bucle avanza `W * f` en vez de `W`;
- el rasterizer lanza `dim[0] * f` grupos (`Dispatch(dim[0] * pipeline->WorkgroupSplit(), ...)`).

Cada item lo procesa el mismo hilo virtual que antes, asi que el resultado no cambia, pero los
items de un registro se reparten en `f` veces mas waves. Con `f = 8` los 26 grupos pasan a 208,
de 208 a 416 waves: 2--4 por SIMD y las latencias de los atomicos empiezan a solaparse.

El juego sigue viendo el mismo numero de grupos; solo cambia como los ejecuta la GPU.

### Cuando es seguro

El pase solo transforma programas cuyo resultado no puede depender del ancho del grupo. Si algo
no cuadra deja el shader intacto y el log dice por que (`not split (...)`):

- local size solo en X, sin memoria compartida ni barreras de grupo (`SharedMemory`, `Barrier`);
- cada uso del local id es el arranque de un bucle de stride o una comparacion con su limite
  (`NoStrideLoop`, `BadStrideLoop`, `NoGuard`, `OtherLocalUse`, `PackedId`);
- un chequeo de lider (`lid == 0`) solo se acepta como `lid == 0 && count == 0`. A solas, el
  lider podria terminar antes que las otras partes de su grupo y romper el clasico contador de
  "ultimo grupo terminado"; con el limite en 0 ningun trozo tiene trabajo con el que competir;
- toda escritura (store, atomico, escritura de imagen) debe estar dentro de un bucle de stride o
  en bloques que solo se alcanzan pasando por ese chequeo de lider vacio (`UnconfinedWrite`). Si
  no, los hilos extra la repetirian.
- las operaciones de subgrupo mantienen su sentido: un grupo real sigue siendo una o mas waves
  enteras.

Tests: `tools/xbox/tests/run-demote-helper.ps1 -Fixture workgroup-split` (19 casos: divisiones
validas, lideres aceptados y rechazados, escrituras fuera del bucle).

### Error que costo una version

En 0.3.8.62 la division no tuvo efecto. Tras el primer dispatch, `EmulateReduction` cambia el
shader a su variante que emula los filtros MIN/MAX, y esa variante nunca se dividia: el DXIL de
la .62 era identico al de la .64 sin dividir. Ahora la division se calcula siempre y existen
cuatro caches (`compute_cache`, `compute_split_cache`, `compute_reduction_cache`,
`compute_reduction_split_cache`). Leccion: verificar en el DXIL volcado que el pipeline que corre
de verdad es el modificado, no solo el primero que se compilo.

## Como medir en consola

| Opcion de boot.cfg | Que da |
|---|---|
| `hunt=1` | Tiempo GPU por draw/dispatch, blits 2D, copias DMA y colas de command list. `tools/xbox/hunt-report.py` lo resume y dice si el frame esta limitado por GPU. |
| `cs_inputs=<hash>` | Vuelca los buffers de un compute shader antes y despues del dispatch. `tools/xbox/cs-inputs-report.py LOG_DIR --hash H --jobs` muestra la lista de trabajos y las iteraciones por grupo. |
| `dump_shader=<hash>` | IR y DXIL del shader, incluidos compute. Sirve para leer offsets y confirmar que la variante esperada es la que corre. |
| `cs_split=2\|4\|8\|16` | Factor de division para los compute que la admitan. El log dice `split: each group of 64 runs as N` o el motivo del rechazo. |
| `min_max_filter=off` | Salta la emulacion de filtros MIN/MAX, para aislar su coste. |

Flujo: paquete con la opcion, partida hasta el punto, `tools/xbox/fetch-logs.ps1 -OutDir <dir>`,
comparar los ns del dispatch en `hunt` contra la base. Un cambio cuenta solo con A/B en el mismo
punto del juego.

## Reglas generales para la Series

1. **Primero ocupacion, despues instrucciones.** Si un dispatch tiene menos grupos que CU, es
   casi seguro que espera latencia. Repartir trabajo (dividir grupos, mas hilos por item) gana
   mas que micro-optimizar.
2. **Los atomicos que devuelven valor son caros y en serie.** Cientos de ciclos cada uno. Solo se
   pagan bien con muchas waves en vuelo, y solo si van a direcciones distintas: a una misma
   direccion L2 los atiende de uno en uno. Uno sin valor de retorno no bloquea a la wave. Antes
   de agregarlos en el host, mirar el IR: muchos juegos ya los agregan por warp con un ballot.
3. **Cuidar los registros.** Un shader con muchos VGPR baja las waves por SIMD. Variantes que
   agregan codigo (emulaciones, rutas de fallback) pueden costar ocupacion aunque no se ejecuten.
4. **Evitar esperas CPU-GPU.** Lecturas de GPU a CPU, fences en mitad del frame y descargas de
   recursos vacian la cola y dejan la GPU parada; son las siguientes pistas para pasar de 30 fps.
5. **Preferir compilar al arrancar.** Un arranque mas largo es aceptable si en juego hay menos
   tirones de shader/PSO.
6. **Sin fastmem.** En la Series se queda sin memoria; no forma parte de ninguna ruta de
   rendimiento.
7. **Medir en la Series, no en PC.** El PC tiene otra GPU, otro driver y otra eleccion de
   wave32/64; sus numeros no predicen la consola.

### Resultado en 0.3.8.67

Mismo punto del juego, ventana de 300 frames presentados:

| Version | FPS | p50 | p95 |
|---|---|---|---|
| 0.3.8.66 (division sin aplicar a la variante MIN/MAX) | 7,47 | 108,27 ms | 232,32 ms |
| 0.3.8.67 (`cs_split=8` en todas las variantes) | 13,89 | 43,48 ms | 191,49 ms |

El log confirma `split: each group of 64 runs as 8` para `8ad2228ecae48414` (las dos variantes),
`e11a6ece0bcbdf41` y `224f0d72dd436dde`. El IR volcado muestra `gid >> 3`, `& 7` y paso 512.

Trampa de medicion: `jadyx_hunt.txt` y los `cs_inputs_*.bin` solo se reescriben cuando se toma
una captura nueva. Los que bajo la .67 eran identicos byte a byte a los de la .66 (mismo md5), asi
que no median la version nueva. Antes de comparar tiempos de `hunt`, confirmar que el archivo
cambio (md5 o numero de records) frente a la descarga anterior.

### Resultado en 0.3.8.68 (`cs_split=16`)

Primera captura `hunt` valida con la division activa (md5 distinto al de la .67):

| | sin division (captura de la .66) | `cs_split=16` (.68) |
|---|---|---|
| Dispatch de `8ad2228` (GPU) | 80--85 ms | 2,5--6,3 ms |
| Dispatches por frame (GPU) | 82,42 ms | 4,52 ms |
| GPU ocupada por frame | 91,19 ms (80 %) | 12,83 ms (29 %) |
| Frame en la captura | 113,42 ms | 43,77 ms |

Ritmo de presentacion en la .68, ventanas de 300 frames: la primera, con la carga, da 11,85 FPS;
las siguientes 22,02, 21,71, 24,97 y 24,63 FPS (p50 36--40 ms, p95 47--81 ms). La .67 solo tuvo
una ventana, la de la carga (13,89 FPS), asi que no se puede decir aun si 16 gana a 8.

El dispatch dejo de ser el cuello: la GPU pasa el 71 % del frame libre y `hunt-report.py` ya no
marca el frame como limitado por GPU. Lo siguiente esta del lado CPU (hilo de GPU grabando
llamadas y emulacion del juego).

### A/B `cs_split=8` (.69) contra `cs_split=16` (.68)

Mismo punto, emulacion de MIN/MAX activa en ambas (el log dice `emulating it in the shader`).
Ventanas despues de la carga:

| Factor | FPS por ventana | p50 |
|---|---|---|
| 16 | 22,02 / 21,71 / 24,97 / 24,63 | 36--40 ms |
| 8 | 22,98 / 24,67 / 24,16 | 35--41 ms |

En FPS empatan: el frame ya no lo limita ese dispatch. En GPU 16 rinde mas: 2,5--6,5 ms por
dispatch contra 5--13 ms con 8 (las capturas no son del mismo instante, 680 contra 309 draws
indexados por frame, asi que es orientativo). Se queda `cs_split=16`.

## Pendiente

- Buscar el cuello nuevo en CPU: perfil del hilo de GPU (texture bindings, descriptores,
  buffers) y del hilo del juego.
- Agregacion de atomicos por wave: descartada para este shader. El IR muestra que el juego ya
  agrega por warp: `SubgroupBallot #true`, lider = `FindUMsb(ballot) == LaneId`, y solo el lider
  hace `StorageAtomicIAdd32` con `BitCount(ballot)`; los atomicos sin retorno del cbuf +864 suman
  tambien conteos de ballot. Ya es un atomico por cada 32 hilos. El backend emula el warp de 32
  dentro de una wave64 (`WarpExtract`), asi que agregar en el host juntaria como mucho dos warps:
  2x en wave64 y nada en wave32. El coste que queda es la cadena de atomicos con retorno del
  lider, y eso se ataca con ocupacion (`cs_split`), no con menos atomicos.

## Fuentes

- [Occupancy explained (GPUOpen)](https://gpuopen.com/learn/occupancy-explained/)
- [RDNA Architecture (AMD, GPUOpen)](https://gpuopen.com/download/RDNA_Architecture_public.pdf)
- [Xbox Series X specs (Guru3D)](https://guru3d.com/story/xbox-series-x-specs-released-by-microsoft)
